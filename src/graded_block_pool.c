#include "arnm/graded_block_pool.h"

#include "arnm/bit.h"
#include "arnm/bytes.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stdint.h>
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
  bool is_oversized;
  bool belong_to_pool;
} graded_block_pool_request;

static arnm_result graded_pool_classify_request(
    graded_block_pool_request *state,
    const arnm_graded_block_pool *pool,
    uint32_t aligned_size
) {
  state->belong_to_pool = true;
  uint8_t grade_exp = arnm_log2_power_of_two(aligned_size);
  // if requested memory size exceed biggest grade
  state->is_oversized = grade_exp > pool->max_log2;
  if (state->is_oversized && pool->oversized_bytes < aligned_size) {
    state->belong_to_pool = false;
  }
  state->grade_index = grade_exp < pool->min_log2 ? 0 : grade_exp - pool->min_log2;
  state->grade_bytes = arnm_pow2_u32(grade_exp < pool->min_log2 ? pool->min_log2 : grade_exp);
  if (pool->lent_bytes < state->grade_bytes) {
    state->belong_to_pool = false;
  }
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

arnm_result arnm_init_graded_block_pool(arnm_graded_block_pool* pool, arnm_graded_block_pool_options *options, arnm *source)
{
  arnm_result result = arnm_graded_block_pool_options_validate(options);
  if (ARNM_SUCCESS != result) { return result; }

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
  if (!pool->source) {
    return ARNM_ERROR_OUT_OF_MEMORY;
  }
  pool->min_log2 = options->min_block_log2;
  pool->max_log2 = options->max_block_log2;

  return ARNM_SUCCESS;
}

arnm_graded_block_pool *arnm_create_graded_block_pool(arnm_graded_block_pool_options *options, arnm *source) {
  if (ARNM_SUCCESS != arnm_graded_block_pool_options_validate(options)) { return NULL; }
  uint8_t* block = NULL;
  const size_t allocation_capacity = sizeof(arnm_graded_block_pool);
  if (ARNM_SUCCESS != arnm_alloc(&block, allocation_capacity, source)) { return NULL; }
  arnm_graded_block_pool *pool = (arnm_graded_block_pool *)(void *)block;
  memset(pool, 0, allocation_capacity);

  if (ARNM_SUCCESS != arnm_init_graded_block_pool(pool, options, source)) {
    arnm_free(block, allocation_capacity, source);
    return NULL;
  }
  return pool;
}

// ********** manage memory allocations with data ptr and size explicit *******************

static arnm_result graded_block_pool_alloc_classified(
    arnm_graded_block_pool *pool,
    uint8_t **buffer,
    graded_block_pool_request *request,
    uint32_t aligned_size
) {
  // if requested memory size exceed biggest grade
  if (request->is_oversized) {
    // alloc extra buffer in
    const arnm_result result = arnm_alloc(buffer, aligned_size, pool->source);
    if (ARNM_SUCCESS == result) { pool->oversized_bytes += aligned_size; }
    return result;
  }

  uint8_t *block = pool->free_head[request->grade_index];
  if (block) {
    pool->free_head[request->grade_index] = arnm_load_ptr(block);
    pool->cached_bytes -= request->grade_bytes;
  } else {
    const arnm_result result = arnm_alloc(&block, request->grade_bytes, pool->source);
    if (ARNM_SUCCESS != result) { return result; }
  }
  pool->lent_bytes += request->grade_bytes;
  *buffer = block;
  return ARNM_SUCCESS;
}


arnm_result arnm_graded_block_pool_alloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t aligned_size
) {
  graded_block_pool_request request;
  arnm_result result = graded_pool_classify_request(&request, pool, aligned_size);
  if (result != ARNM_SUCCESS) { return result; }
  return graded_block_pool_alloc_classified(pool, buffer, &request, aligned_size);
}


static arnm_result graded_block_pool_free_classified(
    arnm_graded_block_pool *pool,
    uint8_t *buffer,
    graded_block_pool_request *request,
    uint32_t aligned_size
);

arnm_result arnm_graded_block_pool_realloc(
    arnm_graded_block_pool *pool,
    uint8_t **buffer,
    uint32_t old_size,
    uint32_t new_size
) {
  uint32_t new_aligned = arnm_align8_u32(new_size);
  uint32_t old_aligned = arnm_align8_u32(old_size);
  if ((new_size && !new_aligned) || (old_size && !old_aligned))
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;

  // a block with no size has no grade to leave, the same refusal a free of it gets
  if (!old_aligned) { return ARNM_ERROR_INVALID_PARAM; }

  graded_block_pool_request new_size_request;
  graded_block_pool_request old_size_request;
  arnm_result result = ARNM_SUCCESS;
  result = graded_pool_classify_request(&new_size_request, pool, new_aligned);
  if (result != ARNM_SUCCESS) { return result; }
  result = graded_pool_classify_request(&old_size_request, pool, old_aligned);
  if (result != ARNM_SUCCESS) { return result; }

  // the block already has room for anything its grade serves
  if (!old_size_request.is_oversized && !new_size_request.is_oversized &&
      old_size_request.grade_index == new_size_request.grade_index) {
    return ARNM_SUCCESS;
  }

  // both sides past the largest grade: the source resizes, in place where it can
  if (old_size_request.is_oversized && new_size_request.is_oversized) {
    const arnm_result result = arnm_realloc(buffer, old_aligned, new_aligned, pool->source);
    if (ARNM_SUCCESS == result || ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == result) {
      pool->oversized_bytes -= old_aligned;
      pool->oversized_bytes += new_aligned;
    }
    return result;
  }

  // another grade, or across the largest one: a new block, the contents, the old block back.
  // The old block is checked above, so its free below cannot be refused by the counters.
  uint8_t *moved = NULL;
  result = graded_block_pool_alloc_classified(pool, &moved, &new_size_request, new_aligned);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(moved, *buffer, old_size < new_size ? old_size : new_size);
  result = graded_block_pool_free_classified(pool, *buffer, &old_size_request, old_aligned);
  *buffer = moved;
  // an oversized old block behind an arena may stay there: the resize happened regardless
  return result;
}

static arnm_result graded_block_pool_free_classified(
    arnm_graded_block_pool *pool,
    uint8_t *buffer,
    graded_block_pool_request *request,
    uint32_t aligned_size
) {
  if (!request->belong_to_pool) { return ARNM_ERROR_INVALID_STATE; }
  // if requested memory size exceed biggest grade
  if (request->is_oversized) {
    const arnm_result result = arnm_free(buffer, aligned_size, pool->source);
    // a warning means the free happened and the source keeps the bytes; either way they are no
    // longer out. Only a refusal leaves the block where it was.
    if (ARNM_SUCCESS == result || ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == result) {
      pool->oversized_bytes -= aligned_size;
    }
    return result;
  }

  memcpy(buffer, &pool->free_head[request->grade_index], sizeof(uint8_t *));
  pool->free_head[request->grade_index] = buffer;
  pool->lent_bytes -= request->grade_bytes;
  pool->cached_bytes += request->grade_bytes;
  return ARNM_SUCCESS;
}

arnm_result arnm_graded_block_pool_free(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint32_t size
) {
  // nothing handed back is nothing to do, as free(NULL) is
  if (!buffer) { return ARNM_SUCCESS; }
  // the grade is the size's to name; without one the block has no list to go on
  if (!size) { return ARNM_ERROR_INVALID_PARAM; }
  const uint32_t pool_size = arnm_align8_u32(size);
  if (size && !pool_size) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  graded_block_pool_request request;
  arnm_result result = graded_pool_classify_request(&request, pool, pool_size);
  if (result != ARNM_SUCCESS) { return result; }
  return graded_block_pool_free_classified(pool, buffer, &request, pool_size);
}

void arnm_graded_block_pool_reset(arnm_graded_block_pool *pool) {
  arnm_reset(pool->source);
  memset(pool->free_head, 0, sizeof(pool->free_head));
  pool->lent_bytes = 0;
  pool->cached_bytes = 0;
  pool->oversized_bytes = 0;
}

void arnm_graded_block_pool_release(arnm_graded_block_pool *pool) {
  arnm_graded_block_pool_reset(pool);
  arnm_release(pool->source);
}

arnm_result arnm_graded_block_pool_destroy(arnm_graded_block_pool *pool, arnm *allocator) {
  arnm_result result = arnm_destroy(pool->source, allocator);
  if (ARNM_SUCCESS != result) { return result; }
  return arnm_free((uint8_t *)pool, sizeof(arnm_graded_block_pool), allocator);
}
