#include "arnm/graded_arena_pool.h"

#include "memory_intern.h"

#include "arnm/bitmap.h"
#include "arnm/dynamic_arena_pool.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A ladder is an array of dynamic pools in ascending capacity, plus one word saying which sizes
 * it holds. The sizes are not kept a second time: every grade carries its own in
 * `arena_capacity`, and `grade_bits` carries the same fact as a mask. Two figures that must
 * agree cannot disagree if one is derived from the other, so the mask is built once at init and
 * every lookup reads only it.
 *
 * The whole design rests on one restriction: a grade's capacity is a power of two, so its
 * exponent is a bit position. Bit e of `grade_bits` set means the ladder has a grade of 1 << e
 * bytes. That turns both lookups into arithmetic:
 *
 *   out: round the request up to a power of two, take its exponent e, mask off everything below
 *        e, and the lowest bit left is the grade that answers. Nothing above e is skipped and
 *        nothing below it can serve, so the first bit at or above e is the smallest grade that
 *        fits -- which is exactly what a walk from the bottom used to find one comparison at a
 *        time.
 *   back: the arena's capacity is already a power of two, so its exponent is its grade.
 *
 * Either way the array index is the number of set bits below that one: the grades are packed
 * with no gaps, so counting what comes before a bit counts the grades before it. That is the
 * one place arnm_popcount() is used in this library, and what it is for.
 *
 * The array comes from the host, like the arenas below it. A ladder whose grades grow on demand
 * has no fixed size to be carved out of somewhere.
 */

/** Bytes the grade array occupies. Cannot overflow: 65535 grades of a two-word struct. */
static inline uint32_t grades_bytes(uint16_t grade_count) {
  return (uint32_t)sizeof(arnm_dynamic_arena_pool) * (uint32_t)grade_count;
}

/** The state a pool is in before init and after release: holding nothing, promising nothing. */
static void forget_everything(arnm_graded_arena_pool *pool) {
  pool->grades = NULL;
  pool->grade_count = 0;
  pool->grade_bits = 0;
}

/** Is @p size a power of two? A ladder accepts nothing else, and 0 is not one. */
static bool is_power_of_two(uint32_t size) {
  return size && (size & (size - 1u)) == 0u;
}

/**
 * @p size rounded up to a power of two, branchless.
 *
 * Every bit below the highest set one is filled in, which leaves a run of ones ending where the
 * value did, and one more turns that run into the next power of two. Local rather than shared:
 * it is the one place in arnm that rounds to anything but 8, and it belongs to this rule.
 *
 * @param size Must be > 0 and at most ARNM_GRADED_MAX_SIZE; above that the answer would need a
 *             33rd bit and comes back as 0. Callers refuse that range before asking.
 */
static uint32_t ceil_power_of_two(uint32_t size) {
  size--;
  size |= size >> 1;
  size |= size >> 2;
  size |= size >> 4;
  size |= size >> 8;
  size |= size >> 16;
  return size + 1u;
}

/**
 * The smallest grade that can carry @p size, or grade_count when none can.
 *
 * A rounding step, a mask and a count -- the same work whether the ladder has two rungs or
 * twenty nine. @p size must be > 0.
 *
 * No bit scan appears here, though the obvious way to write it uses two. The grade that answers
 * is the lowest bit of @c at_or_above, and its slot is the number of grades below that bit --
 * but nothing between the rounded size and that bit is set, by the definition of lowest. So the
 * grades below it are exactly the grades the mask dropped, which the complement already names.
 * The scan and the shift it would feed both fall away.
 */
static uint16_t grade_for_size(const arnm_graded_arena_pool *pool, uint32_t size) {
  // above the tallest rung there could ever be; ceil_power_of_two() has no answer up there
  if (size > ARNM_GRADED_MAX_SIZE) { return pool->grade_count; }
  // every grade whose capacity reaches the request. A size under the smallest arena needs no
  // special case: the bits it would keep below exponent 3 are never set.
  const uint32_t at_or_above = pool->grade_bits & ~(ceil_power_of_two(size) - 1u);
  if (!at_or_above) { return pool->grade_count; }
  return (uint16_t)arnm_popcount(pool->grade_bits & ~at_or_above);
}

/** The grade whose capacity is exactly @p capacity, or grade_count when there is none. */
static uint16_t grade_for_capacity(const arnm_graded_arena_pool *pool, uint32_t capacity) {
  /* not a power of two means it was never one of ours, whatever else it may be */
  if (!is_power_of_two(capacity)) { return pool->grade_count; }

  /* the grades below this size, counted: the slot a grade of this size would occupy */
  const uint16_t slot = (uint16_t)arnm_popcount(pool->grade_bits & (capacity - 1u));

  /* Which is not yet proof that it is there. A capacity the ladder has no rung for lands on the
     next rung up, or past the last one -- so the grade found has to say its own size back. That
     read costs nothing on top: it is the same cache line the grade is about to be used from. */
  if (slot >= pool->grade_count || pool->grades[slot].arena_capacity != capacity) {
    return pool->grade_count;
  }
  return slot;
}

// ********** manage the pool itself *******************

arnm_result arnm_graded_arena_pool_init(
    arnm_graded_arena_pool *pool,
    const uint32_t *sizes,
    uint16_t grade_count,
    uint32_t spare_per_grade
) {
  if (!pool || !sizes) { return ARNM_ERROR_NULL_POINTER; }
  if (!grade_count) { return ARNM_ERROR_INVALID_PARAM; }

  // there are only 29 powers of two in the accepted range, so a longer list has a repeat in it
  // somewhere and the ascending check below would catch it -- this says so before the loop
  if (grade_count > ARNM_GRADED_MAX_GRADE_COUNT) { return ARNM_ERROR_INVALID_PARAM; }

  // every size is judged before the host is asked for anything, so a refusal costs nothing and
  // leaves nothing to unwind. The mask is built here as well: it is the same pass.
  uint32_t grade_bits = 0;
  uint32_t previous = 0;
  for (uint16_t i = 0; i < grade_count; i++) {
    const uint32_t capacity = sizes[i];
    // nothing is rounded: a ladder is what it says it is, and a size that is not a rung is a
    // mistake in the caller's list rather than a number to be repaired
    if (!is_power_of_two(capacity)) { return ARNM_ERROR_INVALID_PARAM; }
    if (capacity < ARNM_GRADED_MIN_SIZE || capacity > ARNM_GRADED_MAX_SIZE) {
      return ARNM_ERROR_INVALID_PARAM;
    }
    // a grade equal to or below the one before it would sit on the ladder without a request
    // able to reach it, and the mask could not tell the two apart at all
    if (capacity <= previous) { return ARNM_ERROR_INVALID_PARAM; }
    previous = capacity;
    grade_bits |= 1u << arnm_ctz(capacity);
  }

  arnm_dynamic_arena_pool *grades = NULL;
  const arnm_result allocated = arnm_alloc((uint8_t **)&grades, grades_bytes(grade_count), NULL);
  if (ARNM_SUCCESS != allocated) { return allocated; }

  // the sizes passed above, so no grade can refuse and the loop needs no way back
  for (uint16_t i = 0; i < grade_count; i++) {
    arnm_dynamic_arena_pool_init(&grades[i], sizes[i], spare_per_grade);
  }

  pool->grades = grades;
  pool->grade_count = grade_count;
  pool->grade_bits = grade_bits;
  return ARNM_SUCCESS;
}

arnm_graded_arena_pool *arnm_graded_arena_pool_create(
    const uint32_t *sizes, uint16_t grade_count, uint32_t spare_per_grade, arnm *allocator
) {
  arnm_graded_arena_pool *pool = NULL;
  // `allocator` carries this descriptor and nothing else; the ladder and its arenas are the
  // host's
  if (ARNM_SUCCESS != arnm_alloc((uint8_t **)&pool, sizeof(arnm_graded_arena_pool), allocator)) {
    return NULL;
  }
  if (ARNM_SUCCESS != arnm_graded_arena_pool_init(pool, sizes, grade_count, spare_per_grade)) {
    // straight back to where it came from; it is still the tail there, so an arena takes it
    arnm_free((uint8_t *)pool, sizeof(arnm_graded_arena_pool), allocator);
    return NULL;
  }
  return pool;
}

arnm_result arnm_graded_arena_pool_release(arnm_graded_arena_pool *pool) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->grades) {
    forget_everything(pool);
    return ARNM_SUCCESS;
  }

  // asked in full before anything is taken down: a ladder half released would be neither
  // released nor usable, and one grade in use is reason enough to leave all of them standing
  for (uint16_t i = 0; i < pool->grade_count; i++) {
    if (pool->grades[i].acquired_count) { return ARNM_ERROR_RESOURCE_IN_USE; }
  }
  for (uint16_t i = 0; i < pool->grade_count; i++) {
    arnm_dynamic_arena_pool_release(&pool->grades[i]);
  }

  uint8_t *grades = (uint8_t *)pool->grades;
  const uint32_t bytes = grades_bytes(pool->grade_count);

  // emptied first, so the pool is never left pointing at an array it has let go of
  forget_everything(pool);
  return arnm_free(grades, bytes, NULL);
}

arnm_result arnm_graded_arena_pool_destroy(arnm_graded_arena_pool *pool, arnm *allocator) {
  // nothing to give back is not a failure
  if (!pool) { return ARNM_SUCCESS; }

  const arnm_result released = arnm_graded_arena_pool_release(pool);
  // the descriptor outlives a refusal on purpose: the caller still has arenas to return and
  // needs the pool to return them to
  if (ARNM_ERROR_RESOURCE_IN_USE == released) { return released; }

  const arnm_result freed = arnm_free((uint8_t *)pool, sizeof(arnm_graded_arena_pool), allocator);
  // a warning from either step is worth more to the caller than the success of the other
  return (ARNM_SUCCESS != released) ? released : freed;
}

uint32_t arnm_graded_arena_pool_capacity_for(const arnm_graded_arena_pool *pool, uint32_t size) {
  if (!pool || !pool->grades || !size) { return 0; }
  const uint16_t index = grade_for_size(pool, size);
  return (index < pool->grade_count) ? pool->grades[index].arena_capacity : 0u;
}

arnm_dynamic_arena_pool *arnm_graded_arena_pool_grade_at(
    const arnm_graded_arena_pool *pool, uint16_t index
) {
  if (!pool || !pool->grades || index >= pool->grade_count) { return NULL; }
  return &pool->grades[index];
}

uint64_t arnm_graded_arena_pool_reserved(const arnm_graded_arena_pool *pool) {
  if (!pool || !pool->grades) { return 0; }
  uint64_t total = grades_bytes(pool->grade_count);
  for (uint16_t i = 0; i < pool->grade_count; i++) {
    total += arnm_dynamic_arena_pool_reserved(&pool->grades[i]);
  }
  return total;
}

// ********** lend and take back *******************

arnm_result arnm_graded_arena_pool_alloc(arnm_graded_arena_pool *pool, uint32_t size, arnm **out) {
  if (!pool || !out) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->grades) { return ARNM_ERROR_NOT_INITIALIZED; }
  if (!size) { return ARNM_ERROR_INVALID_PARAM; }

  const uint16_t index = grade_for_size(pool, size);
  // past the last rung. Nothing coming back would help, so this is not RESOURCE_EXHAUSTED: the
  // ladder ends where init left it.
  if (index >= pool->grade_count) { return ARNM_ERROR_RESOURCE_SIZE_EXCEED; }

  return arnm_dynamic_arena_pool_alloc(&pool->grades[index], out);
}

arnm_result arnm_graded_arena_pool_free(arnm_graded_arena_pool *pool, arnm *arena) {
  if (!pool || !arena) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->grades) { return ARNM_ERROR_NOT_INITIALIZED; }
  // a chain or a host handle carries no capacity to match, and a released arena carries 0 --
  // both fall to the same refusal below, which is the honest one either way
  if (!is_single_arena(arnm_intern_of_const(arena))) { return ARNM_ERROR_INVALID_PARAM; }

  // the arena says which grade it belongs to, so nothing had to be remembered between the two
  // calls; a capacity is a power of two and names at most one grade
  const uint16_t index = grade_for_capacity(pool, arnm_intern_of_const(arena)->capacity);
  if (index >= pool->grade_count) { return ARNM_ERROR_INVALID_PARAM; }

  return arnm_dynamic_arena_pool_free(&pool->grades[index], arena);
}
