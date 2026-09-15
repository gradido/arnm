#include "arnm/arena.h"
#include "arnm/key_map.h"
#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * What arnm_key_map costs per key at three sizes -- a table that fits the L2 cache, one that fits
 * L3, and one that does not -- for the two things a map is asked: a new key, and a key it has.
 *
 * Inserting is timed three ways, because where the memory comes from changes the answer: growing
 * from empty behind an arena (every doubling leaves the old table stranded there), the same after
 * a reserve (no doubling at all), and growing on the host, where a superseded table goes back.
 * The arena rows print what the arena handed out beside them, which is the figure the reserve
 * exists to change.
 *
 * Lookups come in three kinds: a find that hits, a find that misses, and get_or_insert on a key
 * that is already there -- the call an index makes for every address of every transaction, and
 * the one that has to cost no more than a find. The keys of a hit are asked in shuffled order,
 * so the table is read the way an index reads it rather than in the order it was filled.
 *
 * Every row runs twice, once on each hash, so what SipHash-1-3 costs over the fast mix is read
 * off side by side. The rows marked "generated" run the same work through the calls
 * ARNM_KEY_MAP_DEFINE(pubkey_map, 32) generates, where the key size is a constant -- the
 * difference to the rows above them is what the macro is for.
 *
 * Keys are 32 random bytes, the size of the public keys the first consumer maps. Random keys
 * are the fair case for the hash and the only case this file measures; keys that share a
 * prefix or a suffix are test_key_map's to check, where the question is correctness and probe
 * length rather than nanoseconds.
 */

#define KEY_SIZE 32u
#define KEYS_BUCKET_LOG2 12u
#define HASH_K0 0x243f6a8885a308d3ULL
#define HASH_K1 0x13198a2e03707344ULL

/* the fixed size copy of the lookup, for the "generated" rows */
ARNM_KEY_MAP_DEFINE(pubkey_map, KEY_SIZE)
#define ARENA_CAPACITY (512u * 1024u * 1024u)

static uint8_t *g_keys;       /* key_count keys that go in */
static uint8_t *g_misses;     /* key_count keys that never do */
static uint32_t *g_hit_order; /* a shuffled permutation of 0..key_count-1 */
static uint32_t g_key_count;
static arnm g_arena;
static arnm g_lookup_arena;
static arnm_key_map g_filled_fast;  /* every key of the current size, fast hash */
static arnm_key_map g_filled_keyed; /* the same keys, SipHash-1-3 */
static arnm_key_map *g_filled;      /* the one the lookup rows read */
static bool g_keyed;                /* which init the insert rows open their maps with */
static volatile uint64_t g_sink;    /* keeps the loops from being folded away */

static uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static void fill_random(uint8_t *bytes, uint32_t count, uint64_t *state) {
  for (uint32_t i = 0; i < count; i += 8u) {
    const uint64_t word = splitmix64(state);
    memcpy(bytes + i, &word, 8);
  }
}

static bool require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS == result) { return true; }
  fprintf(stderr, "benchmark setup failed: %s answered %d\n", what, (int)result);
  exit(EXIT_FAILURE);
}

static arnm_result open_map(arnm_key_map *map, arnm *allocator) {
  if (g_keyed) {
    return arnm_key_map_init_keyed(map, KEY_SIZE, KEYS_BUCKET_LOG2, HASH_K0, HASH_K1, allocator);
  }
  return arnm_key_map_init(map, KEY_SIZE, KEYS_BUCKET_LOG2, allocator);
}

static void prepare_keys(uint32_t key_count) {
  uint64_t state = 0x6b65795f6d6170ULL ^ key_count;
  g_key_count = key_count;
  g_keys = (uint8_t *)malloc((size_t)key_count * KEY_SIZE);
  g_misses = (uint8_t *)malloc((size_t)key_count * KEY_SIZE);
  g_hit_order = (uint32_t *)malloc((size_t)key_count * sizeof(uint32_t));
  if (!g_keys || !g_misses || !g_hit_order) {
    fprintf(stderr, "benchmark setup failed: out of host memory\n");
    exit(EXIT_FAILURE);
  }
  fill_random(g_keys, key_count * KEY_SIZE, &state);
  fill_random(g_misses, key_count * KEY_SIZE, &state);
  for (uint32_t i = 0; i < key_count; ++i) { g_hit_order[i] = i; }
  for (uint32_t i = key_count; i > 1u; --i) {
    const uint32_t j = (uint32_t)(splitmix64(&state) % i);
    const uint32_t swap = g_hit_order[i - 1u];
    g_hit_order[i - 1u] = g_hit_order[j];
    g_hit_order[j] = swap;
  }

  for (int keyed = 0; keyed < 2; ++keyed) {
    g_keyed = keyed;
    arnm_key_map *map = keyed ? &g_filled_keyed : &g_filled_fast;
    require_ok(open_map(map, &g_lookup_arena), "init");
    for (uint32_t i = 0; i < key_count; ++i) {
      uint32_t id = 0;
      require_ok(
          arnm_key_map_get_or_insert(map, g_keys + (size_t)i * KEY_SIZE, &id, NULL), "get_or_insert"
      );
    }
  }
}

static void release_keys(void) {
  arnm_key_map_free(&g_filled_fast);
  arnm_key_map_free(&g_filled_keyed);
  arnm_reset(&g_lookup_arena);
  free(g_keys);
  free(g_misses);
  free(g_hit_order);
}

static uint64_t insert_all(arnm_key_map *map) {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    require_ok(
        arnm_key_map_get_or_insert(map, g_keys + (size_t)i * KEY_SIZE, &id, NULL), "get_or_insert"
    );
    sum += id;
  }
  return sum;
}

static void test_insert_arena(int count) {
  (void)count;
  arnm_reset(&g_arena);
  arnm_key_map map;
  require_ok(open_map(&map, &g_arena), "init");
  g_sink = insert_all(&map);
}

static void test_insert_arena_reserved(int count) {
  (void)count;
  arnm_reset(&g_arena);
  arnm_key_map map;
  require_ok(open_map(&map, &g_arena), "init");
  require_ok(arnm_key_map_reserve(&map, g_key_count), "reserve");
  g_sink = insert_all(&map);
}

static void test_insert_host(int count) {
  (void)count;
  arnm_key_map map;
  require_ok(open_map(&map, NULL), "init");
  g_sink = insert_all(&map);
  arnm_key_map_free(&map);
}

static uint64_t insert_all_generated(arnm_key_map *map) {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    require_ok(
        pubkey_map_get_or_insert(map, g_keys + (size_t)i * KEY_SIZE, &id, NULL), "get_or_insert"
    );
    sum += id;
  }
  return sum;
}

static void test_insert_arena_reserved_generated(int count) {
  (void)count;
  arnm_reset(&g_arena);
  arnm_key_map map;
  require_ok(open_map(&map, &g_arena), "init");
  require_ok(arnm_key_map_reserve(&map, g_key_count), "reserve");
  g_sink = insert_all_generated(&map);
}

static void test_get_or_insert_known_generated(int count) {
  (void)count;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    pubkey_map_get_or_insert(g_filled, g_keys + (size_t)g_hit_order[i] * KEY_SIZE, &id, NULL);
    sum += id;
  }
  g_sink = sum;
}

static void test_find_hit_generated(int count) {
  (void)count;
  uint64_t found = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    found += pubkey_map_find(g_filled, g_keys + (size_t)g_hit_order[i] * KEY_SIZE, &id);
  }
  g_sink = found;
}

static void test_find_miss_generated(int count) {
  (void)count;
  uint64_t found = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    found += pubkey_map_find(g_filled, g_misses + (size_t)i * KEY_SIZE, &id);
  }
  g_sink = found;
}

static void test_get_or_insert_known(int count) {
  (void)count;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    arnm_key_map_get_or_insert(g_filled, g_keys + (size_t)g_hit_order[i] * KEY_SIZE, &id, NULL);
    sum += id;
  }
  g_sink = sum;
}

static void test_find_hit(int count) {
  (void)count;
  uint64_t found = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    found += arnm_key_map_find(g_filled, g_keys + (size_t)g_hit_order[i] * KEY_SIZE, &id);
  }
  g_sink = found;
}

static void test_find_miss(int count) {
  (void)count;
  uint64_t found = 0;
  for (uint32_t i = 0; i < g_key_count; ++i) {
    uint32_t id = 0;
    found += arnm_key_map_find(g_filled, g_misses + (size_t)i * KEY_SIZE, &id);
  }
  g_sink = found;
}

/** What the arena handed out after the step that just ran, in MiB. */
static void print_arena_used(const char *label) {
  const uint32_t used = ARENA_CAPACITY - arnm_arena_remaining(&g_arena);
  printf("%-*s %9.1f MiB\n", BENCH_NAME_WIDTH, label, (double)used / (1024.0 * 1024.0));
}

static void run_rows(void) {
  g_filled = g_keyed ? &g_filled_keyed : &g_filled_fast;
  const int steps = (int)g_key_count;
  bench_step(test_insert_arena, steps, "  insert, growing, arena", "key");
  print_arena_used("    arena handed out");
  bench_step(test_insert_arena_reserved, steps, "  insert, reserved, arena", "key");
  print_arena_used("    arena handed out");
  bench_step(test_insert_host, steps, "  insert, growing, host", "key");
  // the insert rows above touched several times the table's size and pushed it out of the
  // cache; one untimed pass brings it back, so the rows below time the steady state an index
  // spends its life in rather than the first minute of it
  test_find_hit(0);
  test_find_miss(0);
  bench_step(test_get_or_insert_known, steps, "  get_or_insert, key known", "key");
  bench_step(test_find_hit, steps, "  find, hit", "key");
  bench_step(test_find_miss, steps, "  find, miss", "key");
  bench_step(
      test_insert_arena_reserved_generated, steps, "  generated: insert, reserved, arena", "key"
  );
  test_find_hit_generated(0);
  bench_step(test_get_or_insert_known_generated, steps, "  generated: get_or_insert, known", "key");
  bench_step(test_find_hit_generated, steps, "  generated: find, hit", "key");
  bench_step(test_find_miss_generated, steps, "  generated: find, miss", "key");
}

static void run_size(uint32_t key_count, const char *size) {
  char title[64];
  prepare_keys(key_count);
  g_keyed = false;
  snprintf(title, sizeof(title), "%s keys of 32 bytes, fast hash", size);
  bench_section(title);
  run_rows();
  g_keyed = true;
  snprintf(title, sizeof(title), "%s keys of 32 bytes, SipHash-1-3", size);
  bench_section(title);
  run_rows();
  release_keys();
}

int main(void) {
  arnm_mono_timer time_used;
  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  require_ok(arnm_init_arena(&g_arena, ARENA_CAPACITY), "arena");
  require_ok(arnm_init_arena(&g_lookup_arena, ARENA_CAPACITY), "arena");

  run_size(10000u, "10k");
  run_size(100000u, "100k");
  run_size(1000000u, "1M");

  arnm_release(&g_arena);
  bench_total_time(time_used);
  return 0;
}
