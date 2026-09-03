#ifndef ARNM_GRADED_ARENA_POOL_H
#define ARNM_GRADED_ARENA_POOL_H

#include <stdint.h>

#include "arnm/bitmap.h"
#include "arnm/dynamic_arena_pool.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_graded_arena_pool arnm_graded_arena_pool
 * @brief Arenas in a few settled sizes; a request is served by the next one up.
 *
 * @ref arnm_dynamic_arena_pool lends arenas of one size. This holds several of those, one per
 * size named at init -- 128, 256, 512, 1024, whatever the work actually asks for -- and turns a
 * caller's number into the smallest arena that can carry it. A request for 300 bytes against
 * that ladder comes back as a 512 byte arena; a request for 5000 comes back as
 * @ref ARNM_ERROR_RESOURCE_SIZE_EXCEED, because no waiting and no returning would ever make the
 * ladder taller.
 *
 * ### Every grade is a power of two
 *
 * 8, 16, 32 and on up to 2147483648 -- twenty nine sizes to choose from, and nothing between
 * them. A size that is not one of those is refused at init rather than rounded to one, so a
 * ladder is never quietly something other than what was written down.
 *
 * That restriction is what buys the lookup. A power of two is its own index: the exponent is
 * where the size sits in a 32 bit mask, so @ref arnm_graded_arena_pool::grade_bits can say
 * which sizes exist in one word, and finding the grade for a request is a rounding step, a
 * mask, a bit scan and a bit count -- the same handful of instructions whether the ladder has
 * two rungs or twenty nine. Nothing is searched and nothing is walked.
 *
 * The mask costs no memory: it sits in bytes the descriptor was padding out anyway.
 *
 * ### What the grades cost
 *
 * They are the tuning knob, and powers of two make it a coarse one: a request lands in an arena
 * at most twice its size, and 130 bytes in a 256 byte arena leaves nearly half untouched. That
 * slack is memory held for as long as the arena is out, and for good where a grade keeps stock.
 * Naming every rung the work touches fits tighter and holds more stock, since every grade keeps
 * its own; leaving rungs out saves the stock and widens the gaps. Run the workload, look at what
 * it asked for, then name the sizes -- and where a size sits just above a rung, the honest fix
 * is usually the work asking for less rather than the ladder gaining a rung it cannot have.
 *
 * ### What it is not
 *
 * Not a general allocator. It hands out whole arenas, and every one of them is a
 * @ref arnm_memory handle the caller allocates from as usual. The size named here is the
 * arena's capacity, not one object's -- an arena of 512 bytes serves a request for 300 and has
 * room left for whatever else that piece of work needs.
 *
 * ### Ownership
 *
 * The arenas come from the host, the grades hold them, and a returned arena goes back to the
 * grade whose capacity it carries -- found from the arena itself in the same constant time, so
 * a caller returns an arena without remembering which size it asked for. Everything
 * @ref arnm_dynamic_arena_pool says about stock and ceilings holds here, once per grade.
 *
 * ### Memory that comes back
 *
 * | call | what it does |
 * |---|---|
 * | `_alloc` | the smallest grade that fits, then that grade's `_alloc` |
 * | `_free` | the grade the arena's capacity names, then that grade's `_free` |
 * | `_release` | every grade released, but only once every lent arena has returned |
 * | `_destroy` | `_release`, then the descriptor itself |
 *
 * @note The lookup was measured against the walk it replaces, both in place and in the same
 * harness, as the cost a round trip pays for its two searches together. The walk ran 2.0 ns
 * where the first rung already fits, 2.9, 6.4 and 11.6 ns as ladders of 2, 8 and 16 rungs were
 * climbed to the top. This one runs 2.9 ns, and runs 2.9 ns for every row of that table. So it
 * costs about a nanosecond where a short ladder is asked for a small size, and saves up to
 * nine at the other end.
 *
 * @note What settled it is not the nanoseconds either way but that the figure stopped depending
 * on the ladder: a rung added costs nothing anywhere else, and no request is slower than
 * another. A binary search was measured too and is never the best of the three -- slower than
 * the walk on short ladders and slower than this on tall ones -- and a branchless one is slower
 * again, the array never leaving L1 at these lengths.
 *
 * @note No hidden state and no locks. One pool used from two threads at once is a data race.
 * A pool per thread is the intended shape.
 *
 * @whisper Every request finds the rung above it, or is told the ladder ends here
 *
 * @{
 */

/**
 * @brief The grades, in ascending capacity, and how many there are.
 *
 * arnm_graded_arena_pool_init() writes every field and reads none, so uninitialized storage is
 * a valid input. A NULL @c grades is the empty state: the pool was never initialized or has
 * been released, and every call says so.
 *
 * @note Do not write these fields; read them through the API.
 */
typedef struct arnm_graded_arena_pool {
  /**
   * One dynamic pool per size, ascending and strictly so, packed with no gaps. The sizes are
   * not stored a second time -- each grade carries its own in @c arena_capacity, and
   * @c grade_bits says which ones the ladder has.
   */
  arnm_dynamic_arena_pool *grades;
  /** Grades in the ladder. Fixed at init and never changed; equals the bits set below. */
  uint16_t grade_count;
  /**
   * Which sizes exist: bit @c e set means a grade of `1 << e` bytes, so bit 8 is the 256 byte
   * grade. Bits 0 to 2 are never set -- 8 bytes is the smallest arena there is.
   *
   * Counting the bits below one gives that grade's place in @c grades, which is what makes the
   * lookup constant time. It sits in bytes the two fields above were padding out, so the
   * descriptor is the same 16 bytes it would be without it.
   */
  uint32_t grade_bits;
} arnm_graded_arena_pool;

/** The smallest grade a ladder can have: an arena narrower than its own free list link. */
#define ARNM_GRADED_MIN_SIZE 8u

/** The largest, being the largest power of two a `uint32_t` size can name. */
#define ARNM_GRADED_MAX_SIZE 2147483648u

/** Grades a ladder can hold at most: the powers of two from 8 to 2147483648, and no others. */
#define ARNM_GRADED_MAX_GRADE_COUNT 29u

/**
 * @brief Open one dynamic pool per size in @p sizes. No arenas are made here.
 *
 * The sizes become the grades in the order they arrive, which must be ascending and strictly
 * so, and each one must be a power of two between @ref ARNM_GRADED_MIN_SIZE and
 * @ref ARNM_GRADED_MAX_SIZE. Nothing is rounded here, unlike everywhere else in arnm: a size
 * of 100 is not quietly served as 128, because the grades are what a caller sizes a memory
 * budget against and a ladder that is not what it says would move that budget without saying
 * so. The gap between rungs is already the coarse part; the rungs themselves are exact.
 *
 * Every size is checked before anything is allocated, so a refusal leaves @p pool untouched
 * and the host unasked. The grades themselves start empty; the first request to a grade makes
 * its first arena. arnm_graded_arena_pool_grade_at() reaches a grade to fill it up front.
 *
 * The ladder need not be a run: 8, 64, 4096 is as valid as 8, 16, 32, and a request that falls
 * in a gap climbs to the next rung that exists. Leaving rungs out costs nothing here -- an
 * absent grade is an unset bit, not an empty slot.
 *
 * @param[in,out] pool            Pool to initialize; not NULL. Need not be zeroed.
 * @param[in]     sizes           Arena capacities, ascending; not NULL. Each must be a power of
 *                                two in [@ref ARNM_GRADED_MIN_SIZE, @ref ARNM_GRADED_MAX_SIZE].
 *                                Read here and not kept -- the array is the caller's again when
 *                                this returns.
 * @param[in]     grade_count     Entries in @p sizes; must be > 0 and at most 29, there being
 *                                no more powers of two in the range.
 * @param[in]     spare_per_grade Arenas each grade keeps in stock, as
 *                                arnm_dynamic_arena_pool_init() takes it. The same figure for
 *                                every grade; a grade that should hold more is reached through
 *                                arnm_graded_arena_pool_grade_at().
 * @retval ARNM_SUCCESS             Every grade is open and empty.
 * @retval ARNM_ERROR_NULL_POINTER  @p pool or @p sizes is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p grade_count is 0 or above 29, a size is not a power of
 *                                     two, a size is outside the range above, or the sizes do
 *                                     not ascend strictly.
 * @retval ARNM_ERROR_OUT_OF_MEMORY The host had no room for the grades themselves.
 * @warning Calling this on a pool that already holds grades leaks them. Use
 *          arnm_graded_arena_pool_release() first.
 * @whisper The rungs are set, from the lowest to the last
 */
arnm_result arnm_graded_arena_pool_init(
    arnm_graded_arena_pool *pool,
    const uint32_t *sizes,
    uint16_t grade_count,
    uint32_t spare_per_grade
);

/**
 * @brief Allocate a pool descriptor and initialize it.
 *
 * One allocator, and only for the descriptor. The grades and their arenas are the host's
 * business and no argument here changes that.
 *
 * @param[in]     sizes           As in arnm_graded_arena_pool_init().
 * @param[in]     grade_count     As in arnm_graded_arena_pool_init().
 * @param[in]     spare_per_grade As in arnm_graded_arena_pool_init().
 * @param[in,out] allocator       Allocator to take this descriptor from, or NULL for malloc.
 * @return Initialized pool, or NULL when @p allocator had no room or
 *         arnm_graded_arena_pool_init() refused the arguments.
 * @note Pair with arnm_graded_arena_pool_destroy() **and hand it the same @p allocator**.
 * @whisper A vessel for the ladder, drawn from whichever stream the host points to
 */
arnm_graded_arena_pool *arnm_graded_arena_pool_create(
    const uint32_t *sizes, uint16_t grade_count, uint32_t spare_per_grade, arnm *allocator
);

/**
 * @brief Hand out an arena that holds at least @p size bytes.
 *
 * The smallest grade whose capacity reaches @p size answers, from its stock or by making an
 * arena. Which grade that is costs the same to work out at every ladder height: @p size is
 * rounded up to a power of two, and the grade is read out of @c grade_bits with a bit scan and
 * a bit count. Nothing is searched.
 *
 * The arena arrives empty and is an ordinary @ref arnm; what it actually holds is its grade's
 * capacity, which is at least @p size and, the rungs being powers of two, can be nearly twice
 * it -- arnm_graded_arena_pool_capacity_for() says how much before anything is asked for.
 *
 * @param[in,out] pool Pool to draw from; not NULL.
 * @param[in]     size Bytes the caller needs the arena to hold; must be > 0.
 * @param[out]    out  Receives the arena; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS                     An arena was handed over.
 * @retval ARNM_ERROR_NULL_POINTER          @p pool or @p out is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED       @p pool holds no grades.
 * @retval ARNM_ERROR_INVALID_PARAM         @p size is 0.
 * @retval ARNM_ERROR_RESOURCE_SIZE_EXCEED  @p size is past the largest grade. The ladder ends
 *                                             where init left it, so returning arenas would not
 *                                             help; ask for less, or name a larger size at init.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED    That grade already has `UINT32_MAX` arenas out.
 * @retval ARNM_ERROR_OUT_OF_MEMORY         The grade's stock was empty and the host had no room.
 * @note Never arnm_release() an arena from here; arnm_graded_arena_pool_free() is how it
 *       comes back.
 * @whisper The request climbs to the rung that can carry it
 */
arnm_result arnm_graded_arena_pool_alloc(arnm_graded_arena_pool *pool, uint32_t size, arnm **out);

/**
 * @brief Give an arena back to the grade it came from, reset on the way in.
 *
 * Which grade that is comes from the arena's own capacity, so nothing has to be remembered
 * between the two calls -- and a capacity that is a power of two names its grade directly,
 * without the search the way out already did without. Whether the arena is then kept in stock
 * or returned to the host is the grade's decision; see arnm_dynamic_arena_pool_free().
 *
 * @param[in,out] pool  Pool the arena came from; not NULL.
 * @param[in,out] arena Arena from arnm_graded_arena_pool_alloc(); not NULL. May be freed
 *                      outright -- do not touch it afterwards.
 * @retval ARNM_SUCCESS               Back in stock, or given to the host.
 * @retval ARNM_ERROR_NULL_POINTER    @p pool or @p arena is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p pool holds no grades.
 * @retval ARNM_ERROR_INVALID_PARAM   @p arena is no single arena, or its capacity is not a
 *                                       power of two, or it is one this ladder has no grade
 *                                       for. The one stranger this pool can recognise, and it
 *                                       recognises it by size alone.
 * @retval ARNM_ERROR_INVALID_STATE   That grade has nothing out, so this arena cannot be
 *                                       coming back.
 * @warning An arena of exactly a grade's capacity that came from somewhere else is taken in as
 *          that grade's own. Return each arena to the pool it came from, exactly once.
 * @whisper The vessel finds its rung again by its own measure
 */
arnm_result arnm_graded_arena_pool_free(arnm_graded_arena_pool *pool, arnm *arena);

/**
 * @brief Release every grade and the ladder itself, once all lent arenas have returned.
 *
 * Leaves the pool in the empty state it had before init, keeping the descriptor. Every grade is
 * asked first and none is touched until all of them agree: a ladder half taken down while a
 * caller still holds an arena would be neither released nor usable.
 *
 * @param[in,out] pool Pool to empty; not NULL.
 * @retval ARNM_SUCCESS               Every grade released, pool empty.
 * @retval ARNM_ERROR_NULL_POINTER    @p pool is NULL.
 * @retval ARNM_ERROR_RESOURCE_IN_USE At least one arena is still out, in any grade. Nothing was
 *                                       changed; return them and call again.
 * @whisper The ladder comes down, but not while anyone still stands on it
 */
arnm_result arnm_graded_arena_pool_release(arnm_graded_arena_pool *pool);

/**
 * @brief arnm_graded_arena_pool_release(), then give the descriptor itself back.
 *
 * @param[in,out] pool      From arnm_graded_arena_pool_create(), never stack or static storage;
 *                          may be NULL.
 * @param[in,out] allocator The allocator @p pool itself came from -- the same one
 *                          arnm_graded_arena_pool_create() was handed, NULL for malloc.
 * @retval ARNM_SUCCESS               Released, or @p pool was NULL and there was nothing to do.
 * @retval ARNM_ERROR_RESOURCE_IN_USE An arena is still out. **Nothing was released and the
 *                                       descriptor is still yours.**
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED An arena kept the descriptor. It is gone from
 *                                       the caller's side regardless.
 * @whisper The vessel that held the ladder returns to the stream it came from
 */
arnm_result arnm_graded_arena_pool_destroy(arnm_graded_arena_pool *pool, arnm *allocator);

/**
 * @brief What an arena for @p size would actually hold, without asking for one.
 *
 * The capacity of the grade that would answer, so a caller weighing the slack -- or looking for
 * somewhere else to put a large block -- can find out before being refused.
 *
 * @param[in] pool Pool to ask; may be NULL.
 * @param[in] size Bytes the caller is considering.
 * @return The grade's capacity, always >= @p size and always a power of two, or 0 when @p pool
 *         is NULL or empty, @p size is 0, or @p size is past the largest grade.
 * @note Worth asking where the slack matters. A ladder of powers of two answers 4096 to a
 *       request for 2056, and the difference is memory a stocked grade holds for good.
 * @whisper The height of the rung, asked from the ground
 */
uint32_t arnm_graded_arena_pool_capacity_for(const arnm_graded_arena_pool *pool, uint32_t size);

/**
 * @brief One grade of the ladder, to fill up front or to read counts from.
 *
 * The grades are ordinary @ref arnm_dynamic_arena_pool objects and every call in that module
 * works on them. Filling the stock before the first request is what this is mainly for:
 * `arnm_dynamic_arena_pool_reserve(arnm_graded_arena_pool_grade_at(pool, 0), 4)`.
 *
 * @param[in] pool  Pool to ask; may be NULL.
 * @param[in] index Grade to reach, 0 being the smallest capacity.
 * @return The grade, or NULL when @p pool is NULL or empty, or @p index names no grade.
 * @warning Releasing a grade through this pointer leaves the ladder intact and that grade
 *          empty -- usable, but no longer holding what it held. Lending and returning are
 *          what this is for.
 */
arnm_dynamic_arena_pool *arnm_graded_arena_pool_grade_at(
    const arnm_graded_arena_pool *pool, uint16_t index
);

/**
 * @brief Grades in the ladder.
 *
 * @param[in] pool Pool to query; may be NULL.
 * @return The number of grades, or 0 if @p pool is NULL or empty.
 */
static inline uint16_t arnm_graded_arena_pool_grade_count(const arnm_graded_arena_pool *pool) {
  return (pool && pool->grades) ? pool->grade_count : (uint16_t)0;
}

/**
 * @brief Bytes this pool holds from the host right now, every grade and the ladder together.
 *
 * The sum of what the grades carry at this moment plus the array they live in -- what the host
 * is holding on this pool's behalf, which is the number a budget is read against.
 *
 * @param[in] pool Pool to measure; may be NULL.
 * @return Bytes currently held, or 0 if @p pool is NULL or holds nothing.
 * @note `uint64_t`, for the reason given at arnm_dynamic_arena_pool_reserved(): the sizes
 *       handed to the host stay `uint32_t`, only their sum needs the wider type.
 * @whisper What the whole ladder weighs, this moment and not the next
 */
uint64_t arnm_graded_arena_pool_reserved(const arnm_graded_arena_pool *pool);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_GRADED_ARENA_POOL_H
