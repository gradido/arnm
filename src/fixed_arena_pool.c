#include "arnm/fixed_arena_pool.h"

#include "memory_intern.h"

#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * One allocation holds the whole pool:
 *
 *   [ arnm descriptor 0 .. N-1 ][ pad to 8 ][ buffer 0 ][ buffer 1 ] ... [ buffer N-1 ]
 *
 * The descriptors come first so that the block's address and the descriptor array's address are
 * the same pointer -- `arenas` is both, which is what lets _release() hand everything back
 * without storing a second pointer. Each buffer is `arena_capacity` bytes, that figure already
 * rounded up to 8, so every buffer starts 8 byte aligned and arnm_init_arena_borrow() accepts
 * it as it is.
 *
 * The free list threads through the buffers rather than through the descriptors: a free arena's
 * bytes are dead until it is lent out, and its first sizeof(arnm *) of them carry the address
 * of the next free arena. Written and read with memcpy, because a uint8_t buffer is not a
 * arnm * and pretending otherwise is the kind of aliasing a sanitizer is right to complain
 * about. The pointer fits: capacities are rounded up to 8, and the assert below settles that 8
 * is enough on this target.
 */

static_assert(
    sizeof(arnm *) <= 8, "the free list link has to fit in the smallest arena, which is 8 bytes"
);
static_assert(sizeof(arnm) <= UINT16_MAX - 1, "arnm has an unreasonable size");
static_assert(ARNM_ALIGN8(sizeof(arnm)) == sizeof(arnm), "arnm struct must be 8-Byte aligned");

/** Where the buffers begin, measured from the front of the block. */
static inline uint64_t buffers_offset(uint16_t arena_count) {
  return (uint64_t)arena_count * sizeof(arnm);
}

/** Bytes the single block occupies. Recomputed rather than stored; the inputs never change. */
static inline uint64_t block_bytes(const arnm_fixed_arena_pool *pool) {
  if (!pool) { return 0; }
  return buffers_offset(pool->arena_count) + (uint64_t)pool->arena_count * pool->arena_capacity;
}

/* The link goes into the arena's buffer, never into `arena->bytes` -- that is the descriptor
   itself, and its first eight bytes are the pointer to the buffer. Writing the link there
   overwrites exactly what makes the arena an arena, and the first allocation out of it lands
   on whatever address the link happened to hold. Hence the reach into the private layout. */
static uint8_t *link_slot(const arnm *arena) {
  return arnm_intern_of_const(arena)->data;
}

/** Read the link a free arena carries in the first bytes of its buffer. */
static arnm *next_free(const arnm *arena) {
  arnm *next = NULL;
  memcpy(&next, link_slot(arena), sizeof(arnm *));
  return next;
}

/** Write the link into a free arena's buffer. Only ever called on an arena the pool holds. */
static void set_next_free(arnm *arena, arnm *next) {
  memcpy(link_slot(arena), &next, sizeof(arnm *));
}

/* An arena has to be able to carry the link while it is free. _init rounds the capacity up to
   8 and refuses 0, so the smallest arena a pool can have is exactly wide enough. */
static_assert(sizeof(arnm *) <= 8, "a free arena's buffer must be able to hold the link");

/** True when @p arena is one of @p pool's, addressed exactly at a slot and not between two. */
static bool belongs_to(const arnm_fixed_arena_pool *pool, const arnm *arena) {
  if (arena < pool->arenas || arena >= pool->arenas + pool->arena_count) { return false; }
  const size_t offset = (size_t)((const uint8_t *)arena - (const uint8_t *)pool->arenas);
  return offset % sizeof(arnm) == 0;
}

/** The state a pool is in before init and after release: owning nothing, promising nothing. */
static void forget_everything(arnm_fixed_arena_pool *pool) {
  pool->arenas = NULL;
  pool->free_head = NULL;
  pool->arena_capacity = 0;
  pool->arena_count = 0;
  pool->acquired_count = 0;
}

// ********** manage the pool itself *******************

arnm_result arnm_fixed_arena_pool_init(
    arnm_fixed_arena_pool *pool, uint32_t arena_capacity, uint16_t arena_count, arnm *source
) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!arena_capacity || !arena_count) { return ARNM_ERROR_INVALID_PARAM; }

  uint32_t capacity = arnm_align8_u32(arena_capacity);
  if (!capacity) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  uint32_t arena_count_u32 = (uint32_t)arena_count;
  uint32_t descriptors_size = (uint32_t)sizeof(arnm) * arena_count_u32;
  if ((ARNM_MAX_ALLOC_SIZE - descriptors_size) / arena_count_u32 < capacity) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }

  uint32_t total = ((uint32_t)sizeof(arnm) + capacity) * arena_count_u32;
  // one block for everything, so one call gets it and one call gives it back
  uint8_t *block = NULL;
  arnm_result result = arnm_alloc(&block, total, source);
  if (ARNM_SUCCESS != result) { return result; }

  arnm *descriptors = (arnm *)block;
  uint8_t *buffers = block + buffers_offset(arena_count);

  // strung back to front, so the head is arena 0 and a run of allocations walks the block in
  // order -- the same work either way, and far easier to follow in a debugger

  arnm *head = NULL;
  for (uint32_t i = arena_count; i > 0; --i) {
    arnm *arena = &descriptors[i - 1];
    uint8_t *buffer = buffers + (uint64_t)(i - 1) * capacity;

    result = arnm_init_arena_borrow(arena, buffer, capacity);
    if (ARNM_SUCCESS != result) {
      // unreachable with a layout this file laid out itself; handled rather than assumed, and
      // the block goes straight back so a failure leaves nothing behind
      arnm_free(block, (uint32_t)total, source);
      return result;
    }
    set_next_free(arena, head);
    head = arena;
  }

  // nothing above can fail from here on, so the descriptor is written only now: a refused init
  // leaves whatever the caller had
  pool->arenas = descriptors;
  pool->free_head = head;
  pool->arena_capacity = capacity;
  pool->arena_count = arena_count;
  pool->acquired_count = 0;
  return ARNM_SUCCESS;
}

arnm_fixed_arena_pool *arnm_fixed_arena_pool_create(
    uint32_t arena_capacity, uint16_t arena_count, arnm *source, arnm *allocator
) {
  arnm_fixed_arena_pool *pool = NULL;
  // `allocator` carries this descriptor, `source` the arenas -- two questions, two answers
  if (ARNM_SUCCESS != arnm_alloc((uint8_t **)&pool, sizeof(arnm_fixed_arena_pool), allocator)) {
    return NULL;
  }
  if (ARNM_SUCCESS != arnm_fixed_arena_pool_init(pool, arena_capacity, arena_count, source)) {
    // straight back to where it came from; it is still the tail there, so an arena takes it
    arnm_free((uint8_t *)pool, sizeof(arnm_fixed_arena_pool), allocator);
    return NULL;
  }
  return pool;
}

arnm_result arnm_fixed_arena_pool_release(arnm_fixed_arena_pool *pool, arnm *source) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  // the one refusal that matters: an arena still out is memory someone is writing to
  if (pool->acquired_count) { return ARNM_ERROR_RESOURCE_IN_USE; }
  if (!pool->arenas) {
    forget_everything(pool);
    return ARNM_SUCCESS;
  }

  // the size is recomputed from what the pool holds, the allocator comes from the caller -- the
  // same split every free in this library uses, and the same duty it puts on the caller
  const uint32_t total = (uint32_t)block_bytes(pool);
  uint8_t *block = (uint8_t *)pool->arenas;

  // emptied first: whatever the source answers, the pool has let go of the block and must not
  // be left pointing at it
  forget_everything(pool);
  return arnm_free(block, total, source);
}

arnm_result arnm_fixed_arena_pool_destroy(
    arnm_fixed_arena_pool *pool, arnm *source, arnm *allocator
) {
  // nothing to give back is not a failure
  if (!pool) { return ARNM_SUCCESS; }

  const arnm_result released = arnm_fixed_arena_pool_release(pool, source);
  // the descriptor outlives a refusal on purpose: the caller still has arenas to return and
  // needs the pool to return them to
  if (ARNM_ERROR_RESOURCE_IN_USE == released) { return released; }

  const arnm_result freed = arnm_free((uint8_t *)pool, sizeof(arnm_fixed_arena_pool), allocator);
  // a warning from either step is worth more to the caller than the success of the other
  return (ARNM_SUCCESS != released) ? released : freed;
}

uint32_t arnm_fixed_arena_pool_reserved(const arnm_fixed_arena_pool *pool) {
  if (!pool || !pool->arenas) { return 0; }
  return (uint32_t)block_bytes(pool);
}

// ********** lend and take back *******************

arnm_result arnm_fixed_arena_pool_alloc(arnm_fixed_arena_pool *pool, arnm **out) {
  if (!pool || !out) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->arenas) { return ARNM_ERROR_NOT_INITIALIZED; }
  // the pool is the size it is; this says so rather than pretending the request was wrong
  if (!pool->free_head) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }

  arnm *arena = pool->free_head;
  pool->free_head = next_free(arena);
  pool->acquired_count++;

  // the arena is already empty: _init left it so and _free resets on the way in. The link that
  // sat in its first bytes is simply overwritten by whatever the caller allocates.
  *out = arena;
  return ARNM_SUCCESS;
}

arnm_result arnm_fixed_arena_pool_free(arnm_fixed_arena_pool *pool, arnm *arena) {
  if (!pool || !arena) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->arenas) { return ARNM_ERROR_NOT_INITIALIZED; }
  if (!belongs_to(pool, arena)) { return ARNM_ERROR_INVALID_PARAM; }
  // nothing is out, so nothing can be coming back. The cheapest double return to catch, and the
  // only one a pool without a busy list can see at all.
  if (!pool->acquired_count) { return ARNM_ERROR_INVALID_STATE; }

  // emptied before it rejoins the list, so the next caller gets what the first one got
  arnm_reset(arena);
  set_next_free(arena, pool->free_head);
  pool->free_head = arena;
  pool->acquired_count--;
  return ARNM_SUCCESS;
}
