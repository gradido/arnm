#include "arnm/graded_block_pool.h"

#include "arnm/bit.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"
#include "memory_intern.h"

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

arnm *arnm_create_graded_block_pool(arnm_graded_block_pool_options *options, arnm *source) {
  if (ARNM_SUCCESS != arnm_graded_block_pool_options_validate(options)) { return NULL; }
  const uint32_t allocation_size = sizeof(arnm) + sizeof(arnm_graded_block_pool_state);
  uint8_t *block = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&block, allocation_size, source)) { return NULL; }
  // zeroed: every free list empty, every counter 0, and no stray bytes in the handle's union
  memset(block, 0, allocation_size);

  arnm_intern *memory = (arnm_intern *)(void *)block;
  arnm_graded_block_pool_state *pool =
      (arnm_graded_block_pool_state *)(void *)(block + sizeof(arnm));
  arnm_multi_arena_options multi_arena_options = {0};
  uint32_t max_grade_size = pow2_u32(options->max_block_log2);
  multi_arena_options.arena_capacity = max_grade_size * options->alloc_arena_capacity;
  // detect overflow/wrap
  if (multi_arena_options.arena_capacity < max_grade_size || multi_arena_options.arena_capacity > ARNM_MAX_ALLOC_SIZE) {
    multi_arena_options.arena_capacity = ARNM_MAX_ALLOC_SIZE;
  }
  multi_arena_options.full_remaining = pow2_u32(options->min_block_log2) - 1u;
  pool->source = arnm_create_multi_arena(&multi_arena_options, source);
  pool->min_log2 = options->min_block_log2;
  pool->max_log2 = options->max_block_log2;
  memory->graded_block_pool = pool;
  memory->allocation_type = ARNM_ALLOC_TYPE_GRADED_BLOCK_POOL;
  return (arnm *)memory;
}

bool arnm_is_graded_block_pool(const arnm *memory) {
  return is_graded_block_pool((const arnm_intern *)memory);
}

arnm_result arnm_graded_block_pool_measure(const arnm *memory, arnm_graded_block_pool_stats *out) {
  if (!memory || !out) { return ARNM_ERROR_NULL_POINTER; }
  const arnm_intern *intern = (const arnm_intern *)memory;
  if (!is_graded_block_pool(intern)) { return ARNM_ERROR_INVALID_STATE; }
  const arnm_graded_block_pool_state *pool = intern->graded_block_pool;
  out->lent_bytes = pool->lent_bytes;
  out->cached_bytes = pool->cached_bytes;
  out->oversized_bytes = pool->oversized_bytes;
  out->min_block_log2 = pool->min_log2;
  out->max_block_log2 = pool->max_log2;
  return ARNM_SUCCESS;
}
