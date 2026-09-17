#ifndef ARNM_GRADED_BLOCK_POOL_H
#define ARNM_GRADED_BLOCK_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_graded_block_pool arnm_graded_block_pool
 * @brief Blocks in power of two grades, drawn from an allocator you name and reused once given
 *        back.
 *
 * An arena takes back only its newest allocation. A container that grows -- a vector's index
 * array, a hash table, a set whose array turns into a bitmap -- leaves every block it outgrew
 * behind it, and behind an arena that block stays until the reset. This handle sits between
 * such a container and the allocator underneath: a freed block goes onto a free list for its
 * size, and the next request of that size gets it back, wherever in the arena it lies.
 *
 * Once created the pool is an ordinary @ref arnm. @ref arnm_alloc(), @ref arnm_free(),
 * @ref arnm_realloc() and every container that takes an `arnm *` use it unchanged. This header
 * covers bringing one into being and asking it what it holds.
 *
 * ### Grades
 *
 * Every size between @ref arnm_graded_block_pool_options::min_block_log2 and
 * @ref arnm_graded_block_pool_options::max_block_log2 is a grade, one per power of two. A
 * request is rounded up to 8 and then to the next grade: 20 bytes take a 32 byte block, 100
 * bytes a 128 byte one. The slack is at most the request again, and it is what buys the reuse --
 * a block fits every later request of its grade, whatever size that request has.
 *
 * @ref arnm_free() and @ref arnm_realloc() work the grade out again from the size they are
 * told, the way every allocator in arnm does. Nothing is stored next to a block, and a free
 * block keeps the link to the next one in its own first 8 bytes, which is why no grade is
 * smaller than 8 bytes.
 *
 * ### Past the largest grade
 *
 * A request larger than the largest grade is handed to the source as it is, and so is its free.
 * It is not reused and not cached: a block that size is rare, and a grade kept for it would
 * hold its memory for good. The source's answer comes back unchanged -- behind an arena a free
 * of such a block can therefore be @ref ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED.
 *
 * ### The source is borrowed
 *
 * The pool draws every block from the allocator given to @ref arnm_create_graded_block_pool()
 * and never owns it. Blocks go back to it only on @ref arnm_release() -- or a reset where a reset
 * could not do better, see below -- and only those waiting on a free list; the ones still out are
 * the caller's.
 *
 * - @ref arnm_reset() over an arena or a chain forgets the free lists and the counters and does
 *   not touch the source; the forgotten blocks stay there until the source's own reset. Over
 *   anything else -- the host, another pool -- forgetting a block would lose it, so there a reset
 *   does what @ref arnm_release() does.
 * - Resetting or releasing the *source* ends the pool too: its own bytes come from there, so the
 *   handle is gone with them. Make a new pool afterwards; calling the old one reads freed memory.
 * - @ref arnm_release() gives every cached block back to the source through @ref arnm_free():
 *   behind the host that is `free()`, behind an arena only what lies at its tail comes back.
 * - @ref arnm_destroy() releases, then gives the pool's own bytes back to the allocator named.
 *
 * With the host as the source (NULL, or a zeroed handle) every block is its own `malloc()`, the
 * free lists keep them, and both @ref arnm_release() and @ref arnm_reset() really free what they
 * hold.
 *
 * @note A freed pointer is trusted to be one this pool handed out with the size it is freed
 *       with. A foreign pointer or a wrong size goes onto a free list and is handed to the next
 *       caller of that grade; the pool cannot tell. A free that would take the counters below
 *       zero is refused, which catches the plainest double free and no more.
 * @note Blocks are never split or merged. A free list only grows until the peak of its grade
 *       and gives nothing to another grade.
 * @note Nothing here is thread safe. One pool belongs to one thread at a time.
 *
 * @whisper What the stream let go of is caught in the right basin and poured out again
 *
 * @{
 */

/** @brief Smallest grade a pool can have: 2^3, the 8 bytes a free block keeps its link in. */
#define ARNM_GRADED_BLOCK_POOL_MIN_LOG2 3u

/** @brief Largest grade a pool can have: 2^31, the last power of two a uint32_t size holds. */
#define ARNM_GRADED_BLOCK_POOL_MAX_LOG2 31u

/** @brief Smallest grade when the caller names none: 2^4, 16 bytes. */
#define ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2 4u

/** @brief Largest grade when the caller names none: 2^20, 1 MiB. */
#define ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2 20u

#define ARNM_GRADED_BLOCK_POOL_DEFAULT_ALLOC_ARENA_CAPACITY 4u

/**
 * @brief Which grades a pool has. Every field takes its default from 0, so `{0}` is a valid set.
 *
 * Pass the same struct to @ref arnm_graded_block_pool_options_validate() to find out *why* a set
 * was refused; @ref arnm_create_graded_block_pool() only answers whether it was.
 */
typedef struct arnm_graded_block_pool_options {
  /** Smallest grade as a power of two, 3 to 31. 0 means
   *  @ref ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2. A request below it takes a block this size. */
  uint8_t min_block_log2;
  /** Largest grade as a power of two, from @c min_block_log2 to 31. 0 means
   *  @ref ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2. A request above it goes to the source. */
  uint8_t max_block_log2;
  uint8_t alloc_arena_capacity; // as multiple of largest grade
} arnm_graded_block_pool_options;

/**
 * @brief What a graded block pool holds: a free list per grade, the source, three counters.
 *
 * Lives directly behind the pool's handle, in the one allocation arnm_create_graded_block_pool()
 * takes from the source. @c free_head is indexed by the grade's exponent itself, so the entries
 * below @c min_log2 are never used -- 24 bytes spent to keep every lookup a plain index.
 */
typedef struct arnm_graded_block_pool {
  arnm *source; /**< Where blocks come from and return to; NULL for the host. Borrowed. */
  uint8_t *free_head[ARNM_GRADED_BLOCK_POOL_MAX_LOG2 + 1u]; /**< First free block per grade. */
  uint64_t lent_bytes;      /**< Graded block bytes out with callers. */
  uint64_t cached_bytes;    /**< Graded block bytes on the free lists. */
  uint64_t oversized_bytes; /**< Bytes handed to the source past the largest grade. */
  uint8_t min_log2;         /**< Smallest grade. */
  uint8_t max_log2;         /**< Largest grade. */
} arnm_graded_block_pool;

// ********** manage the allocator itself *******************

/**
 * @brief Check a set of options and fill its 0 fields in with the defaults.
 *
 * @param[in,out] options Options to check; not NULL. Updated in place with the effective values.
 * @retval ARNM_SUCCESS             Usable, and @p options now holds the effective values.
 * @retval ARNM_ERROR_NULL_POINTER  @p options is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM A grade is outside 3 to 31, or the smallest is above the
 *                                  largest.
 */
arnm_result arnm_graded_block_pool_options_validate(arnm_graded_block_pool_options *options);

/**
 * @brief Build a pool over @p source. It holds no block until the first allocation.
 *
 * @param[in,out] options Which grades; not NULL. `{0}` gives the defaults. Filled in with the
 *                        effective values on return.
 * @param[in,out] source  Where every block and the pool's own bytes come from, or NULL for the
 *                        host. Any handle will do -- an arena, a chain, the host, another pool.
 *                        Borrowed, not owned, and it has to outlive the pool.
 * @return The pool, or NULL if @p options was refused or @p source had no room. Use
 *         @ref arnm_graded_block_pool_options_validate() to tell those apart.
 * @note Give it back with @ref arnm_destroy(), naming the same @p source.
 * @whisper Basins dug in a row, the smallest first, the stream not yet let in
 */
arnm_graded_block_pool *arnm_create_graded_block_pool(arnm_graded_block_pool_options *options, arnm *source);

arnm_result arnm_init_graded_block_pool(arnm_graded_block_pool* pool, arnm_graded_block_pool_options *options, arnm *source);

arnm_result arnm_graded_block_pool_alloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t aligned_size
);

arnm_result arnm_graded_block_pool_realloc(
    arnm_graded_block_pool *pool,
    uint8_t **buffer,
    uint32_t old_size,
    uint32_t new_size
);

arnm_result arnm_graded_block_pool_free(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint32_t size
);

void arnm_graded_block_pool_reset(arnm_graded_block_pool *pool);

void arnm_graded_block_pool_release(arnm_graded_block_pool *pool);

arnm_result arnm_graded_block_pool_destroy(arnm_graded_block_pool *pool, arnm *allocator)

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_GRADED_BLOCK_POOL_H
