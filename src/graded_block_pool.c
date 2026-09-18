#include "arnm/graded_block_pool.h"

#include "arnm/bit.h"
#include "arnm/bytes.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A pool is one allocation from its source:
 *
 *   [ arnm handle, 32 bytes ][ arnm_graded_block_pool_state ]
 *
 * The handle's union points at the state behind it, the way a chain's points at its
 * arnm_multi_arena. Every block lives in the source; the state only holds the head of one free
 * list per grade, and a free block holds the link to the next in its first 8 bytes. Nothing is
 * kept per block, so the pool's own size does not grow with what it hands out.
 *
 * The counters are uint64_t because they sum blocks: each block fits a uint32_t, a pool full of
 * them need not.
 */

// ************* graded pool functions ***************************************************

typedef struct graded_block_pool_request {
  uint32_t grade_bytes;
  uint8_t grade_index;
} graded_block_pool_request;

static arnm_result graded_pool_classify_request(
    graded_block_pool_request *state, const arnm_graded_block_pool *pool, uint32_t size
) {
  if (!state || !pool) { return ARNM_ERROR_NULL_POINTER; }
  if (!size)  { return ARNM_ERROR_INVALID_PARAM; }

  uint32_t aligned_bytes = arnm_align8_u32(size);
  if ((size && !aligned_bytes)) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  uint8_t grade_exp = arnm_log2_power_of_two(aligned_bytes);
  // if requested memory size exceed biggest grade
  if (grade_exp > pool->max_log2) {
    return ARNM_ERROR_RESOURCE_SIZE_EXCEED;
  }
  state->grade_index = grade_exp < pool->min_log2 ? 0 : grade_exp - pool->min_log2;
  state->grade_bytes = arnm_pow2_u32(grade_exp < pool->min_log2 ? pool->min_log2 : grade_exp);
  return ARNM_SUCCESS;
}

// ********** manage the allocator itself *******************

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

arnm_result arnm_init_graded_block_pool(
    arnm_graded_block_pool *pool, arnm_graded_block_pool_options *options, arnm *source
) {
  arnm_result result = arnm_graded_block_pool_options_validate(options);
  if (ARNM_SUCCESS != result) { return result; }
  memset(pool, 0, sizeof(arnm_graded_block_pool));
  arnm_multi_arena_options multi_arena_options = {0};
  uint32_t max_grade_size = arnm_pow2_u32(options->max_block_log2);
  uint64_t full_capacity = (uint64_t)max_grade_size * (uint64_t)options->alloc_arena_capacity;
  multi_arena_options.arena_capacity = max_grade_size * options->alloc_arena_capacity;
  // detect overflow
  if (full_capacity > ARNM_MAX_ALLOC_SIZE) {
    multi_arena_options.arena_capacity = ARNM_MAX_ALLOC_SIZE;
  } else {
    multi_arena_options.arena_capacity = (uint32_t)full_capacity;
  }
  multi_arena_options.full_remaining = arnm_pow2_u32(options->min_block_log2) - 1u;
  pool->source = arnm_create_multi_arena(&multi_arena_options, source);
  if (!pool->source) { return ARNM_ERROR_OUT_OF_MEMORY; }
  pool->min_log2 = options->min_block_log2;
  pool->max_log2 = options->max_block_log2;

  return ARNM_SUCCESS;
}

arnm_graded_block_pool *arnm_create_graded_block_pool(
    arnm_graded_block_pool_options *options, arnm *source
) {
  if (ARNM_SUCCESS != arnm_graded_block_pool_options_validate(options)) { return NULL; }
  uint8_t *block = NULL;
  const size_t allocation_capacity = sizeof(arnm_graded_block_pool);
  if (ARNM_SUCCESS != arnm_alloc(&block, allocation_capacity, source)) { return NULL; }
  arnm_graded_block_pool *pool = (arnm_graded_block_pool *)(void *)block;
  if (ARNM_SUCCESS != arnm_init_graded_block_pool(pool, options, source)) {
    arnm_free(block, allocation_capacity, source);
    return NULL;
  }
  return pool;
}

// ********** manage memory allocations with data ptr and size explicit *******************

arnm_result arnm_graded_block_pool_alloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t size
) {
  graded_block_pool_request request;
  arnm_result result = graded_pool_classify_request(&request, pool, size);
  if (result != ARNM_SUCCESS) { return result; }

  uint8_t *block = pool->free_head[request.grade_index];
  if (block) {
    pool->free_head[request.grade_index] = arnm_load_ptr(block);
    pool->cached_bytes -= request.grade_bytes;
  } else {
    const arnm_result result = arnm_alloc(&block, request.grade_bytes, pool->source);
    if (ARNM_SUCCESS != result) { return result; }
  }
  pool->lent_bytes += request.grade_bytes;
  *buffer = block;
  return ARNM_SUCCESS;
}


arnm_result arnm_graded_block_pool_realloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t old_size, uint32_t new_size
) {
  if (!pool) { return ARNM_ERROR_NULL_POINTER; }

  // another grade, or across the largest one: a new block, the contents, the old block back.
  // The old block is checked above, so its free below cannot be refused by the counters.
  uint8_t *moved = NULL;
  arnm_result result = arnm_graded_block_pool_alloc(pool, &moved, new_size);
  *buffer = moved;
  if (ARNM_SUCCESS != result) { return result; }
  if (*buffer && old_size) {
    memcpy(moved, *buffer, old_size < new_size ? old_size : new_size);
    result = arnm_graded_block_pool_free(pool, *buffer, old_size);
    if (ARNM_SUCCESS != result) {
      return ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
    }
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_free(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint32_t size
) {
  // nothing handed back is nothing to do, as free(NULL) is
  if (!buffer) { return ARNM_SUCCESS; }
  graded_block_pool_request request;
  arnm_result result = graded_pool_classify_request(&request, pool, size);
  if (result != ARNM_SUCCESS) { return result; }
  if (pool->lent_bytes < request.grade_bytes) { return ARNM_ERROR_INVALID_STATE; }

  memcpy(buffer, &pool->free_head[request.grade_index], sizeof(uint8_t *));
  pool->free_head[request.grade_index] = buffer;
  pool->lent_bytes -= request.grade_bytes;
  pool->cached_bytes += request.grade_bytes;
  return ARNM_SUCCESS;
}

void arnm_graded_block_pool_reset(arnm_graded_block_pool *pool) {
  arnm_reset(pool->source);
  memset(pool->free_head, 0, sizeof(pool->free_head));
  pool->lent_bytes = 0;
  pool->cached_bytes = 0;
  pool->oversized_bytes = 0;
}

void arnm_graded_block_pool_release(arnm_graded_block_pool *pool, arnm *source) {
  arnm_graded_block_pool_reset(pool);
  arnm_destroy(pool->source, source);
  pool->source = NULL;
}

arnm_result arnm_graded_block_pool_destroy(arnm_graded_block_pool *pool, arnm *allocator) {
  arnm_result result = arnm_destroy(pool->source, allocator);
  if (ARNM_SUCCESS != result) { return result; }
  return arnm_free((uint8_t *)pool, sizeof(arnm_graded_block_pool), allocator);
}
