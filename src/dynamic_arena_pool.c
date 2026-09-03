#include "arnm/dynamic_arena_pool.h"

#include "arena_free_list.h"

#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * One allocation holds one arena:
 *
 *   [ arnm descriptor ][ buffer ]
 *
 * The descriptor first, so the block's address and the arena's address are the same pointer --
 * which is what lets an arena be given back without the pool ever having written down where its
 * block began. The buffer follows at offset sizeof(arnm), a multiple of 8 next to a host
 * allocation that is at least as aligned, so arnm_init_arena_borrow() takes it as it is.
 *
 * Where the fixed pool lays every arena out in one block and can therefore check an address
 * against it, here the arenas are scattered across the host and no such check exists. That is
 * the price of growing on demand and it is documented at _free() rather than worked around.
 *
 * The host and nothing else: arnm_alloc/arnm_free with a NULL allocator. A pool that exists to
 * outgrow what was foreseen cannot be rooted in a block that was.
 */

/** Bytes one arena costs the host: its descriptor and its buffer, in one piece. */
static inline uint32_t arena_block_bytes(uint32_t capacity) {
  return (uint32_t)sizeof(arnm) + capacity;
}

/** Ask the host for one arena of @p capacity bytes, ready to be lent. NULL when it says no. */
static arnm *make_arena(uint32_t capacity) {
  uint8_t *block = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&block, arena_block_bytes(capacity), NULL)) { return NULL; }

  arnm *arena = (arnm *)block;
  if (ARNM_SUCCESS != arnm_init_arena_borrow(arena, block + sizeof(arnm), capacity)) {
    // unreachable with a layout this file laid out itself; handled rather than assumed
    arnm_free(block, arena_block_bytes(capacity), NULL);
    return NULL;
  }
  return arena;
}

/* Never arnm_release(): the arena borrowed its buffer from the same block its descriptor sits
   in, so there is one piece to give back and the descriptor is its front. */
static void destroy_arena(arnm *arena, uint32_t capacity) {
  arnm_free((uint8_t *)arena, arena_block_bytes(capacity), NULL);
}

/** Give a chain of free arenas back to the host, head first. */
static void destroy_free_list(arnm *head, uint32_t capacity) {
  while (head) {
    arnm *next = arnm_arena_next_free(head);
    destroy_arena(head, capacity);
    head = next;
  }
}

/** The state a pool is in before init and after release: holding nothing, promising nothing. */
static void forget_everything(arnm_dynamic_arena_pool *pool) {
  pool->free_head = NULL;
  pool->arena_capacity = 0;
  pool->acquired_count = 0;
  pool->spare_count = 0;
  pool->spare_limit = 0;
}

// ********** manage the pool itself *******************

arnm_result arnm_dynamic_arena_pool_init(
    arnm_dynamic_arena_pool *pool, uint32_t arena_capacity, uint32_t spare_limit
) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!arena_capacity) { return ARNM_ERROR_INVALID_PARAM; }

  const uint32_t capacity = arnm_align8_u32(arena_capacity);
  if (!capacity) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  // the descriptor rides in front of every buffer, so the pair has to fit one request
  if (capacity > ARNM_MAX_ALLOC_SIZE - (uint32_t)sizeof(arnm)) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }

  // nothing above touched the host, and nothing below can fail: the pool is written in full
  pool->free_head = NULL;
  pool->arena_capacity = capacity;
  pool->acquired_count = 0;
  pool->spare_count = 0;
  pool->spare_limit = spare_limit;
  return ARNM_SUCCESS;
}

arnm_dynamic_arena_pool *arnm_dynamic_arena_pool_create(
    uint32_t arena_capacity, uint32_t spare_limit, arnm *allocator
) {
  arnm_dynamic_arena_pool *pool = NULL;
  // `allocator` carries this descriptor and nothing else; the arenas are the host's
  if (ARNM_SUCCESS != arnm_alloc((uint8_t **)&pool, sizeof(arnm_dynamic_arena_pool), allocator)) {
    return NULL;
  }
  if (ARNM_SUCCESS != arnm_dynamic_arena_pool_init(pool, arena_capacity, spare_limit)) {
    // straight back to where it came from; it is still the tail there, so an arena takes it
    arnm_free((uint8_t *)pool, sizeof(arnm_dynamic_arena_pool), allocator);
    return NULL;
  }
  return pool;
}

arnm_result arnm_dynamic_arena_pool_reserve(arnm_dynamic_arena_pool *pool, uint32_t count) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->arena_capacity) { return ARNM_ERROR_NOT_INITIALIZED; }
  // the stock never rises above the limit, so a fill past it could not hold
  if (count > pool->spare_limit) { return ARNM_ERROR_INVALID_PARAM; }
  if (pool->spare_count >= count) { return ARNM_SUCCESS; }

  // built aside and spliced on at the end, so a refusal partway leaves the pool as it was
  arnm *head = NULL;
  arnm *tail = NULL;
  uint32_t made = 0;
  while (pool->spare_count + made < count) {
    arnm *arena = make_arena(pool->arena_capacity);
    if (!arena) {
      destroy_free_list(head, pool->arena_capacity);
      return ARNM_ERROR_OUT_OF_MEMORY;
    }
    arnm_arena_set_next_free(arena, head);
    head = arena;
    if (!tail) { tail = arena; }
    made++;
  }

  arnm_arena_set_next_free(tail, pool->free_head);
  pool->free_head = head;
  pool->spare_count += made;
  return ARNM_SUCCESS;
}

arnm_result arnm_dynamic_arena_pool_release(arnm_dynamic_arena_pool *pool) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  // the one refusal that matters: an arena still out is memory someone is writing to
  if (pool->acquired_count) { return ARNM_ERROR_RESOURCE_IN_USE; }

  destroy_free_list(pool->free_head, pool->arena_capacity);
  forget_everything(pool);
  return ARNM_SUCCESS;
}

arnm_result arnm_dynamic_arena_pool_destroy(arnm_dynamic_arena_pool *pool, arnm *allocator) {
  // nothing to give back is not a failure
  if (!pool) { return ARNM_SUCCESS; }

  const arnm_result released = arnm_dynamic_arena_pool_release(pool);
  // the descriptor outlives a refusal on purpose: the caller still has arenas to return and
  // needs the pool to return them to
  if (ARNM_ERROR_RESOURCE_IN_USE == released) { return released; }

  const arnm_result freed = arnm_free((uint8_t *)pool, sizeof(arnm_dynamic_arena_pool), allocator);
  // a warning from either step is worth more to the caller than the success of the other
  return (ARNM_SUCCESS != released) ? released : freed;
}

uint64_t arnm_dynamic_arena_pool_reserved(const arnm_dynamic_arena_pool *pool) {
  if (!pool || !pool->arena_capacity) { return 0; }
  const uint64_t arenas = (uint64_t)pool->spare_count + (uint64_t)pool->acquired_count;
  return arenas * (uint64_t)arena_block_bytes(pool->arena_capacity);
}

// ********** lend and take back *******************

arnm_result arnm_dynamic_arena_pool_alloc(arnm_dynamic_arena_pool *pool, arnm **out) {
  if (!pool || !out) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->arena_capacity) { return ARNM_ERROR_NOT_INITIALIZED; }
  // the counter is the size it is; this says so rather than pretending the request was wrong
  if (UINT32_MAX == pool->acquired_count) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }

  arnm *arena = pool->free_head;
  if (arena) {
    pool->free_head = arnm_arena_next_free(arena);
    pool->spare_count--;
  } else {
    // the shelf is bare, so the arena is made now. The only place this pool asks the host
    // during normal work, and the only place a caller can be told OUT_OF_MEMORY.
    arena = make_arena(pool->arena_capacity);
    if (!arena) { return ARNM_ERROR_OUT_OF_MEMORY; }
  }
  pool->acquired_count++;

  // the arena is already empty: make_arena() left it so and _free resets on the way in. The
  // link that sat in its first bytes is simply overwritten by whatever the caller allocates.
  *out = arena;
  return ARNM_SUCCESS;
}

arnm_result arnm_dynamic_arena_pool_free(arnm_dynamic_arena_pool *pool, arnm *arena) {
  if (!pool || !arena) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->arena_capacity) { return ARNM_ERROR_NOT_INITIALIZED; }
  // nothing is out, so nothing can be coming back. The cheapest double return to catch, and the
  // only one a pool whose arenas are scattered can see at all.
  if (!pool->acquired_count) { return ARNM_ERROR_INVALID_STATE; }

  pool->acquired_count--;

  // emptied before it rejoins the shelf, so the next caller gets what the first one got. An
  // arena on its way to the host is reset too -- the cost is one store, and it keeps the two
  // paths from differing in anything a debugger would show.
  arnm_reset(arena);
  if (pool->spare_count < pool->spare_limit) {
    arnm_arena_set_next_free(arena, pool->free_head);
    pool->free_head = arena;
    pool->spare_count++;
  } else {
    destroy_arena(arena, pool->arena_capacity);
  }
  return ARNM_SUCCESS;
}
