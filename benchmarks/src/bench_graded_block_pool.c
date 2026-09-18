#include "arnm/arena.h"
#include "arnm/graded_block_pool.h"
#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "arnm/multi_arena.h"
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
 * another of a new random size. The host answers that with malloc and free, the pool with its
 * free lists. An arena cannot take the freed blocks back, so behind it the working set walks
 * forward through the arena; that row runs a sixteenth of the steps, which already takes half a
 * gigabyte, and reports how far. Each row times one free plus one allocation.
 *
 * Growth: the pattern of a roaring style set. Many containers, each starting at 16 bytes and
 * doubling up to 8 KiB, all of them a step at a time in turn, then all dropped and the next
 * round built the same way. The host answers with realloc, an arena with arnm_realloc (which buries
 * every block but the tail), the pool with its realloc. The rows report the time per doubling and
 * the memory all rounds together held at the end.
 */

#define ARENA_CAPACITY (1024u * 1024u * 1024u)
#define WORKING_SET 4096u
#define CHURN_STEPS 4000000
#define MAX_BLOCK 4096u
#define CONTAINERS 4096u
#define GROWTH_ROUNDS 8u
#define GROWTH_FIRST 16u
#define GROWTH_LAST 8192u
#define GROWTH_STEPS 9u /* 16 -> 8192 is nine doublings */

typedef struct held {
  uint8_t *block;
  uint32_t size;
} held;

typedef enum backend { BACKEND_HOST, BACKEND_ARENA, BACKEND_POOL } backend;

static held g_held[WORKING_SET];
static uint8_t *g_containers[CONTAINERS];
static uint32_t *g_sizes;   /* CHURN_STEPS random sizes, the same sequence for every row */
static uint32_t *g_victims; /* CHURN_STEPS random slots of the working set */
static arnm g_arena;
static arnm_graded_block_pool g_pool;
static backend g_backend; /* what the current row runs on */
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

/* one switch per call on every row alike, so no row gets a cheaper dispatch than another */
static void do_alloc(uint8_t **block, uint32_t size) {
  switch (g_backend) {
  case BACKEND_HOST:
    require_ok(arnm_alloc(block, size, NULL), "alloc");
    break;
  case BACKEND_ARENA:
    require_ok(arnm_alloc(block, size, &g_arena), "alloc");
    break;
  case BACKEND_POOL:
    require_ok(arnm_graded_block_pool_alloc(&g_pool, block, size), "alloc");
    break;
  }
}

static void do_free(uint8_t *block, uint32_t size) {
  switch (g_backend) {
  case BACKEND_HOST:
    (void)arnm_free(block, size, NULL);
    break;
  case BACKEND_ARENA:
    (void)arnm_free(block, size, &g_arena);
    break;
  case BACKEND_POOL:
    (void)arnm_graded_block_pool_free(&g_pool, block, size);
    break;
  }
}

static void do_realloc(uint8_t **block, uint32_t old_size, uint32_t new_size) {
  switch (g_backend) {
  case BACKEND_HOST:
    require_ok(arnm_realloc(block, old_size, new_size, NULL), "realloc");
    break;
  case BACKEND_ARENA:
    require_ok(arnm_realloc(block, old_size, new_size, &g_arena), "realloc");
    break;
  case BACKEND_POOL:
    require_ok(arnm_graded_block_pool_realloc(&g_pool, block, old_size, new_size), "realloc");
    break;
  }
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

static void init_pool(void) {
  arnm_graded_block_pool_options options = {0};
  require_ok(arnm_graded_block_pool_init(&g_pool, &options, NULL), "pool");
}

static uint64_t pool_reserved(void) {
  arnm_multi_arena_stats stats = {0};
  require_ok(arnm_multi_arena_measure(g_pool.source, &stats), "measure");
  return stats.reserved;
}

static void print_bytes(const char *label, uint64_t bytes) {
  printf("%-*s %9.1f MiB\n", BENCH_NAME_WIDTH, label, (double)bytes / (1024.0 * 1024.0));
}

// ********** churn *******************

static void test_churn(int count) {
  uint64_t sum = 0;
  for (int step = 0; step < count; ++step) {
    held *slot = &g_held[g_victims[step]];
    do_free(slot->block, slot->size);
    slot->size = g_sizes[step];
    do_alloc(&slot->block, slot->size);
    slot->block[0] = (uint8_t)step;
    sum += slot->block[0];
  }
  g_sink = sum;
}

static void churn_row(backend which, int steps, const char *name) {
  g_backend = which;
  for (uint32_t i = 0; i < WORKING_SET; ++i) {
    g_held[i].size = g_sizes[i];
    do_alloc(&g_held[i].block, g_held[i].size);
  }
  bench_step(test_churn, steps, name, "free+alloc");
  for (uint32_t i = 0; i < WORKING_SET; ++i) { do_free(g_held[i].block, g_held[i].size); }
}

// ********** growth *******************

static void test_growth(int count) {
  (void)count;
  uint64_t sum = 0;
  for (uint32_t round = 0; round < GROWTH_ROUNDS; ++round) {
    for (uint32_t c = 0; c < CONTAINERS; ++c) {
      do_alloc(&g_containers[c], GROWTH_FIRST);
      g_containers[c][0] = (uint8_t)c;
    }
    // round robin, the way values arrive for many sets at once: no container stays at the tail
    for (uint32_t size = GROWTH_FIRST; size < GROWTH_LAST; size *= 2u) {
      for (uint32_t c = 0; c < CONTAINERS; ++c) {
        do_realloc(&g_containers[c], size, size * 2u);
        g_containers[c][size] = g_containers[c][0];
      }
    }
    for (uint32_t c = 0; c < CONTAINERS; ++c) {
      sum += g_containers[c][GROWTH_LAST / 2u];
      do_free(g_containers[c], GROWTH_LAST);
    }
  }
  g_sink = sum;
}

static void growth_row(backend which, const char *name) {
  g_backend = which;
  bench_step(test_growth, (int)(CONTAINERS * GROWTH_ROUNDS * GROWTH_STEPS), name, "doubling");
}

int main(void) {
  arnm_mono_timer time_used;
  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  prepare();
  require_ok(arnm_init_arena(&g_arena, ARENA_CAPACITY), "arena");
  bench_prepared(time_used);

  char title[128];
  snprintf(
      title, sizeof(title), "churn: %u live blocks of 1..%u bytes, %d replacements", WORKING_SET,
      MAX_BLOCK, CHURN_STEPS
  );
  bench_section(title);
  churn_row(BACKEND_HOST, CHURN_STEPS, "  host malloc/free");

  init_pool();
  churn_row(BACKEND_POOL, CHURN_STEPS, "  pool");
  print_bytes("    pool reserved", pool_reserved());
  arnm_graded_block_pool_release(&g_pool, NULL);

  arnm_reset(&g_arena);
  // a sixteenth of the steps: nothing comes back, and the full run would need some 8 GiB
  churn_row(BACKEND_ARENA, CHURN_STEPS / 16, "  arena alone, 1/16 of the steps");
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));

  snprintf(
      title, sizeof(title), "growth: %u rounds of %u containers, %u -> %u bytes by doubling",
      GROWTH_ROUNDS, CONTAINERS, GROWTH_FIRST, GROWTH_LAST
  );
  bench_section(title);
  growth_row(BACKEND_HOST, "  host realloc");

  init_pool();
  growth_row(BACKEND_POOL, "  pool realloc");
  print_bytes("    pool reserved", pool_reserved());
  arnm_graded_block_pool_release(&g_pool, NULL);

  arnm_reset(&g_arena);
  growth_row(BACKEND_ARENA, "  arena realloc");
  print_bytes("    arena used", ARENA_CAPACITY - arnm_arena_remaining(&g_arena));

  arnm_release(&g_arena);
  free(g_sizes);
  free(g_victims);
  bench_total_time(time_used);
  return 0;
}
