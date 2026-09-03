#ifndef ARNM_DYNAMIC_ARENA_POOL_H
#define ARNM_DYNAMIC_ARENA_POOL_H

#include <stdint.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_dynamic_arena_pool arnm_dynamic_arena_pool
 * @brief Equal sized arenas that are made on demand and kept in stock up to a limit.
 *
 * The other side of @ref arnm_fixed_arena_pool. That one reserves everything at init and
 * refuses once the last arena is out; this one asks the host for another arena the moment the
 * shelf is bare, and lets an arena go again when the shelf is full. The peak is therefore not
 * known in advance -- what is known instead is the resting size: at most @c spare_limit arenas
 * are ever held while nothing is out.
 *
 * That is the shape for a load that breathes. A quiet minute leaves the pool holding
 * @c spare_limit arenas and no more; a burst is served rather than refused, and what the burst
 * needed drains away as the arenas come back.
 *
 * ### One host allocation per arena
 *
 * `arnm_alloc(..., NULL)` and `arnm_free(..., NULL)` -- the host, malloc and free, never
 * another arnm allocator. An arena that drew from one could not outlive it, and a pool whose
 * whole purpose is to grow past what was foreseen has no business growing inside a fixed
 * block. Each arena is a single allocation holding its descriptor and its buffer together, so
 * one call makes an arena and one call lets it go.
 *
 * ### Counts and their ceilings
 *
 * @c acquired_count is a `uint32_t`, so `UINT32_MAX` arenas can be out at once and the
 * `UINT32_MAX + 1`st is answered with @ref ARNM_ERROR_RESOURCE_EXHAUSTED. Reaching that with
 * arenas of the smallest legal size would take 32 GB of host memory, so in practice the host
 * says no first, as @ref ARNM_ERROR_OUT_OF_MEMORY. Both are refusals that change nothing.
 *
 * ### Memory that comes back
 *
 * | call | what it does |
 * |---|---|
 * | `_alloc` | pops the free list, or makes an arena when it is empty |
 * | `_free` | resets the arena, then keeps it if the stock is below @c spare_limit, otherwise gives
 * it to the host | | `_reserve` | fills the stock ahead of time, so the first callers do not pay
 * for it | | `_release` | gives every spare arena back, but only once every lent one has returned |
 * | `_destroy` | `_release`, then the descriptor itself |
 *
 * `_release` refuses with @ref ARNM_ERROR_RESOURCE_IN_USE while anything is still out, the
 * same as the fixed pool: a pool cannot know what a caller is doing with an arena it holds.
 *
 * @note The free list lives in the free arenas, exactly as in @ref arnm_fixed_arena_pool: the
 * first bytes of a free arena's buffer hold the address of the next free one. Nothing is
 * allocated for the list and its length cannot drift from the stock.
 *
 * @note Nothing is stored that could be handed in -- with one exception that is not a choice:
 * the arenas come from the host, always, so there is no allocator to remember and none to pass
 * to `_release()`. The descriptor of a pool made by @ref arnm_dynamic_arena_pool_create() is
 * the one thing an allocator may hold, and that allocator is the caller's to keep.
 *
 * @note No hidden state and no locks. One pool used from two threads at once is a data race.
 * A pool per thread is the intended shape.
 *
 * @whisper The shelf refills itself, and never grows taller than it was told to
 *
 * @{
 */

/**
 * @brief Arenas currently in stock, and the counts that bound them.
 *
 * arnm_dynamic_arena_pool_init() writes every field and reads none, so uninitialized storage
 * is a valid input. A capacity of 0 is the empty state: the pool was never initialized or has
 * been released, and every call says so.
 *
 * @note Do not write these fields; read them through the API.
 */
typedef struct arnm_dynamic_arena_pool {
  /** First spare arena, or NULL when the stock is empty. The rest hang off it. */
  arnm *free_head;
  /** Bytes each arena holds, rounded up to 8 at init. Every arena has this same size. */
  uint32_t arena_capacity;
  /** How many are out with a caller right now. What _release() refuses on. */
  uint32_t acquired_count;
  /** Spare arenas on the free list. Never above @c spare_limit. */
  uint32_t spare_count;
  /** How many spares to keep. A return beyond it goes to the host instead of the shelf. */
  uint32_t spare_limit;
} arnm_dynamic_arena_pool;

/**
 * @brief Settle the arena size and the size of the stock. Nothing is allocated here.
 *
 * The pool starts empty and stays that way until the first request, which makes the first
 * arena. This call therefore cannot run out of memory -- it only writes down the two figures
 * every later call is measured against. @ref arnm_dynamic_arena_pool_reserve() is how a caller
 * pays for the stock up front rather than at the first request.
 *
 * Writes every field and reads none, so uninitialized storage is a valid input.
 *
 * @param[in,out] pool           Pool to initialize; not NULL. Need not be zeroed.
 * @param[in]     arena_capacity Bytes each arena holds, rounded up to 8; must be > 0. The
 *                               rounded figure is what the pool stores and what every arena gets.
 * @param[in]     spare_limit    Arenas to keep in stock. 0 is allowed and means every returned
 *                               arena goes straight back to the host.
 * @retval ARNM_SUCCESS                   The pool is ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER        @p pool is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p arena_capacity is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW An arena and its descriptor together would not fit
 *                                           @ref ARNM_MAX_ALLOC_SIZE.
 * @warning Calling this on a pool that still holds arenas leaks them. Use
 *          arnm_dynamic_arena_pool_release() first.
 * @whisper The measure is set; the shelf fills itself later
 */
arnm_result arnm_dynamic_arena_pool_init(
    arnm_dynamic_arena_pool *pool, uint32_t arena_capacity, uint32_t spare_limit
);

/**
 * @brief Allocate a pool descriptor and initialize it.
 *
 * One allocator, and only for the descriptor. The arenas are the host's business and no
 * argument here changes that.
 *
 * @param[in]     arena_capacity As in arnm_dynamic_arena_pool_init().
 * @param[in]     spare_limit    As in arnm_dynamic_arena_pool_init().
 * @param[in,out] allocator      Allocator to take this descriptor from, or NULL for malloc.
 * @return Initialized pool, or NULL when @p allocator had no room or
 *         arnm_dynamic_arena_pool_init() refused the arguments.
 * @note Pair with arnm_dynamic_arena_pool_destroy() **and hand it the same @p allocator**.
 * @whisper A vessel for vessels, drawn from whichever stream the host points to
 */
arnm_dynamic_arena_pool *arnm_dynamic_arena_pool_create(
    uint32_t arena_capacity, uint32_t spare_limit, arnm *allocator
);

/**
 * @brief Fill the stock up to @p count arenas now, so the first callers do not wait for them.
 *
 * Makes arenas until the stock reaches @p count and stops there; a stock already that deep is
 * left alone and answers ARNM_SUCCESS. Either every missing arena is made or none is kept --
 * a partial fill would leave the pool in a state no argument describes, so what was made on
 * the way to a failure goes straight back to the host.
 *
 * @param[in,out] pool  Pool to fill; not NULL.
 * @param[in]     count Arenas the stock should hold; must not exceed @c spare_limit.
 * @retval ARNM_SUCCESS               The stock holds at least @p count arenas.
 * @retval ARNM_ERROR_NULL_POINTER    @p pool is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p pool has no capacity; it was never initialized, or has
 *                                       been released.
 * @retval ARNM_ERROR_INVALID_PARAM   @p count is above @c spare_limit. The stock never rises
 *                                       above that line, so filling past it could not hold.
 * @retval ARNM_ERROR_OUT_OF_MEMORY   The host had no room. Nothing partial is kept.
 * @whisper The shelf is stocked before the day begins
 */
arnm_result arnm_dynamic_arena_pool_reserve(arnm_dynamic_arena_pool *pool, uint32_t count);

/**
 * @brief Take an arena out of the stock, or make one when the stock is empty.
 *
 * The arena arrives empty and is an ordinary @ref arnm -- allocate from it, reset it, measure
 * it. The pool stops knowing about it until arnm_dynamic_arena_pool_free() brings it back, so
 * the only thing not permitted is letting it go out of scope without returning it.
 *
 * O(1) when the stock has something; one host allocation when it does not.
 *
 * @param[in,out] pool Pool to draw from; not NULL.
 * @param[out]    out  Receives the arena; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS                   An arena was handed over.
 * @retval ARNM_ERROR_NULL_POINTER        @p pool or @p out is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p pool has no capacity; it was never initialized, or
 *                                           has been released.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED  `UINT32_MAX` arenas are already out and the count
 *                                           cannot carry another. Nothing is wrong with the
 *                                           request; the counter is the size it is.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The stock was empty and the host had no room for a
 *                                           new arena.
 * @note Never arnm_release() an arena from here. Its buffer sits in the same allocation as its
 *       descriptor, and arnm_dynamic_arena_pool_free() is how the pair comes back.
 * @whisper A vessel leaves the shelf, or is shaped where the shelf was bare
 */
arnm_result arnm_dynamic_arena_pool_alloc(arnm_dynamic_arena_pool *pool, arnm **out);

/**
 * @brief Give an arena back: onto the shelf while it has room, otherwise to the host.
 *
 * The arena is emptied first, so an arena on the shelf always waits in the state a fresh one
 * would arrive in. Whether it is kept is decided by @c spare_limit alone, and either way every
 * pointer it handed out dangles from here on.
 *
 * @param[in,out] pool  Pool the arena came from; not NULL.
 * @param[in,out] arena Arena from arnm_dynamic_arena_pool_alloc(); not NULL. Freed outright
 *                      when the stock is already full -- do not touch it afterwards.
 * @retval ARNM_SUCCESS               Kept in stock, or given back to the host.
 * @retval ARNM_ERROR_NULL_POINTER    @p pool or @p arena is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p pool has no capacity.
 * @retval ARNM_ERROR_INVALID_STATE   Nothing is out, so this arena cannot be coming back --
 *                                       the plainest case of a second return of the same arena,
 *                                       which would otherwise knot the free list into a ring.
 * @warning Where @ref arnm_fixed_arena_pool_free() can tell an arena of its own from a
 *          stranger, this one cannot: its arenas are scattered across the host and there is no
 *          block to compare an address against. An arena from somewhere else is taken in and
 *          later freed as if it were the pool's. Return each arena to the pool it came from,
 *          exactly once.
 * @whisper A vessel returns, emptied -- kept if there is room, released if there is not
 */
arnm_result arnm_dynamic_arena_pool_free(arnm_dynamic_arena_pool *pool, arnm *arena);

/**
 * @brief Give every spare arena back to the host, once the lent ones have come home.
 *
 * Leaves the pool in the empty state it had before init, keeping the descriptor itself. The
 * refusal below is the point of the call: a pool cannot know what a caller is doing with an
 * arena it still holds, so it does not take it away.
 *
 * @param[in,out] pool Pool to empty; not NULL.
 * @retval ARNM_SUCCESS               Every spare arena returned, pool empty.
 * @retval ARNM_ERROR_NULL_POINTER    @p pool is NULL.
 * @retval ARNM_ERROR_RESOURCE_IN_USE At least one arena is still out. Nothing was changed;
 *                                       return them and call again.
 * @note No allocator argument, and none is missing: every arena here came from the host and
 *       goes back to it.
 * @whisper The shelf comes down, but not while anything still rests on it
 */
arnm_result arnm_dynamic_arena_pool_release(arnm_dynamic_arena_pool *pool);

/**
 * @brief arnm_dynamic_arena_pool_release(), then give the descriptor itself back.
 *
 * @param[in,out] pool      From arnm_dynamic_arena_pool_create(), never stack or static
 *                          storage; may be NULL.
 * @param[in,out] allocator The allocator @p pool itself came from -- the same one
 *                          arnm_dynamic_arena_pool_create() was handed, NULL for malloc.
 * @retval ARNM_SUCCESS               Released, or @p pool was NULL and there was nothing to do.
 * @retval ARNM_ERROR_RESOURCE_IN_USE An arena is still out. **Nothing was released and the
 *                                       descriptor is still yours** -- this is the one failure
 *                                       here that leaves something to call again.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED An arena kept the descriptor. It is gone
 *                                       from the caller's side regardless.
 * @warning An @p allocator other than the one that handed the descriptor out leaves those
 *          bytes where they are; NULL in place of an arena reaches free() unchecked. See
 *          arnm_free().
 * @whisper The vessel that held vessels returns to the stream it came from
 */
arnm_result arnm_dynamic_arena_pool_destroy(arnm_dynamic_arena_pool *pool, arnm *allocator);

/**
 * @brief Arenas on the shelf, ready to be handed out without asking the host.
 *
 * @param[in] pool Pool to query; may be NULL.
 * @return Spare arenas in stock, or 0 if @p pool is NULL.
 * @note Not a ceiling. A request past this one is served by making an arena, not refused.
 */
static inline uint32_t arnm_dynamic_arena_pool_available(const arnm_dynamic_arena_pool *pool) {
  return pool ? pool->spare_count : 0u;
}

/**
 * @brief Bytes this pool holds from the host right now, spare and lent alike.
 *
 * Every arena the pool has made and not yet given back, descriptors included. Unlike the fixed
 * pool's constant figure this one moves with the load: it is what the host is carrying at this
 * moment, which is the number a budget is read against and the number a leak shows up in.
 *
 * @param[in] pool Pool to measure; may be NULL.
 * @return Bytes currently held, or 0 if @p pool is NULL or holds nothing.
 * @note `uint64_t`, where @ref arnm_fixed_arena_pool_reserved() answers in `uint32_t`. A fixed
 *       pool is one allocation and cannot pass @ref ARNM_MAX_ALLOC_SIZE; this one is many, and
 *       enough of them do pass it. The sizes it hands to the host stay `uint32_t` as everything
 *       here does -- only their sum needs the wider type.
 * @whisper What the shelf weighs, this moment and not the next
 */
uint64_t arnm_dynamic_arena_pool_reserved(const arnm_dynamic_arena_pool *pool);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_DYNAMIC_ARENA_POOL_H
