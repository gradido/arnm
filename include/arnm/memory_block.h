#ifndef ARNM_MEMORY_BLOCK_H
#define ARNM_MEMORY_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup arnm_memory_block arnm_memory_block
 *  @brief A pointer and its size, kept together.
 *
 *  @ref arnm keeps no per allocation bookkeeping, so arnm_free() and arnm_realloc()
 *  ask the caller for the size the buffer was allocated with -- fine where that size is
 *  obvious, a hazard where it is not. A block carries it along and updates both halves in
 *  one step, so they cannot drift apart. Every function here is a thin inline wrapper and
 *  passes the allocator straight through; NULL still means malloc/free.
 *
 *  @c size records what the block was *allocated* with, because that is the number the
 *  arnm_* calls have to be told later. It is not the caller's logical length.
 *
 *  In arena mode a resize or free of anything but the tail returns
 *  @ref ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED. What that leaves in the descriptor differs
 *  per function, so check the warning explicitly: `ARNM_SUCCESS != result` reads a completed
 *  resize as a failure, and ignoring it reads memory the arena still holds as released.
 *  Which of the two matters depends on the call site.
 *  @{
 */

/** @brief A buffer and the size it was allocated with.
 *
 *  A zeroed block (@c arnm_memory_block b = {0};) is the empty state, and what the free
 *  functions leave behind when they reclaimed.
 *
 *  @var arnm_memory_block::data
 *  Start of the buffer, or NULL when empty. 8 byte aligned in arena mode.
 *  @var arnm_memory_block::size
 *  Bytes requested from the allocator; an arena reserved ARNM_ALIGN8(size) for them.
 */
typedef struct arnm_memory_block {
  uint8_t *data;
  uint32_t size;
} arnm_memory_block;

/** @brief Allocate a block and record its size.
 *
 *  @param[out]    memory_block Descriptor to fill; not NULL.
 *  @param[in]     size         Bytes to allocate; must be > 0.
 *  @param[in,out] memory       Allocator to draw from, or NULL for malloc.
 *  @retval ARNM_SUCCESS            Block allocated, @c size recorded.
 *  @retval ARNM_ERROR_NULL_POINTER @p memory_block is NULL.
 *  @retval Anything arnm_alloc() can return.
 *  @note Overwrites the descriptor unseen; release a previous block first.
 *  @whisper A vessel carved from the flowing stream
 */
static inline arnm_result arnm_memory_block_alloc(
    arnm_memory_block *memory_block, uint32_t size, arnm *memory
) {
  if (!memory_block) { return ARNM_ERROR_NULL_POINTER; }
  arnm_result result = arnm_alloc(&memory_block->data, size, memory);
  if (ARNM_SUCCESS == result) { memory_block->size = size; }
  return result;
}

/** @brief Release a block and clear its descriptor.
 *
 *  The descriptor is only zeroed when the bytes really came back. An arena that could not
 *  reclaim leaves it pointing at storage that stays valid until arnm_reset() -- the
 *  block is then neither released nor safe to keep indefinitely, so handle the warning.
 *
 *  @param[in,out] memory_block Descriptor to release; not NULL.
 *  @param[in,out] memory       Allocator the block came from, or NULL for free().
 *  @retval ARNM_SUCCESS            Block released, descriptor zeroed.
 *  @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Not the arena's tail, so nothing came back
 *                                 and the descriptor is unchanged. An already empty block
 *                                 reports this too: NULL is never the tail.
 *  @retval ARNM_ERROR_NULL_POINTER @p memory_block is NULL.
 *  @note arnm_memory_block_realloc() with @c new_size 0 is interchangeable with this.
 *  @whisper The vessel returns to water, form dissolving
 */
static inline arnm_result arnm_memory_block_free(arnm_memory_block *memory_block, arnm *memory) {
  if (!memory_block) { return ARNM_ERROR_NULL_POINTER; }
  arnm_result result = arnm_free(memory_block->data, memory_block->size, memory);
  // reset only if bytes where truly deleted
  if (ARNM_SUCCESS == result) {
    memory_block->data = NULL;
    memory_block->size = 0;
  }
  return result;
}

/** @brief Resize a block, keeping pointer and size in step.
 *
 *  See arnm_realloc() for what each allocator does. @c size follows whenever the allocation
 *  really changed: every success, plus the buried grow, where the arena could not resize in
 *  place and moved the block to a fresh, larger one. It deliberately does not follow a
 *  buried shrink -- nothing moved there, the arena still holds the original bytes, and
 *  recording the smaller number would strand the block for good, because a size that does
 *  not match the reservation never matches the arena tail.
 *
 *  @param[in,out] memory_block Descriptor to resize; not NULL.
 *  @param[in]     new_size     Requested size, or 0 to release the block.
 *  @param[in,out] memory       Allocator the block came from, or NULL for realloc.
 *  @retval ARNM_SUCCESS            Block resized, @c size updated.
 *  @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED The arena kept bytes the block no longer
 *                                 needs. On a grow the block moved and @c size follows; on a
 *                                 shrink nothing happened; on @p new_size 0 nothing was
 *                                 released.
 *  @retval ARNM_ERROR_NULL_POINTER @p memory_block is NULL.
 *  @retval Anything arnm_realloc() can return; the descriptor is untouched then, so the block
 *          stays usable at its previous size.
 *  @note @p new_size 0 is interchangeable with arnm_memory_block_free().
 *  @whisper The vessel widens or narrows, the water within untouched
 */
static inline arnm_result arnm_memory_block_realloc(
    arnm_memory_block *memory_block, uint32_t new_size, arnm *memory
) {
  if (!memory_block) { return ARNM_ERROR_NULL_POINTER; }
  uint8_t *before = memory_block->data;
  arnm_result result = arnm_realloc(&memory_block->data, memory_block->size, new_size, memory);
  // the index moved (success), or the arena handed us a new block -- either way the
  // allocation changed and the recorded size has to follow. Observing the pointer rather
  // than inferring from the sizes stays right if arnm_realloc ever moves blocks elsewhere.
  if (ARNM_SUCCESS == result || before != memory_block->data) { memory_block->size = new_size; }
  return result;
}

/** @brief Copy a block into a freshly allocated one.
 *
 *  @param[out]    dst    Descriptor to fill; not NULL.
 *  @param[in]     src    Block to copy; not NULL and non empty.
 *  @param[in,out] memory Allocator for @p dst, or NULL for malloc. Need not be @p src's.
 *  @retval ARNM_SUCCESS             Copy allocated and filled.
 *  @retval ARNM_ERROR_NULL_POINTER  @p dst or @p src is NULL.
 *  @retval ARNM_ERROR_INVALID_PARAM @p src is empty.
 *  @retval Anything arnm_clone() can return.
 *  @whisper Water poured into a vessel newly shaped
 */
static inline arnm_result arnm_memory_block_clone(
    arnm_memory_block *dst, const arnm_memory_block *src, arnm *memory
) {
  if (!dst || !src) { return ARNM_ERROR_NULL_POINTER; }
  // size only after the clone succeeded, so a failed dst keeps its old, honest state
  arnm_result result = arnm_clone(&dst->data, src->data, src->size, memory);
  if (ARNM_SUCCESS == result) { dst->size = src->size; }
  return result;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_MEMORY_BLOCK_H
