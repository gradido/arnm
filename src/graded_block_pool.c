#include "arnm/graded_block_pool.h"

#include "arnm/arena.h"
#include "arnm/bit.h"
#include "arnm/bytes.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include <stdint.h>
#include <string.h>

/*
 * A pool is a plain struct and a chain of arenas it owns:
 *
 *   arnm_graded_block_pool { source -> chain, counters, free_head[grade], current arena }
 *
 * Every block lives in the chain. The pool takes one arena from it at a time, whole, and keeps
 * it as a borrowed arena in `current`: blocks are cut off at its cursor, the arena's own index.
 * When a request no longer fits, what is left is cut into blocks of the grades that fit and put
 * on their lists before the next arena is taken -- so the chain's scan runs once per arena, and
 * no arena keeps a tail nobody can use.
 *
 * The struct holds the head of one free list per grade, and a free block holds the link to the
 * next in its first 8 bytes, so nothing is kept per block and the struct does not grow with what
 * it hands out. A block never goes back to the chain: the chain could only take back its tail,
 * and the free list serves the next request better.
 *
 * The counters are uint64_t because they sum blocks: each block fits a uint32_t, a pool full of
 * them need not.
 */

// ********** grades *******************

/*
 * The one place a size becomes a grade, shared by alloc and free so that both always agree on
 * the list: the exponent, raised to the smallest grade. No NULL checks, every caller has made
 * them already.
 */
static arnm_result size_to_log2(const arnm_graded_block_pool *pool, uint32_t size, uint8_t *log2) {
  if (!size) { return ARNM_ERROR_INVALID_PARAM; }
  const uint32_t aligned_bytes = arnm_align8_u32(size);
  if (!aligned_bytes) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  const uint8_t exponent = arnm_log2_power_of_two(aligned_bytes);
  // 2^31 and above land on 32, past every grade a pool can have
  if (exponent > pool->max_log2) { return ARNM_ERROR_RESOURCE_SIZE_EXCEED; }
  *log2 = exponent < pool->min_log2 ? pool->min_log2 : exponent;
  return ARNM_SUCCESS;
}

/** Puts @p block on the list of its grade as a cached block. */
static void push_free(arnm_graded_block_pool *pool, uint8_t *block, uint8_t log2) {
  uint8_t **head = &pool->free_head[log2 - pool->min_log2];
  memcpy(block, head, sizeof(uint8_t *));
  *head = block;
  pool->cached_bytes += (uint64_t)1u << log2;
}

// ********** manage the pool itself *******************

arnm_result arnm_graded_block_pool_options_validate(arnm_graded_block_pool_options *options) {
  if (!options) { return ARNM_ERROR_NULL_POINTER; }
  if (!options->min_block_log2) {
    options->min_block_log2 = ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2;
  }
  if (!options->max_block_log2) {
    options->max_block_log2 = ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2;
  }
  if (options->min_block_log2 < ARNM_GRADED_BLOCK_POOL_MIN_LOG2 ||
      options->max_block_log2 > ARNM_GRADED_BLOCK_POOL_MAX_LOG2 ||
      options->min_block_log2 > options->max_block_log2) {
    return ARNM_ERROR_INVALID_PARAM;
  }
  if (!options->alloc_arena_capacity) {
    options->alloc_arena_capacity = ARNM_GRADED_BLOCK_POOL_DEFAULT_ALLOC_ARENA_CAPACITY;
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_init(
    arnm_graded_block_pool *pool, arnm_graded_block_pool_options *options, arnm *source
) {
  if (!pool) return ARNM_ERROR_NULL_POINTER;

  arnm_result result = arnm_graded_block_pool_options_validate(options);
  if (ARNM_SUCCESS != result) { return result; }

  arnm_multi_arena_options multi_arena_options = {0};
  uint32_t max_grade_size = arnm_pow2_u32(options->max_block_log2);
  uint64_t full_capacity = (uint64_t)max_grade_size * (uint64_t)options->alloc_arena_capacity;
  // capped rather than refused: a chain opens an arena of exactly the request's size when one
  // does not fit, so even the largest grade is still served
  if (full_capacity > ARNM_MAX_ALLOC_SIZE) {
    multi_arena_options.arena_capacity = ARNM_MAX_ALLOC_SIZE;
  } else {
    multi_arena_options.arena_capacity = (uint32_t)full_capacity;
  }
  // an arena with less than the smallest grade left can serve nothing more
  multi_arena_options.full_remaining = arnm_pow2_u32(options->min_block_log2) - 1u;
  arnm *multi_arena = arnm_create_multi_arena(&multi_arena_options, source);
  if (!multi_arena) { return ARNM_ERROR_OUT_OF_MEMORY; }
  // written only now, so a failure above leaves the caller's struct as it was
  memset(pool, 0, sizeof(arnm_graded_block_pool));
  pool->source = multi_arena;
  pool->arena_capacity = multi_arena_options.arena_capacity;
  pool->min_log2 = options->min_block_log2;
  pool->max_log2 = options->max_block_log2;

  return ARNM_SUCCESS;
}

arnm_graded_block_pool *arnm_graded_block_pool_create(
    arnm_graded_block_pool_options *options, arnm *source
) {
  if (ARNM_SUCCESS != arnm_graded_block_pool_options_validate(options)) { return NULL; }
  uint8_t *block = NULL;
  const uint32_t allocation_capacity = (uint32_t)sizeof(arnm_graded_block_pool);
  if (ARNM_SUCCESS != arnm_alloc(&block, allocation_capacity, source)) { return NULL; }
  arnm_graded_block_pool *pool = (arnm_graded_block_pool *)(void *)block;
  if (ARNM_SUCCESS != arnm_graded_block_pool_init(pool, options, source)) {
    arnm_free(block, allocation_capacity, source);
    return NULL;
  }
  return pool;
}

// ********** blocks *******************

/**
 * Cuts what is left of the current arena into blocks of the grades that fit, largest first, and
 * puts them on their lists. Every block ever cut is a grade, so what is left is a multiple of the
 * smallest one, and nothing stays behind; only an arena capped at ARNM_MAX_ALLOC_SIZE can end
 * in a few bytes that no grade takes.
 */
static void split_current(arnm_graded_block_pool *pool) {
  uint32_t left = arnm_arena_remaining(&pool->current);
  for (uint8_t log2 = pool->max_log2; left >= ((uint32_t)1u << pool->min_log2); --log2) {
    const uint32_t bytes = (uint32_t)1u << log2;
    while (left >= bytes) {
      uint8_t *piece = NULL;
      // cannot fail: the arena was just asked how much it holds
      (void)arnm_alloc(&piece, bytes, &pool->current);
      push_free(pool, piece, log2);
      left -= bytes;
    }
  }
}

/** Takes a fresh arena from the chain, the old one's rest on the lists first. */
static arnm_result next_arena(arnm_graded_block_pool *pool) {
  uint8_t *block = NULL;
  // asked for first, so a refusal leaves the current arena and the lists as they were
  const arnm_result result = arnm_alloc(&block, pool->arena_capacity, pool->source);
  if (ARNM_SUCCESS != result) { return result; }
  split_current(pool);
  // cannot fail: the chain hands out 8 byte aligned blocks, the capacity is a multiple of 8
  (void)arnm_init_arena_borrow(&pool->current, block, pool->arena_capacity);
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_alloc_fresh(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint8_t log2
) {
  if (!pool->source) { return ARNM_ERROR_NOT_INITIALIZED; }
  const uint32_t bytes = (uint32_t)1u << log2;
  if (arnm_arena_remaining(&pool->current) < bytes) {
    const arnm_result result = next_arena(pool);
    if (ARNM_SUCCESS != result) { return result; }
    // the rest of the old arena may have held a block of exactly this grade
    if (pool->free_head[log2 - pool->min_log2]) {
      return arnm_graded_block_pool_alloc_log2(pool, buffer, log2);
    }
  }
  uint8_t *block = NULL;
  // cannot fail: the arena holds at least one largest grade block, and had room for this one
  (void)arnm_alloc(&block, bytes, &pool->current);
  pool->lent_bytes += bytes;
  *buffer = block;
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_alloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t size
) {
  if (!pool || !buffer) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->source) { return ARNM_ERROR_NOT_INITIALIZED; }
  uint8_t log2 = 0;
  const arnm_result result = size_to_log2(pool, size, &log2);
  if (ARNM_SUCCESS != result) { return result; }
  return arnm_graded_block_pool_alloc_log2(pool, buffer, log2);
}

arnm_result arnm_graded_block_pool_realloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t old_size, uint32_t new_size
) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  // always a new block, even within one grade: see the header for why the hot path wins
  uint8_t *moved = NULL;
  arnm_result result = arnm_graded_block_pool_alloc(pool, &moved, new_size);
  if (ARNM_SUCCESS != result) { return result; }

  if (*buffer && old_size) {
    memcpy(moved, *buffer, old_size < new_size ? old_size : new_size);
    result = arnm_graded_block_pool_free(pool, *buffer, old_size);
    // the move happened; the old block is simply not ours to take back
    if (ARNM_SUCCESS != result) {
      *buffer = moved;
      return ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
    }
  }
  *buffer = moved;
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_free(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint32_t size
) {
  // nothing handed back is nothing to do, as free(NULL) is
  if (!buffer) { return ARNM_SUCCESS; }
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!pool->source) { return ARNM_ERROR_NOT_INITIALIZED; }
  uint8_t log2 = 0;
  const arnm_result result = size_to_log2(pool, size, &log2);
  if (ARNM_SUCCESS != result) { return result; }
  return arnm_graded_block_pool_free_log2(pool, buffer, log2);
}

void arnm_graded_block_pool_reset(arnm_graded_block_pool *pool) {
  if (!pool) { return; }
  // the chain keeps its arenas, so the free lists would point into ground that is now open
  arnm_reset(pool->source);
  memset(pool->free_head, 0, sizeof(pool->free_head));
  // the current arena was the chain's and is empty again there; the next block takes a fresh one
  memset(&pool->current, 0, sizeof(pool->current));
  pool->lent_bytes = 0;
  pool->cached_bytes = 0;
}

void arnm_graded_block_pool_release(arnm_graded_block_pool *pool, arnm *source) {
  if (!pool) { return; }
  arnm_graded_block_pool_reset(pool);
  arnm_destroy(pool->source, source);
  pool->source = NULL;
}

arnm_result arnm_graded_block_pool_destroy(arnm_graded_block_pool *pool, arnm *source) {
  if (!pool) { return ARNM_SUCCESS; }
  arnm_result result = arnm_destroy(pool->source, source);
  if (ARNM_SUCCESS != result) { return result; }
  return arnm_free((uint8_t *)pool, sizeof(arnm_graded_block_pool), source);
}
