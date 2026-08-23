#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * What this benchmark measures
 *
 * A multi arena serves a request first fit, scanning from `first_open` -- the earliest arena
 * that may still have room. Under allocation alone that marker only walks forward, and only
 * over arenas whose remainder has fallen to the chain's full threshold or below. (A free or a
 * realloc that hands bytes back can pull it onto an earlier arena again; nothing here does.) An
 * arena left with more than the threshold but less than the current request is therefore neither
 * served from nor skipped: it is walked over, on this allocation and on every one that follows.
 *
 * The threshold is named at init, so these chains name it rather than inherit it: the figures
 * below say what a caller buys by moving it, and they should not shift when the default does.
 *
 * That is the case measured here. A chain is prepared with N such arenas, each holding a
 * remainder just above the threshold, and then asked for a size just above the remainder. Every
 * request pays the full walk before it lands, so the per allocation figure is the scan itself.
 *
 * The counterpart runs the same chain lengths with the arenas filled to the threshold instead.
 * The marker settles past all of them once and the scan goes back to a constant -- the same
 * arenas, the same addresses, only a remainder eight bytes smaller.
 *
 * Every probe in the scan sections lands in one borrowed arena at the end of the chain, sized
 * exactly for the run. No arena is opened and the host is not called while the clock runs, so
 * what the figure holds is the walk and nothing else. The last section drops that shelter on
 * purpose and lets the chain grow under its own allocations.
 */

/** Bytes a regular arena of these chains reserves. */
#define ARENA_CAPACITY 4096u

/** Full threshold these chains are built with, named rather than inherited from the default. */
#define FULL_REMAINING 128u

/**
 * Remainder deliberately left in each stranded arena: the smallest multiple of 8 above the
 * threshold, so the arena stays open by a single alignment step and the marker cannot pass it.
 */
#define STRANDED_REMAINING (FULL_REMAINING + 8u)

/** Request that fills a fresh arena down to exactly STRANDED_REMAINING. */
#define FILL_SIZE (ARENA_CAPACITY - STRANDED_REMAINING)

/** The measured request: one alignment step above the remainder, so no stranded arena fits it. */
#define PROBE_SIZE (STRANDED_REMAINING + 8u)

/** Allocations per scan step. The borrowed tail arena is sized for exactly this many. */
#define PROBE_COUNT 50000

/** Allocations in the growth section, where every one of them opens and strands an arena. */
#define GROWTH_COUNT 4000

/* The two counts as text, so a section heading names its own denominator and cannot drift from
   the number the steps below it actually run. The closing line has no single count to give. */
#define BENCH_STRINGIFY_(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_(x)
#define BENCH_PROBES BENCH_STRINGIFY(PROBE_COUNT)
#define BENCH_GROWTH BENCH_STRINGIFY(GROWTH_COUNT)

/* The sizes above only do their job in this order. Settled here rather than discovered as a
   flat line in the report, or as a chain that refuses to init at all. */
static_assert(STRANDED_REMAINING > FULL_REMAINING, "remainder must stay open");
static_assert(FULL_REMAINING < ARENA_CAPACITY, "init refuses a threshold that fills an arena");
static_assert(PROBE_SIZE > STRANDED_REMAINING, "probe must not fit a stranded remainder");
static_assert(FILL_SIZE > STRANDED_REMAINING, "a fill must not fit a stranded remainder either");
static_assert(STRANDED_REMAINING % 8u == 0u, "remainder must survive the arena's rounding");
static_assert(PROBE_SIZE % 8u == 0u, "probe must survive the arena's rounding");

/**
 * One prepared chain, ready to be probed.
 *
 * The tail is borrowed rather than allocated by the chain: an arena that is already there and
 * large enough for the whole run keeps every host call outside the measured loop.
 */
typedef struct scan_case {
  arnm *chain;        /**< Owned by this file; released with arnm_destroy(). */
  uint8_t *tail;      /**< Borrowed buffer every probe lands in; owned by this file. */
  uint32_t tail_size; /**< Bytes of @c tail, exactly PROBE_COUNT probes worth. */
} scan_case;

/** Chains whose arenas keep a remainder the marker cannot pass. Index follows g_scan_arenas. */
static scan_case g_stranded[5];
/** The same chain lengths with the arenas run full, so the marker settles past them. */
static scan_case g_full[5];
/** Arenas ahead of the tail in each prepared case. */
static const uint32_t g_scan_arenas[5] = {0, 16, 64, 256, 1024};

/** Consumes every address handed out, so no allocation can be optimized away. */
static volatile uint64_t g_sink = 0;

/**
 * Stop on a failed setup instead of measuring the wreckage.
 *
 * A chain that could not be built serves nothing, and the probe loop would leave on its first
 * request -- a step finishing suspiciously fast and reported as a result. Checked during
 * preparation only, never inside a measured loop.
 */
/**
 * Every step of a measured loop has to have run, or the row is a lie.
 *
 * bench_step() divides the elapsed time by the count it was given and calls the result a per
 * allocation figure. A loop that left early did less work than that count, so the row prints
 * as the fastest on the page -- the one shape a benchmark must never take. Checked after the
 * loop and not inside it: the branch that leaves is already there, and a call per iteration
 * would show up in figures measured in tens of nanoseconds.
 */
static void require_all_steps(int served, int steps, const char *what) {
  if (served == steps) { return; }
  fprintf(stderr, "benchmark aborted: %s served %d of %d steps\n", what, served, steps);
  exit(EXIT_FAILURE);
}

static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS == result) { return; }
  fprintf(stderr, "benchmark setup failed: %s: %s\n", what, arnm_result_to_string(result));
  exit(EXIT_FAILURE);
}

/**
 * A chain with this file's capacity and threshold, rather than the library's defaults.
 *
 * The figures below are about the walk, so the two numbers that decide its shape are named
 * here and cannot move when a default does.
 */
static arnm *make_chain(void) {
  arnm_multi_arena_options options = {0};
  options.arena_capacity = ARENA_CAPACITY;
  options.full_remaining = FULL_REMAINING;
  arnm *chain = arnm_create_multi_arena(&options, NULL);
  if (!chain) {
    fprintf(stderr, "benchmark setup failed: create chain\n");
    exit(EXIT_FAILURE);
  }
  return chain;
}

/* --- preparation ------------------------------------------------------------------------- */

/**
 * Build a chain of @p arena_count arenas and give it a tail large enough for the whole run.
 *
 * Each arena is filled by a single request, so the remainder is exactly what is left over:
 * @ref STRANDED_REMAINING when @p stranded, nothing at all otherwise. A request that no arena
 * can serve opens the next one, which is what makes the chain grow one arena per fill.
 *
 * @param[out] c           Case to fill in; every field is written.
 * @param[in]  arena_count Arenas to place ahead of the tail.
 * @param[in]  stranded    true leaves each arena a remainder above the full threshold, false
 *                         fills it to the brim.
 * @whisper Ground laid down ahead of time, so the walk over it can be timed alone
 */
static void build_case(scan_case *c, uint32_t arena_count, int stranded) {
  const uint32_t fill = stranded ? (uint32_t)FILL_SIZE : ARENA_CAPACITY;
  uint8_t *block = NULL;

  c->chain = make_chain();
  /* the descriptors are bookkeeping, not part of what is measured -- have them ready */
  require_ok(arnm_multi_arena_reserve(c->chain, arena_count + 1), "reserve descriptors");

  for (uint32_t i = 0; i < arena_count; ++i) {
    require_ok(arnm_alloc(&block, fill, c->chain), "fill arena");
  }

  /* sized for the run exactly: the last probe leaves it empty and none opens fresh ground */
  c->tail_size = (uint32_t)PROBE_COUNT * (uint32_t)PROBE_SIZE;
  require_ok(arnm_alloc(&c->tail, c->tail_size, NULL), "tail buffer");
  require_ok(arnm_multi_arena_borrow(c->chain, c->tail, c->tail_size), "borrow tail");
}

static void release_case(scan_case *c) {
  /* the borrowed block is let go untouched, so it is ours to hand back afterwards */
  require_ok(arnm_destroy(c->chain, NULL), "destroy chain");
  c->chain = NULL;
  arnm_free(c->tail, c->tail_size, NULL);
  c->tail = NULL;
}

static void prepare_test_data(void) {
  for (uint32_t i = 0; i < 5; ++i) {
    build_case(&g_stranded[i], g_scan_arenas[i], 1);
    build_case(&g_full[i], g_scan_arenas[i], 0);
  }
}

static void release_test_data(void) {
  for (uint32_t i = 0; i < 5; ++i) {
    release_case(&g_stranded[i]);
    release_case(&g_full[i]);
  }
}

/* --- the measured walk -------------------------------------------------------------------- */

/**
 * Ask @p c for @p steps blocks of @ref PROBE_SIZE and consume every address.
 *
 * The chain is prepared so that each request walks the arenas ahead of the tail and is then
 * served by the tail itself. Nothing is written to the memory: the question is what the search
 * costs, and touching seven megabytes would answer a different one.
 */
static void probe(scan_case *c, int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    uint8_t *block = NULL;
    if (ARNM_SUCCESS != arnm_alloc(&block, PROBE_SIZE, c->chain)) { break; }
    sink += (uint64_t)(uintptr_t)block;
  }
  g_sink += sink;
  require_all_steps(i, steps, "probe");
}

static void test_stranded_0(int steps) {
  probe(&g_stranded[0], steps);
}
static void test_stranded_16(int steps) {
  probe(&g_stranded[1], steps);
}
static void test_stranded_64(int steps) {
  probe(&g_stranded[2], steps);
}
static void test_stranded_256(int steps) {
  probe(&g_stranded[3], steps);
}
static void test_stranded_1024(int steps) {
  probe(&g_stranded[4], steps);
}

static void test_full_16(int steps) {
  probe(&g_full[1], steps);
}
static void test_full_64(int steps) {
  probe(&g_full[2], steps);
}
static void test_full_256(int steps) {
  probe(&g_full[3], steps);
}
static void test_full_1024(int steps) {
  probe(&g_full[4], steps);
}

/* --- a chain growing under its own allocations -------------------------------------------- */

/**
 * Allocate @p steps times so that every request strands one more arena behind it.
 *
 * Request k walks the k arenas already there, finds every remainder too small, and opens the
 * next one -- the walk grows with the chain. Opening the arenas and handing them back is inside
 * the clock here, on purpose: this is the shape a long lived chain really takes when its
 * requests never fit the remainders they leave, and the host calls are part of that shape.
 * Read the two rows against each other, not against the sections above.
 *
 * @whisper Each step lays ground the next step must cross
 */
static void test_growth_stranded(int steps) {
  arnm *m = make_chain();
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    uint8_t *block = NULL;
    if (ARNM_SUCCESS != arnm_alloc(&block, FILL_SIZE, m)) { break; }
    sink += (uint64_t)(uintptr_t)block;
  }
  g_sink += sink;
  require_all_steps(i, steps, "growth, stranding");
  arnm_destroy(m, NULL);
}

/** The same growth with requests that leave nothing behind, so the marker keeps up. */
static void test_growth_full(int steps) {
  arnm *m = make_chain();
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    uint8_t *block = NULL;
    if (ARNM_SUCCESS != arnm_alloc(&block, ARENA_CAPACITY, m)) { break; }
    sink += (uint64_t)(uintptr_t)block;
  }
  g_sink += sink;
  require_all_steps(i, steps, "growth, filling");
  arnm_destroy(m, NULL);
}

/* --- driver -------------------------------------------------------------------------------- */

int main(void) {
  arnm_mono_timer timeUsed;

  if (!bench_timer_start(&timeUsed)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(timeUsed);

  printf(
      "\narena %u B, stranded remainder %u B, request %u B, full threshold %u B\n",
      (unsigned)ARENA_CAPACITY, (unsigned)STRANDED_REMAINING, (unsigned)PROBE_SIZE,
      (unsigned)FULL_REMAINING
  );

  bench_section("first fit past arenas whose remainder is too small -- " BENCH_PROBES " allocs");
  bench_step(test_stranded_0, PROBE_COUNT, "  no arena in the way", "alloc");
  bench_step(test_stranded_16, PROBE_COUNT, "  16 stranded arenas", "alloc");
  bench_step(test_stranded_64, PROBE_COUNT, "  64 stranded arenas", "alloc");
  bench_step(test_stranded_256, PROBE_COUNT, "  256 stranded arenas", "alloc");
  bench_step(test_stranded_1024, PROBE_COUNT, "  1024 stranded arenas", "alloc");

  bench_section("the same chains with the arenas run full -- " BENCH_PROBES " allocs");
  bench_step(test_full_16, PROBE_COUNT, "  16 arenas, marker passes", "alloc");
  bench_step(test_full_64, PROBE_COUNT, "  64 arenas, marker passes", "alloc");
  bench_step(test_full_256, PROBE_COUNT, "  256 arenas, marker passes", "alloc");
  bench_step(test_full_1024, PROBE_COUNT, "  1024 arenas, marker passes", "alloc");

  bench_section(
      "a chain growing under its own allocations, opening and release included -- " BENCH_GROWTH
      " allocs"
  );
  bench_step(test_growth_stranded, GROWTH_COUNT, "  every alloc strands an arena", "alloc");
  bench_step(test_growth_full, GROWTH_COUNT, "  every alloc fills one to the brim", "alloc");

  /* the sections do not share a step count, so the closing line names none */
  bench_total_time(timeUsed);

  release_test_data();
  return 0;
}
