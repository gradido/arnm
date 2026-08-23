#ifndef ARNM_ARENA_MEMORY_H
#define ARNM_ARENA_MEMORY_H

#include <stdint.h>

#include "memory.h"
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_arena arnm_arena
 * @brief One block, one index, allocation by moving the index forward.
 *
 * The simplest allocator here and the one to reach for when the peak is known: a single block,
 * taken once, handed out eight bytes at a time by moving an index. An allocation is an add and
 * a bounds check. Nothing is tracked per block, so nothing is paid per block either.
 *
 * The trade is on the way back. An arena releases only from its tail (see @ref arnm_free()),
 * and the way to reclaim everything is @ref arnm_reset() -- one instruction, all of it at once.
 * That fits work that arrives in rounds: a frame, a request, a parse. It does not fit objects
 * with independent lifetimes.
 *
 * Once initialized, an arena is used through @ref arnm_memory like any other handle. This
 * header is only about bringing one into being.
 *
 * ### Owned or borrowed
 *
 * @ref arnm_init_arena() takes its block from the host and frees it on @ref arnm_release().
 * @ref arnm_init_arena_borrow() is handed a block the caller already has -- a stack array,
 * static storage, a slice of something bigger -- and never frees it. Everything after that
 * point behaves identically.
 *
 * ### When it runs out
 *
 * A full arena answers @ref ARNM_ERROR_OUT_OF_MEMORY and does not reach for the host. It also
 * remembers how much it had to refuse, which is what @ref arnm_arena_overflow_total() is for:
 * run the real workload once, read the figure, and size the arena by it instead of by guess.
 * A chain (@ref arnm_multi_arena) is the other answer -- it opens more ground instead.
 *
 * @{
 */

/**
 * @brief Take @p capacity bytes from the host and make @p memory an arena over them.
 *
 * @param[out] memory   Handle to initialize; not NULL. Every field is written and none is read,
 *                      so uninitialized storage is a valid input.
 * @param[in]  capacity Bytes to reserve; must be > 0, rounded up to 8.
 * @retval ARNM_SUCCESS                   The arena is ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER        @p memory is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p capacity is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p capacity exceeds @ref ARNM_MAX_ALLOC_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The host had no such block. @p memory is untouched.
 * @note The block always comes from the host, never from another arnm allocator: an arena that
 *       drew from one could not outgrow it, and sizing is the whole reason to use one.
 * @warning Calling this on an arena that already owns a block leaks it. Use
 *          `arnm_reinit_arena()` or @ref arnm_release() first.
 * @whisper Ground claimed once, and marked from one end
 */
arnm_result arnm_init_arena(arnm *memory, uint32_t capacity);

/**
 * @brief Make @p memory an arena over a block the caller already owns.
 *
 * The block is never freed, not by @ref arnm_release() and not by @ref arnm_destroy(); it
 * outlives the arena and stays the caller's to dispose of.
 *
 * @param[out] memory   Handle to initialize; not NULL. Written in full, read not at all.
 * @param[in]  data     The block; not NULL, and its address must be a multiple of 8.
 * @param[in]  capacity Bytes in @p data; must be > 0 and a multiple of 8.
 * @retval ARNM_SUCCESS                   The arena is ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER        @p memory or @p data is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p capacity is 0, not a multiple of 8, or @p data is
 *                                        not 8 byte aligned.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p capacity exceeds @ref ARNM_MAX_ALLOC_SIZE.
 * @note Both are rejected rather than rounded, and for different reasons: an unaligned base
 *       would break the promise that every block handed out is 8 byte aligned, and a rounded up
 *       capacity would let the index walk past the end of a buffer you sized exactly.
 *       `alignas(8)` on the array is usually all it takes.
 * @whisper Ground that was already there, now measured
 */
arnm_result arnm_init_arena_borrow(arnm *memory, uint8_t *data, uint32_t capacity);

/**
 * @brief Release whatever @p memory held, then @ref arnm_init_arena() at a new size.
 *
 * The one safe way to resize an arena: what it owned goes back before the new block is asked
 * for. A borrowed block is let go untouched, as always.
 *
 * @param[in,out] memory   Handle to re-initialize; not NULL.
 * @param[in]     capacity New size in bytes; must be > 0.
 * @return As @ref arnm_init_arena(). On failure @p memory is left released and empty: it still
 *         reads as an arena, and allocations from it answer @ref ARNM_ERROR_INVALID_STATE.
 * @warning Every block ever handed out by @p memory is dangling afterwards.
 * @whisper The same hand, a different field
 */
static inline arnm_result arnm_reinit_arena(arnm *memory, uint32_t capacity) {
  arnm_release(memory);
  return arnm_init_arena(memory, capacity);
}

/**
 * @brief Does @p memory allocate by moving an index?
 *
 * @param[in] memory Handle to ask; NULL answers false.
 * @return true for an arena, owned or borrowed, **and for a chain** -- a chain allocates by
 *         moving an index too, just in more than one block. False only for host mode.
 * @note Use @ref arnm_is_multi_arena() to tell the two apart. A true answer implies
 *       `memory != NULL`, so callers can skip their own null check.
 */
bool arnm_is_arena(const arnm *memory);

/**
 * @brief Bytes this arena had to refuse since the last reset, saturating.
 *
 * Every request an arena could not serve adds its rounded up size here. The figure says how
 * much larger the arena would have had to be, so a run under real load answers the sizing
 * question that a guess only postpones.
 *
 * @param[in] memory Handle to ask.
 * @return The accumulated shortfall, capped at `UINT32_MAX` rather than rolled over. 0 for
 *         NULL, for host mode, and for a chain -- a chain opens more ground instead of
 *         refusing, so it has nothing to record.
 * @note @ref arnm_reset() and @ref arnm_release() clear it.
 * @whisper The measure of what the ground could not hold
 */
size_t arnm_arena_overflow_total(const arnm *memory);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_ARENA_MEMORY_H
