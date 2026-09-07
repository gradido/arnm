#include "arnm/arena.h"
#include "arnm/dynamic_arena_pool.h"
#include "arnm/fixed_arena_pool.h"
#include "arnm/graded_arena_pool.h"
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
 * what the figure holds is the walk and nothing else. The growth section drops that shelter on
 * purpose and lets the chain grow under its own allocations.
 *
 * The sections after it leave the chain and turn to the three pools, which all answer the same
 * question -- hand me an arena -- and answer it at very different prices. A round trip is
 * measured against the host call it replaces, so what a shelf is worth is a ratio and not an
 * assertion; a burst is measured against the same pool with no shelf at all; and the graded
 * pool's ladder is walked from two rungs to thirty two, which is the one cost it adds over the
 * dynamic pool underneath it -- which is a bit scan and a bit count, and so is meant to cost
 * the same at every ladder height.
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

/* Defined with the pool sections further down, where their constants live; called from here so
   that every pool is standing before the clock starts, the same as the chains above. */
static void prepare_pools(void);
static void release_pools(void);

static void prepare_test_data(void) {
  for (uint32_t i = 0; i < 5; ++i) {
    build_case(&g_stranded[i], g_scan_arenas[i], 1);
    build_case(&g_full[i], g_scan_arenas[i], 0);
  }
  prepare_pools();
}

static void release_test_data(void) {
  for (uint32_t i = 0; i < 5; ++i) {
    release_case(&g_stranded[i]);
    release_case(&g_full[i]);
  }
  release_pools();
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

/* --- the pools ----------------------------------------------------------------------------- */

/*
 * Three pools, one question. What separates them is where an arena comes from when the shelf is
 * bare, so the rows are laid out to make exactly that visible: the same round trip against a
 * shelf that always has one, against a shelf that never does, and against no pool at all.
 *
 * The arenas are small on purpose. A pool's own work is a pointer swap and a counter, measured
 * in single nanoseconds, and the host call it replaces is the cheapest one malloc has -- a
 * large arena would put page faults in the figure and answer a question about the host instead.
 * Nothing is written into the arenas either: what an arena costs to fill is arnm_alloc's row in
 * the sections above, not a pool's.
 *
 * One row is there to be read as a warning rather than as an option. A dynamic pool with a
 * stock of zero comes out slower than opening an arena by hand: it makes the same host call and
 * adds its own bookkeeping on top, and keeps nothing to show for it. A shelf of zero is a legal
 * configuration and an empty one -- what a dynamic pool is worth begins at a stock of one.
 */

/** Bytes every pooled arena holds. Small, so a row reports the pool and not the page fault. */
#define POOL_ARENA_CAPACITY 256u

/** Arenas the fixed pool reserves, and the depth of the burst section. */
#define BURST_DEPTH 64

/** Rounds of BURST_DEPTH in the burst section; the step count is the arenas, not the rounds. */
#define BURST_ROUNDS 20000

/** Take one arena and give it back, this many times. */
#define ROUNDTRIP_COUNT 2000000

/** The same, on ladders of differing height. */
#define LADDER_COUNT 2000000

/**
 * Arenas the burst section moves in total, spelled out rather than multiplied.
 *
 * The section heading is built from this by stringification, and `#x` prints the expression it
 * is handed -- a product would arrive in the report as the arithmetic instead of the number.
 * The assert below keeps the literal honest.
 */
#define BURST_ARENAS 1280000
static_assert(BURST_ARENAS == BURST_DEPTH * BURST_ROUNDS, "the heading must name the real count");

#define BENCH_ROUNDTRIPS BENCH_STRINGIFY(ROUNDTRIP_COUNT)
#define BENCH_BURST BENCH_STRINGIFY(BURST_ARENAS)
#define BENCH_BURST_DEPTH BENCH_STRINGIFY(BURST_DEPTH)
#define BENCH_LADDERS BENCH_STRINGIFY(LADDER_COUNT)

/**
 * Ladder heights the last section is run at. The point of the rows is that they do not differ:
 * the lookup reads a mask rather than walking the grades, so height is not supposed to show.
 *
 * Sixteen and not the twenty nine a ladder can hold, because every rung is stocked with an
 * arena and the rungs double: sixteen of them is half a megabyte held, twenty nine would be
 * four gigabytes.
 */
static const uint16_t g_ladder_grades[3] = {2, 8, 16};
#define LADDER_MAX_GRADES 16u

/** Capacity of rung i. Powers of two, which is the only shape a ladder takes. */
#define LADDER_RUNG_CAPACITY(i) (8u << (i))

static_assert(POOL_ARENA_CAPACITY % 8u == 0u, "a pooled arena must survive the rounding");
static_assert(BURST_DEPTH <= UINT16_MAX, "the fixed pool counts its arenas in a uint16_t");
static_assert(LADDER_RUNG_CAPACITY(0) == ARNM_GRADED_MIN_SIZE, "the ladder starts at the floor");
static_assert(
    LADDER_MAX_GRADES <= ARNM_GRADED_MAX_GRADE_COUNT, "a ladder holds 29 powers of two at most"
);

/** Reserved up front, so no round trip in the first sections ever reaches the host. */
static arnm_fixed_arena_pool g_fixed_pool;
/** A shelf deep enough for the whole burst: every arena comes from stock and goes back to it. */
static arnm_dynamic_arena_pool g_dynamic_stocked;
/** The same pool with no shelf at all -- every round trip is a malloc and a free. */
static arnm_dynamic_arena_pool g_dynamic_bare;
/** Ladders of g_ladder_grades[i] rungs, each rung holding one arena in stock. */
static arnm_graded_arena_pool g_ladders[3];

static void prepare_pools(void) {
  require_ok(
      arnm_fixed_arena_pool_init(&g_fixed_pool, POOL_ARENA_CAPACITY, (uint16_t)BURST_DEPTH, NULL),
      "init fixed pool"
  );
  require_ok(
      arnm_dynamic_arena_pool_init(&g_dynamic_stocked, POOL_ARENA_CAPACITY, BURST_DEPTH),
      "init stocked pool"
  );
  /* the stock is filled here rather than by the first rounds, so no row pays for the others */
  require_ok(
      arnm_dynamic_arena_pool_reserve(&g_dynamic_stocked, BURST_DEPTH), "reserve stocked pool"
  );
  require_ok(
      arnm_dynamic_arena_pool_init(&g_dynamic_bare, POOL_ARENA_CAPACITY, 0), "init bare pool"
  );

  uint32_t sizes[LADDER_MAX_GRADES];
  for (uint32_t i = 0; i < LADDER_MAX_GRADES; ++i) { sizes[i] = LADDER_RUNG_CAPACITY(i); }
  for (uint32_t i = 0; i < 3; ++i) {
    require_ok(
        arnm_graded_arena_pool_init(&g_ladders[i], sizes, g_ladder_grades[i], 1), "init ladder"
    );
    /* every rung stocked, so a ladder row is the walk and never a host call */
    for (uint16_t grade = 0; grade < g_ladder_grades[i]; ++grade) {
      require_ok(
          arnm_dynamic_arena_pool_reserve(arnm_graded_arena_pool_grade_at(&g_ladders[i], grade), 1),
          "stock a rung"
      );
    }
  }
}

static void release_pools(void) {
  require_ok(arnm_fixed_arena_pool_release(&g_fixed_pool, NULL), "release fixed pool");
  require_ok(arnm_dynamic_arena_pool_release(&g_dynamic_stocked), "release stocked pool");
  require_ok(arnm_dynamic_arena_pool_release(&g_dynamic_bare), "release bare pool");
  for (uint32_t i = 0; i < 3; ++i) {
    require_ok(arnm_graded_arena_pool_release(&g_ladders[i]), "release ladder");
  }
}

/* --- one arena, taken and given back ------------------------------------------------------- */

static void test_fixed_roundtrip(int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    arnm *arena = NULL;
    if (ARNM_SUCCESS != arnm_fixed_arena_pool_alloc(&g_fixed_pool, &arena)) { break; }
    sink += (uint64_t)(uintptr_t)arena;
    if (ARNM_SUCCESS != arnm_fixed_arena_pool_free(&g_fixed_pool, arena)) { break; }
  }
  g_sink += sink;
  require_all_steps(i, steps, "fixed pool round trip");
}

static void test_dynamic_stocked_roundtrip(int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    arnm *arena = NULL;
    if (ARNM_SUCCESS != arnm_dynamic_arena_pool_alloc(&g_dynamic_stocked, &arena)) { break; }
    sink += (uint64_t)(uintptr_t)arena;
    if (ARNM_SUCCESS != arnm_dynamic_arena_pool_free(&g_dynamic_stocked, arena)) { break; }
  }
  g_sink += sink;
  require_all_steps(i, steps, "stocked pool round trip");
}

/* The shelf is zero deep, so every step makes an arena and gives it straight back. What this
   row costs over the one above it is the host call the shelf exists to avoid. */
static void test_dynamic_bare_roundtrip(int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    arnm *arena = NULL;
    if (ARNM_SUCCESS != arnm_dynamic_arena_pool_alloc(&g_dynamic_bare, &arena)) { break; }
    sink += (uint64_t)(uintptr_t)arena;
    if (ARNM_SUCCESS != arnm_dynamic_arena_pool_free(&g_dynamic_bare, arena)) { break; }
  }
  g_sink += sink;
  require_all_steps(i, steps, "bare pool round trip");
}

/* No pool at all: the arena owns its block, so this is the malloc and free a caller pays
   without one. The baseline every row above is read against. */
static void test_no_pool_roundtrip(int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    arnm arena;
    if (ARNM_SUCCESS != arnm_init_arena(&arena, POOL_ARENA_CAPACITY)) { break; }
    sink += (uint64_t)(uintptr_t)&arena;
    arnm_release(&arena);
  }
  g_sink += sink;
  require_all_steps(i, steps, "no pool round trip");
}

/* --- a burst, taken and given back together ------------------------------------------------ */

/*
 * BURST_DEPTH arenas held at once, then all returned, over and over. Where the round trip above
 * hands back the same arena every time and never leaves the cache line it sits on, this walks
 * the whole shelf before any of it comes home -- which is the shape a request handler has, and
 * the shape the stock was sized for.
 *
 * @p steps counts arenas rather than rounds, so the per step figure stays a per arena figure and
 * can be read against the section above it.
 */
static void burst(void *pool, int steps, int dynamic) {
  arnm *held[BURST_DEPTH];
  uint64_t sink = 0;
  int served = 0;

  while (served + BURST_DEPTH <= steps) {
    int taken = 0;
    for (; taken < BURST_DEPTH; ++taken) {
      arnm *arena = NULL;
      const arnm_result result =
          dynamic ? arnm_dynamic_arena_pool_alloc((arnm_dynamic_arena_pool *)pool, &arena)
                  : arnm_fixed_arena_pool_alloc((arnm_fixed_arena_pool *)pool, &arena);
      if (ARNM_SUCCESS != result) { break; }
      held[taken] = arena;
      sink += (uint64_t)(uintptr_t)arena;
    }
    for (int i = 0; i < taken; ++i) {
      if (dynamic) {
        arnm_dynamic_arena_pool_free((arnm_dynamic_arena_pool *)pool, held[i]);
      } else {
        arnm_fixed_arena_pool_free((arnm_fixed_arena_pool *)pool, held[i]);
      }
    }
    served += taken;
    if (taken < BURST_DEPTH) { break; }
  }

  g_sink += sink;
  require_all_steps(served, steps, "burst");
}

static void test_fixed_burst(int steps) {
  burst(&g_fixed_pool, steps, 0);
}
static void test_dynamic_stocked_burst(int steps) {
  burst(&g_dynamic_stocked, steps, 1);
}
static void test_dynamic_bare_burst(int steps) {
  burst(&g_dynamic_bare, steps, 1);
}

/* --- climbing the ladder ------------------------------------------------------------------- */

/**
 * Round trips against ladder @p index, asking for @p size bytes every time.
 *
 * Every rung holds an arena, so nothing here reaches the host and the figure is the search plus
 * the dynamic pool's own round trip -- which the section above already priced. Asking for the
 * top rung's capacity walks every grade; asking for the smallest stops at the first.
 *
 * A round trip pays for two lookups, not one: _alloc() finds the rung that reaches the request,
 * and _free() finds the rung the returned arena's capacity names. Neither one walks -- both
 * read arnm_graded_arena_pool::grade_bits -- so the four rows below are the claim that height
 * does not show, put where it can be checked rather than left in a header comment.
 *
 * @whisper Each rung counted on the way up, and the same ones again on the way down
 */
static void ladder(uint32_t index, uint32_t size, int steps) {
  uint64_t sink = 0;
  int i = 0;
  for (; i < steps; ++i) {
    arnm *arena = NULL;
    if (ARNM_SUCCESS != arnm_graded_arena_pool_alloc(&g_ladders[index], size, &arena)) { break; }
    sink += (uint64_t)(uintptr_t)arena;
    if (ARNM_SUCCESS != arnm_graded_arena_pool_free(&g_ladders[index], arena)) { break; }
  }
  g_sink += sink;
  require_all_steps(i, steps, "ladder");
}

static void test_ladder_2_top(int steps) {
  ladder(0, LADDER_RUNG_CAPACITY(g_ladder_grades[0] - 1), steps);
}
static void test_ladder_8_top(int steps) {
  ladder(1, LADDER_RUNG_CAPACITY(g_ladder_grades[1] - 1), steps);
}
static void test_ladder_16_top(int steps) {
  ladder(2, LADDER_RUNG_CAPACITY(g_ladder_grades[2] - 1), steps);
}
/* the same sixteen rung ladder, asked for a size the first rung already carries */
static void test_ladder_16_bottom(int steps) {
  ladder(2, 8, steps);
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

  printf(
      "\npooled arena %u B, burst depth %u, ladder rung i = %u B << i\n",
      (unsigned)POOL_ARENA_CAPACITY, (unsigned)BURST_DEPTH, (unsigned)ARNM_GRADED_MIN_SIZE
  );

  bench_section("one arena taken and given back -- " BENCH_ROUNDTRIPS " round trips");
  bench_step(test_fixed_roundtrip, ROUNDTRIP_COUNT, "  fixed pool", "trip");
  bench_step(test_dynamic_stocked_roundtrip, ROUNDTRIP_COUNT, "  dynamic pool, stocked", "trip");
  bench_step(test_dynamic_bare_roundtrip, ROUNDTRIP_COUNT, "  dynamic pool, no stock", "trip");
  bench_step(test_no_pool_roundtrip, ROUNDTRIP_COUNT, "  no pool, arena owns its block", "trip");

  bench_section(
      "the whole shelf held at once, then returned -- " BENCH_BURST " arenas, " BENCH_BURST_DEPTH
      " at a time"
  );
  bench_step(test_fixed_burst, BURST_ARENAS, "  fixed pool", "arena");
  bench_step(test_dynamic_stocked_burst, BURST_ARENAS, "  dynamic pool, stocked", "arena");
  bench_step(test_dynamic_bare_burst, BURST_ARENAS, "  dynamic pool, no stock", "arena");

  bench_section("the graded pool's lookup, every rung stocked -- " BENCH_LADDERS " round trips");
  bench_step(test_ladder_2_top, LADDER_COUNT, "  2 rungs, top asked for", "trip");
  bench_step(test_ladder_8_top, LADDER_COUNT, "  8 rungs, top asked for", "trip");
  bench_step(test_ladder_16_top, LADDER_COUNT, "  16 rungs, top asked for", "trip");
  bench_step(test_ladder_16_bottom, LADDER_COUNT, "  16 rungs, bottom asked for", "trip");

  /* the sections do not share a step count, so the closing line names none */
  bench_total_time(timeUsed);

  release_test_data();
  return 0;
}
