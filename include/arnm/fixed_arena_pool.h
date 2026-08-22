#ifndef HOSTMEM_FIXED_ARENA_POOL_H
#define HOSTMEM_FIXED_ARENA_POOL_H

#include <stdint.h>

#include "hostmem/memory.h"
#include "hostmem/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup hostmem_fixed_arena_pool hostmem_fixed_arena_pool
 * @brief A fixed set of equal sized arenas, lent out one at a time.
 *
 * Where @ref hostmem_multi_arena grows until the host runs out, this one does not grow at all.
 * @ref hostmem_fixed_arena_pool_init() reserves every arena it will ever have, in a single block,
 * and from then on the pool only hands them out and takes them back. The peak is therefore known
 * the moment init returns, and known exactly: `arena_count * arena_capacity` plus the descriptors.
 * That is what a request handler or a worker pool wants -- a ceiling it can be sized against,
 * rather than an allocator that quietly asks the host for more under load.
 *
 * ### Ownership
 *
 * The pool owns exactly the arenas that are free. @ref hostmem_fixed_arena_pool_alloc() hands one
 * over and forgets it; while the caller holds it the pool knows nothing about it and tracks
 * nothing but a count. @ref hostmem_fixed_arena_pool_free() takes it back, resetting it on the
 * way in, so an arena always arrives empty.
 *
 * No handles, no busy list, no owner bookkeeping. What a caller holds is an ordinary
 * @ref hostmem, usable with every function in @ref hostmem_memory -- the pool is where it came
 * from, not a layer it keeps speaking through.
 *
 * ### The free list lives in the free arenas
 *
 * A free arena has nothing to store, so it stores the link: the first bytes of its buffer hold
 * the address of the next free arena, and the pool holds only the head. Nothing is allocated for
 * the list, and its size cannot drift from the number of arenas. The bytes are overwritten the
 * moment the arena is handed out, which is exactly when the link is no longer needed.
 *
 * ### Memory that comes back
 *
 * | call | what it does |
 * |---|---|
 * | `_alloc` | pops the head of the free list, O(1) |
 * | `_free` | resets the arena and pushes it back, O(1) |
 * | `_release` | gives the whole block back, but only once every arena has been returned |
 * | `_destroy` | `_release`, then the descriptor itself |
 *
 * `_release` refuses with @ref HOSTMEM_ERROR_RESOURCE_IN_USE while anything is still out. It
 * would otherwise pull memory out from under a caller still writing to it, and no return code
 * would make that safe.
 *
 * @note One block, one allocation: the descriptors sit at its front and the buffers behind them.
 * So a @p source arena is asked once and can take the whole pool back in one step, instead of
 * being left with holes it cannot reuse.
 *
 * @note Nothing is stored that could be handed in. The pool keeps its arenas and the counts
 * that describe them, and not one allocator: @p source is given to _init() and again to
 * _release(), the way a size is given to hostmem_alloc() and again to hostmem_free(). Which
 * allocator a pool was built on is the caller's to remember, the same duty the sizes carry.
 *
 * @note No hidden state and no locks. One pool used from two threads at once is a data race --
 * the free list has no synchronisation. A pool per thread is the intended shape.
 *
 * @whisper Vessels lent out and returned, the shelf always the same size
 *
 * @{
 */

/**
 * @brief A fixed set of arenas and the list of those currently free.
 *
 * hostmem_fixed_arena_pool_init() writes every field and reads none, so uninitialized storage is
 * a valid input. Unlike @ref hostmem there is no useful zero state: a pool that owns nothing can
 * hand nothing out, and every call says so rather than pretending.
 *
 * @note Do not write these fields; read them through the API.
 */
typedef struct hostmem_fixed_arena_pool {
  /**
   * The one block this pool owns, seen as the descriptor array that starts it. The buffers
   * follow behind, so this address and the two counts below are enough to give everything back.
   */
  hostmem *arenas;
  /** First free arena, or NULL when every one of them is out. The rest hang off it. */
  hostmem *free_head;
  /** Bytes each arena holds, rounded up to 8 at init. Every arena has this same size. */
  uint32_t arena_capacity;
  /** Arenas in the pool. Fixed at init and never changed. */
  uint16_t arena_count;
  /** How many are out with a caller right now. What _release() refuses on. */
  uint16_t acquired_count;
} hostmem_fixed_arena_pool;

/**
 * @brief Reserve @p arena_count arenas of @p arena_capacity bytes each, all at once.
 *
 * Everything the pool will ever hand out is taken from @p source here, in one block, and the
 * arenas are strung into the free list ready to be lent. A partial reservation is not a state
 * this pool has: either every arena is there when this returns HOSTMEM_SUCCESS, or nothing was
 * kept and @p pool is untouched.
 *
 * Writes every field and reads none, so uninitialized storage is a valid input.
 *
 * @param[in,out] pool           Pool to initialize; not NULL. Need not be zeroed.
 * @param[in]     arena_capacity Bytes each arena holds, rounded up to 8; must be > 0. The
 *                               rounded figure is what the pool stores and what every arena gets.
 * @param[in]     arena_count    Arenas to reserve; must be > 0. This is the ceiling, for good.
 * @param[in,out] source         Allocator the block comes from, or NULL for malloc.
 * @retval HOSTMEM_SUCCESS                   Every arena is reserved and free.
 * @retval HOSTMEM_ERROR_NULL_POINTER        @p pool is NULL.
 * @retval HOSTMEM_ERROR_INVALID_PARAM       @p arena_capacity or @p arena_count is 0.
 * @retval HOSTMEM_ERROR_ARITHMETIC_OVERFLOW The block would not fit
 *                                           @ref HOSTMEM_MAX_ALLOC_SIZE.
 * @retval HOSTMEM_ERROR_OUT_OF_MEMORY       @p source had no room for the whole block. Nothing
 *                                           partial is kept.
 * @warning Calling this on a pool that already holds arenas leaks the old block. Use
 *          hostmem_fixed_arena_pool_release() first.
 * @warning @p source is not remembered. hostmem_fixed_arena_pool_release() has to be handed the
 *          same one; which allocator a pool was built on is the caller's to keep, the way the
 *          sizes everything else here is freed with are. Naming another arena there is caught by
 *          the address check in hostmem_free() and only costs the block, which stays with its
 *          real owner until that arena resets. Naming NULL for memory an arena gave is not
 *          caught at all.
 * @whisper The shelf is built once, and its size is settled from then on
 */
hostmem_result hostmem_fixed_arena_pool_init(
    hostmem_fixed_arena_pool *pool, uint32_t arena_capacity, uint16_t arena_count, hostmem *source
);

/**
 * @brief Allocate a pool descriptor and initialize it.
 *
 * Two allocators, and they answer different questions. @p source holds the arenas, all of them,
 * for the pool's whole life; @p allocator hands out this descriptor, once, here.
 *
 * @param[in]     arena_capacity As in hostmem_fixed_arena_pool_init().
 * @param[in]     arena_count    As in hostmem_fixed_arena_pool_init().
 * @param[in,out] source         As in hostmem_fixed_arena_pool_init(): for the arenas.
 * @param[in,out] allocator      Allocator to take this descriptor from, or NULL for malloc.
 * @return Initialized pool, or NULL when @p allocator had no room or
 *         hostmem_fixed_arena_pool_init() refused the arguments.
 * @note Pair with hostmem_fixed_arena_pool_destroy() **and hand it the same @p allocator**.
 * @whisper A vessel for vessels, drawn from whichever stream the host points to
 */
hostmem_fixed_arena_pool *hostmem_fixed_arena_pool_create(
    uint32_t arena_capacity, uint16_t arena_count, hostmem *source, hostmem *allocator
);

/**
 * @brief Take a free arena out of the pool and hand it to the caller. O(1).
 *
 * The arena arrives empty and is an ordinary @ref hostmem: allocate from it, reset it, measure
 * it. The pool stops knowing about it until hostmem_fixed_arena_pool_free() brings it back, so
 * the only thing not permitted is letting it go out of scope without returning it.
 *
 * @param[in,out] pool Pool to draw from; not NULL.
 * @param[out]    out  Receives the arena; not NULL. Untouched on failure.
 * @retval HOSTMEM_SUCCESS                   An arena was handed over.
 * @retval HOSTMEM_ERROR_NULL_POINTER        @p pool or @p out is NULL.
 * @retval HOSTMEM_ERROR_NOT_INITIALIZED     @p pool holds no arenas; it was never initialized,
 *                                           or has been released.
 * @retval HOSTMEM_ERROR_RESOURCE_EXHAUSTED  Every arena is out with someone. Nothing is wrong
 *                                           with the request; the pool is simply the size it is.
 * @note Never hostmem_release() an arena from here. Its buffer belongs to the pool's block, and
 *       hostmem_fixed_arena_pool_free() is how it comes back.
 * @whisper A vessel leaves the shelf, and the shelf forgets it
 */
hostmem_result hostmem_fixed_arena_pool_alloc(hostmem_fixed_arena_pool *pool, hostmem **out);

/**
 * @brief Give an arena back, reset on the way in. O(1).
 *
 * The arena is emptied before it rejoins the free list, so the next caller receives it in the
 * same state the first one did. Every pointer it handed out dangles from here on.
 *
 * @param[in,out] pool  Pool the arena came from; not NULL.
 * @param[in,out] arena Arena from hostmem_fixed_arena_pool_alloc(); not NULL.
 * @retval HOSTMEM_SUCCESS               Reset and back on the free list.
 * @retval HOSTMEM_ERROR_NULL_POINTER    @p pool or @p arena is NULL.
 * @retval HOSTMEM_ERROR_NOT_INITIALIZED @p pool holds no arenas.
 * @retval HOSTMEM_ERROR_INVALID_PARAM   @p arena is not one of this pool's arenas.
 * @retval HOSTMEM_ERROR_INVALID_STATE   Nothing is out, so this arena cannot be coming back --
 *                                       the plainest case of a second return of the same arena,
 *                                       which would otherwise knot the free list into a ring.
 * @warning Returning the same arena twice while others are still out is not caught: without a
 *          busy list there is nothing to check it against, and that absence is what makes the
 *          pool free of bookkeeping. Return each arena once.
 * @whisper A vessel returns, emptied, and takes its place on the shelf
 */
hostmem_result hostmem_fixed_arena_pool_free(hostmem_fixed_arena_pool *pool, hostmem *arena);

/**
 * @brief Give the whole block back to @p source, once every arena has come home.
 *
 * Leaves the pool in the empty state it had before init, keeping the descriptor itself. The
 * refusal below is the point of the call: a pool cannot know what a caller is doing with an
 * arena it still holds, so it does not take it away.
 *
 * @param[in,out] pool   Pool to empty; not NULL.
 * @param[in,out] source The allocator the block came from -- the same one
 *                       hostmem_fixed_arena_pool_init() was handed, NULL for malloc.
 * @retval HOSTMEM_SUCCESS               Block returned, pool empty.
 * @retval HOSTMEM_ERROR_NULL_POINTER    @p pool is NULL.
 * @retval HOSTMEM_ERROR_RESOURCE_IN_USE At least one arena is still out. Nothing was changed;
 *                                       return them and call again.
 * @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED @p source is an arena and the block is not
 *                                       its most recent allocation. The pool is empty either
 *                                       way; those bytes come back on that arena's own reset.
 * @warning A @p source other than the one the pool was built on leaves the block where it is
 *          and answers with the warning above -- except NULL for memory an arena gave, which
 *          reaches free() unchecked. See hostmem_free().
 * @whisper The shelf comes down, but not while anything still rests on it
 */
hostmem_result hostmem_fixed_arena_pool_release(hostmem_fixed_arena_pool *pool, hostmem *source);

/**
 * @brief hostmem_fixed_arena_pool_release(), then give the descriptor itself back.
 *
 * @param[in,out] pool      From hostmem_fixed_arena_pool_create(), never stack or static
 *                          storage; may be NULL.
 * @param[in,out] source     The allocator the arenas came from, passed straight to
 *                           hostmem_fixed_arena_pool_release(), NULL for malloc.
 * @param[in,out] allocator The allocator @p pool itself came from -- the same one
 *                          hostmem_fixed_arena_pool_create() was handed, NULL for malloc. The
 *                          two are separate and are usually not the same allocator.
 * @retval HOSTMEM_SUCCESS               Released, or @p pool was NULL and there was nothing
 *                                       to do.
 * @retval HOSTMEM_ERROR_RESOURCE_IN_USE An arena is still out. **Nothing was released and the
 *                                       descriptor is still yours** -- this is the one failure
 *                                       here that leaves something to call again.
 * @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED An arena kept the block or the descriptor.
 *                                       Both are gone from the caller's side regardless.
 * @warning An @p allocator or @p source other than the one that handed the memory out leaves
 *          that memory where it is; NULL in place of an arena reaches free() unchecked. See
 *          hostmem_free().
 * @whisper The vessel that held vessels returns to the stream it came from
 */
hostmem_result hostmem_fixed_arena_pool_destroy(
    hostmem_fixed_arena_pool *pool, hostmem *source, hostmem *allocator
);

/**
 * @brief Arenas currently free and ready to be handed out.
 *
 * @param[in] pool Pool to query; may be NULL.
 * @return @c arena_count minus what is out, or 0 if @p pool is NULL.
 */
static inline uint16_t hostmem_fixed_arena_pool_available(const hostmem_fixed_arena_pool *pool) {
  return pool ? (uint16_t)(pool->arena_count - pool->acquired_count) : (uint16_t)0;
}

/**
 * @brief Bytes this pool holds from the host, arenas and descriptors together.
 *
 * The whole footprint, known from init and constant until release -- what a host sizes its own
 * budget against.
 *
 * @param[in] pool Pool to measure; may be NULL.
 * @return Bytes of the single block, or 0 if @p pool is NULL or holds nothing.
 */
uint32_t hostmem_fixed_arena_pool_reserved(const hostmem_fixed_arena_pool *pool);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // HOSTMEM_FIXED_ARENA_POOL_H
