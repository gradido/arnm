#include "arnm/arena.h"
#include "arnm/bucket_vector.h"
#include "arnm/fixed_ring.h"
#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * Three ways to hold a sequence, measured against each other.
 *
 * A bucket vector trades one indirection on random access for two things a flat, doubling
 * array cannot give: appends that never copy what is already stored, and element addresses
 * that stay valid forever. The steps below put numbers on both sides of that trade --
 * append throughput, traversal, random access, and the cost of the bucket size itself.
 *
 * A fixed ring gives up growth entirely and buys back a queue that does not accumulate. The
 * queue section is where the two meet -- but only for the pattern they share, a window filled
 * and emptied all at once. A queue consumed continuously is not something a bucket vector does
 * at all, so that row is a ring's alone, and what the vector would cost is reported in memory
 * and in the point where it stops accepting rather than in nanoseconds it never earned.
 */

#define ELEMENT_COUNT 4000000

/** 64 byte payload -- the size range where copying on growth really starts to hurt. */
typedef struct bench_payload {
  uint64_t id;
  uint64_t timestamp;
  uint8_t blob[48];
} bench_payload;

/* The bucket size is an _init argument now rather than part of the type, so each accessor set
   is paired with the exponent its vectors are opened with. Every vector is an arnm_bvec; only
   the generated accessors differ, and they are what keeps the element type straight. */
ARNM_BVEC_DEFINE(bvec_u64, uint64_t)
ARNM_BVEC_DEFINE(bvec_payload, bench_payload)
ARNM_FIXED_RING_DEFINE(ring_u64, uint64_t)

/**
 * Elements in flight at once in the queue section, while ELEMENT_COUNT of them pass through.
 *
 * Small enough that the window itself stays in cache on both sides, so the section reports what
 * the containers do rather than what the working set does. What it is not small enough for is
 * the bucket vector's footprint: 4 M elements enter, and a container that only ever appends
 * keeps every one of them.
 */
#define QUEUE_WINDOW 4096

/*
 * The index array holds at most ARNM_BVEC_MAX_INDEX_CAPACITY bucket pointers, so a vector
 * tops out at that many buckets times its bucket capacity. At ELEMENT_COUNT elements that
 * puts a floor under the exponent -- 4 M uint64 need 4 M / 8191 = 489 per bucket, so nothing
 * below 2^9 can hold the run at all. The bucket size section therefore compares 4 KiB, 16 KiB
 * and 64 KiB rather than starting at 256 B: the smaller buckets are not a slower way to store
 * 4 M elements, they cannot store them.
 */
#define BVEC_U64_LOG2 9        /* 512 * 8 B = 4 KiB buckets */
#define BVEC_U64_MID_LOG2 11   /* 2048 * 8 B = 16 KiB buckets */
#define BVEC_U64_LARGE_LOG2 13 /* 8192 * 8 B = 64 KiB buckets */
#define BVEC_PAYLOAD_LOG2 10   /* 1024 * 64 B = 64 KiB buckets */

static_assert(
    (uint64_t)ARNM_BVEC_MAX_INDEX_CAPACITY << BVEC_U64_LOG2 >= ELEMENT_COUNT,
    "the smallest bucket size in this file must still hold a whole run"
);
static_assert(
    (uint64_t)ARNM_BVEC_MAX_INDEX_CAPACITY << BVEC_PAYLOAD_LOG2 >= ELEMENT_COUNT,
    "same for the payload vector"
);

/* index_grow_step_size 0 takes the library's default, which is what these steps want to time */
#define BVEC_GROW_DEFAULT 0

/** Prefilled sources for the read-side steps; filled once before the benchmarks run. */
static arnm_bvec g_filled;
static uint64_t *g_flat = NULL;
/** Kept across clear/refill so the step measures reuse of already allocated buckets. */
static arnm_bvec g_reused;
/** Arena large enough for ELEMENT_COUNT uint64 plus bucket index; reset per step. */
static arnm g_arena;
/** Consumes every value read so the compiler cannot drop the traversal steps. */
static volatile uint64_t g_sink = 0;
/** Prefilled ring for the read-side steps, the same ELEMENT_COUNT values as g_filled. */
static arnm_fixed_ring g_filled_ring;
/** What the ring still held when its queue step finished, ready for the report below it. */
static char g_ring_queue_bytes_string[32] = "";

/**
 * Stop on a failed setup instead of measuring the wreckage.
 *
 * A reserve that fails leaves the vector empty, and the pushes that follow fail with it --
 * the step would finish suspiciously fast and be reported as a result. Checked once per step,
 * never inside a measured loop, so the timing stays untouched.
 */
static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS == result) { return; }
  fprintf(stderr, "benchmark setup failed: %s: %s\n", what, arnm_result_to_string(result));
  exit(EXIT_FAILURE);
}

/**
 * Append @p count values and report the first refusal.
 *
 * The loop carries the check rather than ignoring the result: a vector that runs into its
 * bucket cap stops storing and keeps returning, which would show up as a very fast step and be
 * printed as a measurement. One predictable compare per element is the price of that not
 * happening quietly.
 */
static arnm_result push_all(arnm_bvec *v, int count) {
  arnm_result result = ARNM_SUCCESS;
  for (int i = 0; i < count && ARNM_SUCCESS == result; ++i) {
    result = bvec_u64_push(v, (uint64_t)i);
  }
  return result;
}

/* --- append -----------------------------------------------------------------------------
 *
 * Every step here is a whole life: init, whatever it reserves, the appends, and the free. That
 * keeps the rows comparable with each other, and it is worth knowing before reading any single
 * one of them -- an append figure includes giving 32 MiB back.
 *
 * The reserved row is where that used to matter, and the history is worth keeping because both
 * causes were invisible in the row itself. With the phases timed apart it read reserve 10.7 ms,
 * append 21.9 ms, free 33.2 ms -- and the append was the smallest of the three.
 *
 * The free was glibc trimming the heap. A freed chunk adjacent to the top chunk merges into it,
 * and once top passes the trim threshold every further merge shrinks the heap with brk(), the
 * pages coming back as faults the next time they are used. Releasing newest first walks the top
 * downwards and pays that per bucket; oldest first reaches the top once. A vector that grew
 * into its buckets was never exposed to it, because its index array is reallocated last and
 * sits above every bucket -- the same call, and only the history differing. 0.7.6 picks the
 * order by allocator, and the phase went to 2.5 ms.
 *
 * The append was arnm's own: _grow derived the count of held buckets by scanning the index
 * array, twice per bucket, which a reserved vector paid quadratically. That is a field now and
 * the phase went to 7.8 ms.
 *
 * What is left in the row is 32 MiB of first-touch page faults, which every variant pays and
 * only the phase it lands in differs.
 */

static void test_bvec_push(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(push_all(&v, stepCount), "push");
  bvec_u64_free(&v);
}

static void test_bvec_push_reserved(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_u64_reserve(&v, (uint32_t)stepCount), "reserve");
  require_ok(push_all(&v, stepCount), "push");
  bvec_u64_free(&v);
}

static void test_bvec_push_arena(int stepCount) {
  arnm_bvec v;
  arnm_reset(&g_arena);
  bvec_u64_init(&v, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, &g_arena);
  require_ok(bvec_u64_reserve(&v, (uint32_t)stepCount), "reserve");
  require_ok(push_all(&v, stepCount), "push");
  bvec_u64_free(&v);
}

/** The reference: a flat array that doubles and copies everything it already holds. */
static void test_flat_push(int stepCount) {
  uint64_t *flat = NULL;
  size_t capacity = 0, length = 0;
  for (int i = 0; i < stepCount; ++i) {
    if (length == capacity) {
      capacity = capacity ? capacity * 2 : 512;
      flat = (uint64_t *)realloc(flat, capacity * sizeof(uint64_t));
      if (!flat) return;
    }
    flat[length++] = (uint64_t)i;
  }
  free(flat);
}

/** Same appends, but into a vector whose buckets already exist -- clear keeps them. */
static void test_bvec_refill_after_clear(int stepCount) {
  require_ok(push_all(&g_reused, stepCount), "push into reused");
  bvec_u64_clear(&g_reused);
}

/* --- bucket size ------------------------------------------------------------------------ */

static void test_bvec_push_16kb_buckets(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_MID_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(push_all(&v, stepCount), "push");
  bvec_u64_free(&v);
}

static void test_bvec_push_64kb_buckets(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_LARGE_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(push_all(&v, stepCount), "push");
  bvec_u64_free(&v);
}

/* --- payload ---------------------------------------------------------------------------- */

/** 64 byte payload passed by value: written twice, once to the stack, once into the bucket. */
static void test_payload_push_by_value(int stepCount) {
  arnm_bvec v;
  bench_payload p;
  bvec_payload_init(&v, BVEC_PAYLOAD_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_payload_reserve(&v, (uint32_t)stepCount), "reserve payload");
  memset(&p, 0, sizeof(p));
  for (int i = 0; i < stepCount; ++i) {
    p.id = (uint64_t)i;
    require_ok(bvec_payload_push(&v, p), "push payload");
  }
  bvec_payload_free(&v);
}

/** The same payload built directly in its final slot -- one write instead of two. */
static void test_payload_emplace(int stepCount) {
  arnm_bvec v;
  bench_payload *slot;
  bvec_payload_init(&v, BVEC_PAYLOAD_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_payload_reserve(&v, (uint32_t)stepCount), "reserve payload");
  for (int i = 0; i < stepCount; ++i) {
    // stops the run rather than the loop: leaving early would hand bench_step a step that did
    // less work than the count it divides by, and the row would print as the fastest here
    require_ok(bvec_payload_emplace(&v, &slot), "emplace payload");
    memset(slot, 0, sizeof(*slot));
    slot->id = (uint64_t)i;
  }
  bvec_payload_free(&v);
}

/* --- traversal and access --------------------------------------------------------------- */

/** Bucket by bucket: contiguous memory, no index lookup per element. */
static void test_bvec_iterate_buckets(int stepCount) {
  uint64_t sum = 0;
  (void)stepCount;
  for (uint16_t b = 0, buckets = bvec_u64_bucket_count(&g_filled); b < buckets; ++b) {
    const uint64_t *data = bvec_u64_bucket_data(&g_filled, b);
    const uint32_t count = bvec_u64_bucket_size(&g_filled, b);
    for (uint32_t k = 0; k < count; ++k) sum += data[k];
  }
  g_sink += sum;
}

/** Flat traversal by index: bucket and offset are recomputed for every element. */
static void test_bvec_iterate_indexed(int stepCount) {
  uint64_t sum = 0;
  (void)stepCount;
  for (uint32_t i = 0, count = bvec_u64_size(&g_filled); i < count; ++i) {
    sum += *bvec_u64_get(&g_filled, i);
  }
  g_sink += sum;
}

/** The ring by index: one addition, one comparison, and a load out of a single block. */
static void test_ring_iterate_indexed(int stepCount) {
  uint64_t sum = 0;
  (void)stepCount;
  for (uint32_t i = 0, count = ring_u64_size(&g_filled_ring); i < count; ++i) {
    sum += *ring_u64_get(&g_filled_ring, i);
  }
  g_sink += sum;
}

static void test_flat_iterate(int stepCount) {
  uint64_t sum = 0;
  for (int i = 0; i < stepCount; ++i) sum += g_flat[i];
  g_sink += sum;
}

/** Scattered reads -- the case that pays for the extra indirection.
 *
 * The index runs in uint32_t here and in the flat reference alike: the modulo is the hottest
 * instruction in both loops, and a 64 bit divisor would put the difference in the step
 * instead of in the container. */
static void test_bvec_random_access(int stepCount) {
  uint64_t sum = 0;
  uint32_t index = 0;
  const uint32_t size = bvec_u64_size(&g_filled);
  for (int i = 0; i < stepCount; ++i) {
    index = (index + 524287) % size; /* prime stride: defeats the prefetcher */
    sum += *bvec_u64_get(&g_filled, index);
  }
  g_sink += sum;
}

static void test_ring_random_access(int stepCount) {
  uint64_t sum = 0;
  uint32_t index = 0;
  const uint32_t size = ring_u64_size(&g_filled_ring);
  for (int i = 0; i < stepCount; ++i) {
    index = (index + 524287) % size;
    sum += *ring_u64_get(&g_filled_ring, index);
  }
  g_sink += sum;
}

static void test_flat_random_access(int stepCount) {
  uint64_t sum = 0;
  uint32_t index = 0;
  const uint32_t size = (uint32_t)ELEMENT_COUNT;
  for (int i = 0; i < stepCount; ++i) {
    index = (index + 524287) % size;
    sum += g_flat[index];
  }
  g_sink += sum;
}

/** Pointers taken before growth stay valid -- this is what the indirection buys. */
static void test_bvec_push_pop_cycle(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_u64_reserve(&v, (uint32_t)stepCount), "reserve");
  require_ok(push_all(&v, stepCount), "push");
  for (int i = 0; i < stepCount; ++i) bvec_u64_pop(&v);
  bvec_u64_free(&v);
}

/* --- queue ------------------------------------------------------------------------------ */

/**
 * Bytes a bucket vector holds: its buckets, plus the index array of pointers to them.
 *
 * Only the allocated buckets count, not the elements in them -- a bucket is taken whole and
 * stays taken until _shrink() or _free(), which is exactly what makes the figure interesting
 * next to a ring that never takes a second one.
 *
 * `allocated_count` and not `_bucket_count()`: the second answers how far the elements reach,
 * and a vector that has been popped empty reports 0 there while still holding every bucket it
 * ever opened. The two agree in the queue below, which never pops, and the one that stays right
 * when they part is the one this asks.
 */
static uint32_t bvec_bytes_held(const arnm_bvec *v, uint8_t bucket_capacity_log2) {
  const uint32_t buckets = v->allocated_count;
  return (buckets << bucket_capacity_log2) * (uint32_t)sizeof(uint64_t) +
         (uint32_t)v->bucket_capacity * (uint32_t)sizeof(void *);
}

/** Bytes as KiB or MiB, one decimal -- the range a queue's footprint lands in. */
static void bench_bytes_string(char *buffer, size_t buffer_size, uint32_t bytes) {
  if (bytes < 1024u * 1024u) {
    snprintf(buffer, buffer_size, "%.1f KiB", (double)bytes / 1024.0);
  } else {
    snprintf(buffer, buffer_size, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
  }
}

/**
 * A ring in its steady state: the window stays full while everything passes through it.
 *
 * Read the front, step over it, put one on the back -- the shape a mail queue or an expiry
 * queue runs in. The slot the front just left is the one the next push takes, so the footprint
 * is what QUEUE_WINDOW asked for and stays there for the whole run.
 */
static void test_ring_queue_steady(int stepCount) {
  arnm_fixed_ring r;
  require_ok(ring_u64_init(&r, QUEUE_WINDOW, NULL), "init ring");
  for (int i = 0; i < QUEUE_WINDOW; ++i) { require_ok(ring_u64_push(&r, (uint64_t)i), "fill"); }
  for (int i = QUEUE_WINDOW; i < stepCount; ++i) {
    g_sink += *ring_u64_front(&r);
    require_ok(ring_u64_pop(&r), "pop");
    require_ok(ring_u64_push(&r, (uint64_t)i), "push");
  }
  bench_bytes_string(
      g_ring_queue_bytes_string, sizeof(g_ring_queue_bytes_string), ring_u64_reserved(&r)
  );
  require_ok(ring_u64_free(&r, NULL), "free ring");
}

/**
 * The other pattern, where a round ends all at once and the bucket vector is at home.
 *
 * Fill the window, read it out, empty the container, start again. `_clear()` keeps every bucket
 * for the next round, so this allocates once and then costs nothing -- and the ring, which
 * allocated once at init, has no advantage left to have. Printed beside the steady state so the
 * queue section does not read as a verdict on the container.
 */
static void test_bvec_queue_rounds(int stepCount) {
  arnm_bvec v;
  bvec_u64_init(&v, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_u64_reserve(&v, QUEUE_WINDOW), "reserve");
  for (int i = 0; i + QUEUE_WINDOW <= stepCount; i += QUEUE_WINDOW) {
    for (int k = 0; k < QUEUE_WINDOW; ++k) {
      require_ok(bvec_u64_push(&v, (uint64_t)(i + k)), "push");
    }
    for (uint32_t k = 0; k < QUEUE_WINDOW; ++k) { g_sink += *bvec_u64_get(&v, k); }
    bvec_u64_clear(&v);
  }
  bvec_u64_free(&v);
}

static void test_ring_queue_rounds(int stepCount) {
  arnm_fixed_ring r;
  require_ok(ring_u64_init(&r, QUEUE_WINDOW, NULL), "init ring");
  for (int i = 0; i + QUEUE_WINDOW <= stepCount; i += QUEUE_WINDOW) {
    for (int k = 0; k < QUEUE_WINDOW; ++k) {
      require_ok(ring_u64_push(&r, (uint64_t)(i + k)), "push");
    }
    for (uint32_t k = 0; k < QUEUE_WINDOW; ++k) { g_sink += *ring_u64_get(&r, k); }
    ring_u64_clear(&r);
  }
  require_ok(ring_u64_free(&r, NULL), "free ring");
}

/* --- driver ----------------------------------------------------------------------------- */

static void prepare_test_data(void) {
  g_flat = (uint64_t *)malloc((size_t)ELEMENT_COUNT * sizeof(uint64_t));
  if (!g_flat) { require_ok(ARNM_ERROR_OUT_OF_MEMORY, "flat reference array"); }
  bvec_u64_init(&g_filled, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_u64_reserve(&g_filled, (uint32_t)ELEMENT_COUNT), "reserve filled");
  for (int i = 0; i < ELEMENT_COUNT; ++i) {
    require_ok(bvec_u64_push(&g_filled, (uint64_t)i), "fill source");
    g_flat[i] = (uint64_t)i;
  }
  /* the same values in a ring, for the read side: full to the brim, so the wrap sits at the
     end of the block and an index walk crosses it exactly once */
  require_ok(ring_u64_init(&g_filled_ring, (uint32_t)ELEMENT_COUNT, NULL), "init filled ring");
  for (int i = 0; i < ELEMENT_COUNT; ++i) {
    require_ok(ring_u64_push(&g_filled_ring, (uint64_t)i), "fill ring");
  }
  /* filled once, then cleared: the refill step finds every bucket already in place */
  bvec_u64_init(&g_reused, BVEC_U64_LOG2, BVEC_GROW_DEFAULT, NULL);
  require_ok(bvec_u64_reserve(&g_reused, (uint32_t)ELEMENT_COUNT), "reserve reused");
  bvec_u64_clear(&g_reused);
  /* payload for the index array included, so no step runs the arena dry */
  /* Without this a failed init would leave g_arena zeroed, which is default mode: the
     arena steps would quietly measure malloc and still be labelled "arena". */
  require_ok(
      arnm_init_arena(&g_arena, (uint32_t)ELEMENT_COUNT * sizeof(uint64_t) + 1024 * 1024),
      "init arena"
  );
}

static void release_test_data(void) {
  require_ok(ring_u64_free(&g_filled_ring, NULL), "free filled ring");
  bvec_u64_free(&g_filled);
  bvec_u64_free(&g_reused);
  arnm_release(&g_arena);
  free(g_flat);
}

int main(void) {
  arnm_mono_timer timeUsed;
  const int stepCount = ELEMENT_COUNT;

  if (!bench_timer_start(&timeUsed)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(timeUsed);

  bench_section("append");
  bench_step(test_bvec_push, stepCount, "  bucket vector push", "element");
  bench_step(test_bvec_push_reserved, stepCount, "  bucket vector push, reserved", "element");
  bench_step(test_bvec_push_arena, stepCount, "  bucket vector push, arena", "element");
  bench_step(
      test_bvec_refill_after_clear, stepCount, "  bucket vector refill after clear", "element"
  );
  bench_step(test_flat_push, stepCount, "  flat array push, doubling realloc", "element");
  bench_step(test_bvec_push_pop_cycle, stepCount, "  bucket vector push + pop cycle", "element");

  bench_section("bucket size (same 4 M appends)");
  bench_step(test_bvec_push, stepCount, "  4 KiB buckets", "element");
  bench_step(test_bvec_push_16kb_buckets, stepCount, "  16 KiB buckets", "element");
  bench_step(test_bvec_push_64kb_buckets, stepCount, "  64 KiB buckets", "element");

  bench_section("64 byte payload");
  bench_step(test_payload_push_by_value, stepCount, "  push by value", "element");
  bench_step(test_payload_emplace, stepCount, "  emplace in place", "element");

  bench_section("queue, 4 M elements through a 4096 element window");
  /* Three rows and no fourth. The first is what a queue costs the container built for one; the
     other two are the round based pattern, which both containers really do and which is where a
     comparison between them means something. What a bucket vector does when it is asked to be
     the first row is underneath, in the only terms it can honestly be put. */
  bench_step(test_ring_queue_steady, stepCount, "  fixed ring, steady state", "element");
  bench_step(test_ring_queue_rounds, stepCount, "  fixed ring, in rounds", "element");
  bench_step(test_bvec_queue_rounds, stepCount, "  bucket vector, cleared per round", "element");

  bench_section("traversal");
  bench_step(test_bvec_iterate_buckets, stepCount, "  bucket vector, bucket wise", "element");
  bench_step(test_bvec_iterate_indexed, stepCount, "  bucket vector, by index", "element");
  bench_step(test_ring_iterate_indexed, stepCount, "  fixed ring, by index", "element");
  bench_step(test_flat_iterate, stepCount, "  flat array", "element");

  bench_section("random access");
  bench_step(test_bvec_random_access, stepCount, "  bucket vector", "element");
  bench_step(test_ring_random_access, stepCount, "  fixed ring", "element");
  bench_step(test_flat_random_access, stepCount, "  flat array", "element");

  bench_total(timeUsed, stepCount, "element");

  release_test_data();
  return 0;
}
