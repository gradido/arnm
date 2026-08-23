#ifndef ARNM_MULTI_ARENA_H
#define ARNM_MULTI_ARENA_H

#include <stdint.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes a regular arena reserves when the caller names no capacity -- 1 KiB. */
#define ARNM_MULTI_ARENA_DEFAULT_CAPACITY ((uint32_t)1 << 10)

/**
 * @brief Full threshold a chain uses when the caller names none -- 64 bytes.
 *
 * An arena leaves the scan once fewer than 64 bytes are left in it, a remainder always being a
 * multiple of 8. That fits a chain of small structs and short strings and gives up little of
 * what it writes off. Name a threshold at init once the request sizes are known.
 */
#define ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING ((uint32_t)64)

/** @brief Arena descriptors per bucket of the descriptor vector. */
#define ARNM_MULTI_ARENA_DEFAULT_BUCKET_LOG2 6

typedef struct arnm_multi_arena_options {
  uint32_t arena_capacity;
  uint32_t arena_max_count;
  uint32_t full_remaining;
  uint8_t bucket_size_log2; // default value = 6 => 2^6 = 64, max value = 16 => 2^16 = 65536
  uint8_t index_grow_step_size;
} arnm_multi_arena_options;

// ********** manage the allocator itself *******************
//
arnm_result arnm_multi_arena_options_validate(arnm_multi_arena_options* options);
arnm *arnm_create_multi_arena(arnm_multi_arena_options *options, arnm *allocator);

// true implies memory != NULL, so callers can skip their own null check
bool arnm_is_multi_arena(const arnm *memory);

/** @brief What the chain currently holds, gathered in one pass. */
typedef struct arnm_multi_arena_stats {
  uint64_t reserved;    /**< Bytes held from the host: the capacities of all arenas. */
  uint64_t used;        /**< Bytes handed out, rounded up to 8 the way the arenas count them. */
  uint32_t arena_count; /**< Arenas in the chain. */
  uint32_t open_count;  /**< Arenas holding more than the chain's full threshold. */
} arnm_multi_arena_stats;


arnm_result arnm_multi_arena_reserve(arnm *m, uint32_t arena_count);

arnm_result arnm_multi_arena_borrow(arnm *m, uint8_t *data, uint32_t capacity);

arnm_result arnm_multi_arena_shrink(arnm *m);

uint32_t arnm_multi_arena_arena_count(const arnm *m);

arnm_result arnm_multi_arena_measure(const arnm *m, arnm_multi_arena_stats *out);

// ********** allocations, with data ptr and size explicit *******************

#ifdef __cplusplus
}
#endif

#endif // ARNM_MULTI_ARENA_H
