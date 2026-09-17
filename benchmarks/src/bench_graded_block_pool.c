#include "arnm/arena.h"
#include "arnm/graded_block_pool.h"
#include "arnm/key_map.h"
#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * Two things a graded block pool is for, each against what a caller would do without one.
 *
 * Churn: a working set of blocks of mixed sizes, one replaced at a time -- free one, allocate
 * another of a new random size. The host answers that with malloc and free. An arena cannot
 * take the freed blocks back, so behind it the working set walks forward through the arena; that
 * row runs a sixteenth of the steps, which already takes half a gigabyte, and reports how far.
 * The pool over the same arena reuses them, and over the host it keeps them cached instead of
 * calling free. Each row times one free plus one allocation.
 *
 * Growth: key maps filled one after another and freed, the way a rebuilt index does it. Straight
 * on an arena every table a map outgrew stays behind; through a pool the next growth reuses it.
 * The rows report the time per key and the arena bytes all rounds together took.
 */

#define ARENA_CAPACITY (1024u * 1024u * 1024u)
#define WORKING_SET 4096u
#define CHURN_STEPS 4000000
#define MAX_BLOCK 4096u
#define MAP_KEYS 200000u
#define MAP_ROUNDS 5u

typedef struct held {
  uint8_t *block;
  uint32_t size;
} held;

static held g_held[WORKING_SET];
static uint32_t *g_sizes;   /* CHURN_STEPS random sizes, the same sequence for every row */
static uint32_t *g_victims; /* CHURN_STEPS random slots of the working set */
static arnm g_arena;
static arnm *g_memory; /* the allocator the current row runs on */
static volatile uint64_t g_sink;

static uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS == result || ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == result) { return; }
  fprintf(stderr, "benchmark setup failed: %s answered %d\n", what, (int)result);
  exit(EXIT_FAILURE);
}

static void prepare(void) {
  g_sizes = (uint32_t *)malloc((size_t)CHURN_STEPS * sizeof(uint32_t));
  g_victims = (uint32_t *)malloc((size_t)CHURN_STEPS * sizeof(uint32_t));
  if (!g_sizes || !g_victims) {
    fprintf(stderr, "benchmark setup failed: out of host memory\n");
    exit(EXIT_FAILURE);
  }
  uint64_t state = 0x626c6f636b706f6fULL;
  for (uint32_t i = 0; i < CHURN_STEPS; ++i) {
    g_sizes[i] = 1u + (uint32_t)(splitmix64(&state) % MAX_BLOCK);
    g_victims[i] = (uint32_t)(splitmix64(&state) % WORKING_SET);
  }
}

static void fill_working_set(void) {
  for (uint32_t i = 0; i < WORKING_SET; ++i) {
    g_held[i].size = g_sizes[i];
    require_ok(arnm_alloc(&g_held[i].block, g_held[i].size, g_memory), "alloc");
  }
}

static void empty_working_set(void) {
  for (uint32_t i = 0; i < WORKING_SET; ++i) {
    (void)arnm_free(g_held[i].block, g_held[i].size, g_memory);
  }
}

static void test_churn(int count) {
  uint64_t sum = 0;
  for (int step = 0; step < count; ++step) {
    held *slot = &g_held[g_victims[step]];
    (void)arnm_free(slot->block, slot->size, g_memory);
    slot->size = g_sizes[step];
    require_ok(arnm_alloc(&slot->block, slot->size, g_memory), "alloc");
    slot->block[0] = (uint8_t)step;
    sum += slot->block[0];
  }
  g_sink = sum;
}

static void print_bytes(const char *label, uint64_t bytes) {
  printf("%-*s %9.1f MiB\n", BENCH_NAME_WIDTH, label, (double)bytes / (1024.0 * 1024.0));
}

static void churn_row(arnm *memory, int steps, const char *name) {
  g_memory = memory;
  fill_working_set();
  bench_step(test_churn, steps, name, "free+alloc");
  empty_working_set();
}

static void test_map_rounds(int count) {
  (void)count;
  uint64_t sum = 0;
  for (uint32_t round = 0; round < MAP_ROUNDS; ++round) {
    arnm_key_map map;
    require_ok(arnm_key_map_init(&map, sizeof(uint64_t), 12, g_memory), "init");
    for (uint64_t key = 0; key < MAP_KEYS; ++key) {
      uint32_t id = 0;
      const uint64_t mixed = key * 0x9e3779b97f4a7c15ULL + round;
      require_ok(
          arnm_key_map_get_or_insert(&map, (const uint8_t *)&mixed, &id, NULL), "get_or_insert"
      );
      sum += id;
    }
    arnm_key_map_free(&map);
  }
  g_sink = sum;
}

int main(void) {
  arnm_mono_timer time_used;
  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  prepare();
  require_ok(arnm_init_arena(&g_arena, ARENA_CAPACITY), "arena");
  bench_prepared(time_used);

  char title[96];
  snprintf(
      title, sizeof(title), "churn: %u live blocks of 1..%u bytes, %d replacements", WORKING_SET,
      MAX_BLOCK, CHURN_STEPS
  );
  bench_section(title);
  churn_row(NULL, CHURN_STEPS, "  host malloc/free");

  arnm_graded_block_pool_options options = {0};
  arnm *host_pool = arnm_create_graded_block_pool(&options, NULL);
  if (!host_pool) { require_ok(ARNM_ERROR_OUT_OF_MEMORY, "pool"); }
  churn_row(host_pool, CHURN_STEPS, "  pool over the host");
  arnm_destroy(host_pool, NULL);

  arnm_reset(&g_arena);
  arnm *arena_pool = arnm_create_graded_block_pool(&options, &g_arena);
  if (!arena_pool) { require_ok(ARNM_ERROR_OUT_OF_MEMORY, "pool"); }
  churn_row(arena_pool, CHURN_STEPS, "  pool over an arena");
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));
  arnm_destroy(arena_pool, &g_arena);

  arnm_reset(&g_arena);
  // a sixteenth of the steps: nothing comes back, and the full run would need some 8 GiB
  churn_row(&g_arena, CHURN_STEPS / 16, "  arena alone, 1/16 of the steps");
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));

  snprintf(
      title, sizeof(title), "growth: %u key maps of %u 8 byte keys, built and freed", MAP_ROUNDS,
      MAP_KEYS
  );
  bench_section(title);
  arnm_reset(&g_arena);
  g_memory = &g_arena;
  bench_step(test_map_rounds, (int)(MAP_KEYS * MAP_ROUNDS), "  straight on an arena", "key");
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));

  arnm_reset(&g_arena);
  arnm *map_pool = arnm_create_graded_block_pool(&options, &g_arena);
  if (!map_pool) { require_ok(ARNM_ERROR_OUT_OF_MEMORY, "pool"); }
  g_memory = map_pool;
  bench_step(
      test_map_rounds, (int)(MAP_KEYS * MAP_ROUNDS), "  through a pool over the arena", "key"
  );
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));
  arnm_destroy(map_pool, &g_arena);

  arnm_release(&g_arena);
  free(g_sizes);
  free(g_victims);
  bench_total_time(time_used);
  return 0;
}
