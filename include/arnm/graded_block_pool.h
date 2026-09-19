#ifndef ARNM_GRADED_BLOCK_POOL_H
#define ARNM_GRADED_BLOCK_POOL_H

#include <stdint.h>
#include <string.h>

#include "arnm/bytes.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_graded_block_pool arnm_graded_block_pool
 * @brief Blocks in power of two grades, kept on a free list per grade and handed out again.
 *
 * An arena takes back only its newest allocation. A container that grows a step at a time -- a
 * set whose array doubles and then turns into a bitmap, a directory that doubles with it --
 * leaves every block it outgrew behind, and behind an arena those blocks stay until the reset.
 * A pool catches them instead: a freed block goes onto the free list of its grade, and the next
 * request of that grade gets it back.
 *
 * The pool is not an @ref arnm and does not go through @ref arnm_alloc(). It has its own calls,
 * named after the ones in @ref arnm_memory, and a container that wants it takes an
 * `arnm_graded_block_pool *`. Keeping it out of the handle keeps its branch out of every other
 * allocator's path.
 *
 * ### Grades
 *
 * Every power of two from @ref arnm_graded_block_pool_options::min_block_log2 to
 * @ref arnm_graded_block_pool_options::max_block_log2 is a grade. A request is rounded up to 8
 * and then to the next grade: 20 bytes take a 32 byte block, 100 bytes a 128 byte one, and
 * anything up to the smallest grade takes a block of the smallest. The slack is less than the
 * request itself, and it is what buys the reuse -- a block fits every later request of its
 * grade, whatever size that request has.
 *
 * A request past the largest grade is refused with @ref ARNM_ERROR_RESOURCE_SIZE_EXCEED. Name a
 * larger grade instead: one that is never asked for costs nothing, because blocks only come
 * into being on request.
 *
 * @ref arnm_graded_block_pool_free() and @ref arnm_graded_block_pool_realloc() work the grade
 * out again from the size they are told, the way every allocator in arnm does. A container that
 * already knows its block as a power of two says so instead, with
 * @c arnm_graded_block_pool_alloc_log2() and @c arnm_graded_block_pool_free_log2(): inline,
 * no rounding, no checks beyond the grade bounds and the lent counter. Nothing is
 * stored next to a block, and a free block keeps the link to the next one in its own first 8
 * bytes, which is why no grade is smaller than 8 bytes.
 *
 * ### Where the blocks live
 *
 * In a chain of arenas the pool opens for itself (see @ref arnm_multi_arena) and owns. Each of
 * its arenas holds @ref arnm_graded_block_pool_options::alloc_arena_capacity blocks of the
 * largest grade, 4 MiB with the defaults, and comes from the host when the chain needs another.
 *
 * The pool takes its arenas from the chain whole, one at a time, and cuts blocks off the
 * current one at its cursor. A request the current arena can no longer hold cuts what is left
 * of it into blocks of the grades that fit -- largest first, so at most one per grade -- puts
 * them on their free lists and moves on to a fresh arena. Nothing is left behind in an arena,
 * and the chain is asked once per arena, not once per block.
 *
 * The allocator passed as `source` gives only the bookkeeping: the chain's descriptor (88 bytes
 * on a 64 bit target), the chain's list of arenas (one 32 byte handle per arena, in buckets),
 * and with @ref arnm_graded_block_pool_create() the pool struct itself (296 bytes). It grows
 * with the number of arenas, not with the number of blocks.
 *
 * A block handed back never leaves the pool: it waits on its free list for the next request of
 * its grade. The memory goes back as a whole, on @ref arnm_graded_block_pool_release().
 *
 * ### Lifetime
 *
 * | call | the blocks | the chain's arenas | the pool afterwards |
 * |------|------------|--------------------|---------------------|
 * | @ref arnm_graded_block_pool_reset() | all gone, lent ones included | kept, emptied | usable |
 * | @ref arnm_graded_block_pool_release() | all gone | back to the host | needs init again |
 * | @ref arnm_graded_block_pool_destroy() | all gone | back to the host | gone |
 *
 * Every one of them ends every block the pool ever handed out, the way an arena's reset does.
 * A pool made with @ref arnm_graded_block_pool_init() is done with after
 * @ref arnm_graded_block_pool_release(): nothing is left behind, and the struct may go out of
 * scope.
 *
 * @note A freed pointer is trusted to be one this pool handed out with the size it is freed
 *       with. A foreign pointer or a wrong size goes onto a free list and is handed to the next
 *       caller of that grade; the pool cannot tell. A free that would take the lent counter
 *       below zero is refused, which catches the plainest double free and no more.
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

/** @brief Most grades a pool can have: every power of two from 2^3 to 2^31. */
#define ARNM_GRADED_BLOCK_POOL_MAX_GRADES                                                          \
  (ARNM_GRADED_BLOCK_POOL_MAX_LOG2 - ARNM_GRADED_BLOCK_POOL_MIN_LOG2 + 1u)

/** @brief Smallest grade when the caller names none: 2^4, 16 bytes. */
#define ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2 4u

/** @brief Largest grade when the caller names none: 2^20, 1 MiB. */
#define ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2 20u

/** @brief Blocks of the largest grade one arena of the chain holds when the caller names none. */
#define ARNM_GRADED_BLOCK_POOL_DEFAULT_ALLOC_ARENA_CAPACITY 4u

/**
 * @brief How a pool is shaped. Every field takes its default from 0, so `{0}` is a valid set.
 *
 * Pass the same struct to @ref arnm_graded_block_pool_options_validate() to find out *why* a set
 * was refused; @ref arnm_graded_block_pool_create() only answers whether it was.
 */
typedef struct arnm_graded_block_pool_options {
  /** Smallest grade as a power of two, 3 to 31. 0 means
   *  @ref ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2. A request below it takes a block this size. */
  uint8_t min_block_log2;
  /** Largest grade as a power of two, from @c min_block_log2 to 31. 0 means
   *  @ref ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2. A request above it is refused. */
  uint8_t max_block_log2;
  /** Size of one arena of the chain, counted in blocks of the largest grade. 0 means
   *  @ref ARNM_GRADED_BLOCK_POOL_DEFAULT_ALLOC_ARENA_CAPACITY. The product is capped at
   *  @ref ARNM_MAX_ALLOC_SIZE. Larger arenas mean fewer trips to the host and more memory
   *  reserved before it is used. */
  uint8_t alloc_arena_capacity;
} arnm_graded_block_pool_options;

/**
 * @brief A graded block pool: its chain, one free list per grade, two counters.
 *
 * Declare one and give it to @ref arnm_graded_block_pool_init(), or let
 * @ref arnm_graded_block_pool_create() take one from an allocator. The fields are public to be
 * read -- @c lent_bytes and @c cached_bytes are the pool's measure -- and are written only by the
 * calls below.
 *
 * @c free_head is indexed by the grade's distance from the smallest, `exponent - min_log2`. It
 * holds room for the most grades any pool can have, so it needs no allocation of its own and a
 * lookup is one load off the pool; the entries past `max_log2 - min_log2` stay unused, 96 bytes
 * with the default grades. It comes after every field an allocation or a free reads or writes
 * besides its one list head, which all sit in the first 32 bytes. A pool that starts on a cache
 * line -- give it 64 byte aligned storage where that matters -- is touched on at most two lines
 * per call from a free list, and on one for the four smallest grades.
 *
 * @c current is the arena blocks are cut from when a list is empty, the path that is left once
 * per arena anyway; it comes last.
 */
typedef struct arnm_graded_block_pool {
  arnm *source; /**< The pool's own chain; every block lives in it. NULL before init and after
                     release. */
  uint64_t lent_bytes;     /**< Block bytes out with callers, counted in whole grades. */
  uint64_t cached_bytes;   /**< Block bytes waiting on the free lists. */
  uint8_t min_log2;        /**< Smallest grade. */
  uint8_t max_log2;        /**< Largest grade. */
  uint32_t arena_capacity; /**< Bytes of one arena, taken from the chain whole. */
  uint8_t *free_head[ARNM_GRADED_BLOCK_POOL_MAX_GRADES]; /**< First free block per grade. */
  arnm current; /**< Arena blocks are cut from, borrowed from the chain; its cursor says how far.
                     Zeroed while there is none. */
} arnm_graded_block_pool;

// ********** manage the pool itself *******************

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
 * @brief Make @p pool a pool. It holds no block and no arena until the first allocation.
 *
 * Opens the pool's chain, whose descriptor comes from @p source. Nothing in @p pool is read, so
 * uninitialized storage is fine; a pool that was still in use is overwritten without being
 * released, and what it held is lost.
 *
 * @param[out]    pool    Pool to set up; not NULL. Untouched on failure.
 * @param[in,out] options Shape of the pool; not NULL. `{0}` gives the defaults. Filled in with
 *                        the effective values, also when the chain could not be opened.
 * @param[in,out] source  Where the chain's descriptor comes from, NULL for the host. Name it
 *                        again to @ref arnm_graded_block_pool_release().
 * @retval ARNM_SUCCESS             Ready.
 * @retval ARNM_ERROR_NULL_POINTER  @p pool or @p options is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p options refused; see
 *                                  @ref arnm_graded_block_pool_options_validate().
 * @retval ARNM_ERROR_OUT_OF_MEMORY @p source had no room for the descriptor.
 * @whisper Basins dug in a row, the smallest first, the stream not yet let in
 */
arnm_result arnm_graded_block_pool_init(
    arnm_graded_block_pool *pool, arnm_graded_block_pool_options *options, arnm *source
);

/**
 * @brief Take a pool struct from @p source and set it up as @ref arnm_graded_block_pool_init()
 *        does, over the same @p source.
 *
 * @param[in,out] options Shape of the pool; not NULL. `{0}` gives the defaults. Filled in with
 *                        the effective values.
 * @param[in,out] source  Where the pool struct and the chain's descriptor come from, NULL for
 *                        the host.
 * @return The pool, or NULL if @p options was refused or @p source had no room. Use
 *         @ref arnm_graded_block_pool_options_validate() to tell those apart.
 * @note Give it back with @ref arnm_graded_block_pool_destroy(), naming the same @p source.
 */
arnm_graded_block_pool *arnm_graded_block_pool_create(
    arnm_graded_block_pool_options *options, arnm *source
);

/**
 * @brief Forget every block and keep the arenas for the next round.
 *
 * Resets the chain and empties the free lists and both counters. The arenas stay with the pool,
 * emptied, so the next round of work reuses memory that is already there.
 *
 * @param[in,out] pool Pool to empty; NULL is a no-op, and so is a released one.
 * @warning Every block the pool ever handed out is dangling afterwards, the ones still lent out
 *          included -- the same as an arena's reset.
 * @whisper The basins are drained, not filled in
 */
void arnm_graded_block_pool_reset(arnm_graded_block_pool *pool);

/**
 * @brief Give everything back: the arenas to the host, the chain's descriptor to @p source.
 *
 * The counterpart of @ref arnm_graded_block_pool_init(). Afterwards the pool holds nothing and
 * answers @ref ARNM_ERROR_NOT_INITIALIZED; leave it, or initialize it again.
 *
 * @param[in,out] pool   Pool to release; NULL is a no-op, and so is a released one.
 * @param[in,out] source The allocator named to @ref arnm_graded_block_pool_init(), NULL for the
 *                       host. Behind an arena the descriptor's bytes come back only if they sit
 *                       at its tail; either way the pool has let go of them.
 * @warning Every block the pool ever handed out is dangling afterwards, the ones still lent out
 *          included.
 * @whisper The basins are filled in, the ground given back
 */
void arnm_graded_block_pool_release(arnm_graded_block_pool *pool, arnm *source);

/**
 * @brief @ref arnm_graded_block_pool_release(), then the pool struct itself.
 *
 * @param[in,out] pool   From @ref arnm_graded_block_pool_create(), never stack or static
 *                       storage; NULL is a no-op.
 * @param[in,out] source The allocator named to @ref arnm_graded_block_pool_create().
 * @retval ARNM_SUCCESS Everything given back, or @p pool was NULL.
 * @return Otherwise the first answer that was not a success: the chain's descriptor first, then
 *         the pool struct. Behind an arena that is @ref ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
 *         when the descriptor was not at its tail; the pool struct then stays where it is too,
 *         and the arena's reset takes both.
 * @warning Every block the pool ever handed out is dangling afterwards.
 */
arnm_result arnm_graded_block_pool_destroy(arnm_graded_block_pool *pool, arnm *source);

// ********** blocks *******************

/**
 * @brief Hand out a block of @p size's grade: from its free list, or new from the chain.
 *
 * @param[in,out] pool   Pool to take from; not NULL.
 * @param[out]    buffer Receives the block; not NULL. Untouched on failure.
 * @param[in]     size   Bytes wanted, 1 up to the largest grade. The block is the whole grade,
 *                       8 byte aligned. Its contents are undefined -- a reused block still holds
 *                       what its last owner and the free list left in it.
 * @retval ARNM_SUCCESS                   @p *buffer is the block.
 * @retval ARNM_ERROR_NULL_POINTER        @p pool or @p buffer is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p pool is zeroed or released.
 * @retval ARNM_ERROR_INVALID_PARAM       @p size is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p size is past @ref ARNM_MAX_ALLOC_SIZE.
 * @retval ARNM_ERROR_RESOURCE_SIZE_EXCEED @p size is past the largest grade.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The list was empty and no new arena could be opened:
 *                                        the host had none, or the source no room to list it.
 * @whisper Water drawn from the basin that fits the cup
 */
arnm_result arnm_graded_block_pool_alloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t size
);

/**
 * @brief Move @p *buffer into a block for @p new_size, carrying its contents along.
 *
 * Always moves, even when @p old_size and @p new_size fall into the same grade and the block
 * would already do. A pool serves containers that grow a grade at a time, so a resize within one
 * grade is expected to be rare or absent; checking for it would cost every resize a second
 * classification, and the hot path does not pay for a case it does not have. A caller that does
 * resize within a grade pays a copy it could have avoided -- keep the capacity yourself and
 * resize only when it is outgrown.
 *
 * The new block is taken first, then the smaller of both sizes is copied, then the old block
 * goes onto its free list. A NULL @p *buffer or an @p old_size of 0 skips the copy and the free,
 * which makes the call an allocation. A @p new_size of 0 is refused, not a free.
 *
 * @param[in,out] pool     Pool the block came from; not NULL.
 * @param[in,out] buffer   Block to move, replaced by the new one; not NULL, @p *buffer may be.
 * @param[in]     old_size Size the block was taken with.
 * @param[in]     new_size Size wanted; 1 up to the largest grade.
 * @retval ARNM_SUCCESS Moved, @p *buffer is the new block.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Moved, but the pool refused the old block
 *         (a size it cannot place, or more than it has lent out) and did not take it back.
 * @return Any refusal of @ref arnm_graded_block_pool_alloc() for @p new_size, with
 *         @p *buffer untouched.
 * @whisper The water is poured into the next basin, never swirled in the old one
 */
arnm_result arnm_graded_block_pool_realloc(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint32_t old_size, uint32_t new_size
);

/**
 * @brief Put a block back on the free list of its grade.
 *
 * The block stays with the pool and serves the next request of its grade; neither the chain nor
 * the host sees it again before @ref arnm_graded_block_pool_release().
 *
 * @param[in,out] pool   Pool the block came from; may be NULL when @p buffer is.
 * @param[in]     buffer Block to give back; NULL is a no-op, as for free().
 * @param[in]     size   Size the block was taken with -- any size of the same grade will do.
 * @retval ARNM_SUCCESS                    On the list, or @p buffer was NULL.
 * @retval ARNM_ERROR_NULL_POINTER         @p pool is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED      @p pool is zeroed or released.
 * @retval ARNM_ERROR_INVALID_PARAM        @p size is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW  @p size is past @ref ARNM_MAX_ALLOC_SIZE.
 * @retval ARNM_ERROR_RESOURCE_SIZE_EXCEED @p size is past the largest grade.
 * @retval ARNM_ERROR_INVALID_STATE        Fewer bytes are lent out than the grade has; the
 *                                         block cannot be one of this pool's.
 * @whisper The cup is emptied back into its own basin
 */
arnm_result arnm_graded_block_pool_free(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint32_t size
);

// ********** blocks by exponent, the hot path *******************

/**
 * @brief The part of @c arnm_graded_block_pool_alloc_log2() behind an empty free list: a block
 *        cut from the current arena, or from a fresh one.
 *
 * Called by the inline path; call that instead. @p log2 is already inside the pool's grades.
 *
 * @retval ARNM_SUCCESS               @p *buffer is the block.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p pool is zeroed or released.
 * @retval ARNM_ERROR_OUT_OF_MEMORY   The host had no new arena, or the source no room to list it.
 */
arnm_result arnm_graded_block_pool_alloc_fresh(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint8_t log2
);

/**
 * @brief Hand out a block of 2^@p log2 bytes: @ref arnm_graded_block_pool_alloc() for a caller
 *        that already holds its sizes as powers of two.
 *
 * Inline, and the free list path is a handful of instructions: no rounding, no size to classify,
 * no NULL checks. An exponent below the smallest grade takes a block of the smallest; free it
 * with the same exponent and it finds the same list.
 *
 * @param[in,out] pool   Pool to take from; not NULL.
 * @param[out]    buffer Receives the block; not NULL. Untouched on failure.
 * @param[in]     log2   Size of the block as a power of two.
 * @retval ARNM_SUCCESS                    @p *buffer is the block.
 * @retval ARNM_ERROR_RESOURCE_SIZE_EXCEED @p log2 is past the largest grade.
 * @return Any refusal of @ref arnm_graded_block_pool_alloc_fresh() when the list was empty.
 * @whisper The cup is filled from the basin whose number it already knows
 */
static inline arnm_result arnm_graded_block_pool_alloc_log2(
    arnm_graded_block_pool *pool, uint8_t **buffer, uint8_t log2
) {
  if (log2 > pool->max_log2) { return ARNM_ERROR_RESOURCE_SIZE_EXCEED; }
  if (log2 < pool->min_log2) { log2 = pool->min_log2; }
  uint8_t **head = &pool->free_head[log2 - pool->min_log2];
  uint8_t *block = *head;
  if (!block) { return arnm_graded_block_pool_alloc_fresh(pool, buffer, log2); }
  *head = arnm_load_ptr(block);
  const uint64_t bytes = (uint64_t)1u << log2;
  pool->cached_bytes -= bytes;
  pool->lent_bytes += bytes;
  *buffer = block;
  return ARNM_SUCCESS;
}

/**
 * @brief Put a block of 2^@p log2 bytes back on its list: @ref arnm_graded_block_pool_free() by
 *        exponent.
 *
 * @param[in,out] pool   Pool the block came from; not NULL.
 * @param[in]     buffer Block to give back; NULL is a no-op.
 * @param[in]     log2   The exponent the block was taken with.
 * @retval ARNM_SUCCESS                    On the list, or @p buffer was NULL.
 * @retval ARNM_ERROR_RESOURCE_SIZE_EXCEED @p log2 is past the largest grade.
 * @retval ARNM_ERROR_INVALID_STATE        Fewer bytes are lent out than the grade has -- also the
 *                                         answer of a released pool, which has none out.
 * @whisper The cup goes back to the basin whose number it carries
 */
static inline arnm_result arnm_graded_block_pool_free_log2(
    arnm_graded_block_pool *pool, uint8_t *buffer, uint8_t log2
) {
  if (!buffer) { return ARNM_SUCCESS; }
  if (log2 > pool->max_log2) { return ARNM_ERROR_RESOURCE_SIZE_EXCEED; }
  if (log2 < pool->min_log2) { log2 = pool->min_log2; }
  const uint64_t bytes = (uint64_t)1u << log2;
  // the plainest double free: more coming back than is out
  if (pool->lent_bytes < bytes) { return ARNM_ERROR_INVALID_STATE; }
  uint8_t **head = &pool->free_head[log2 - pool->min_log2];
  memcpy(buffer, head, sizeof(uint8_t *));
  *head = buffer;
  pool->lent_bytes -= bytes;
  pool->cached_bytes += bytes;
  return ARNM_SUCCESS;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_GRADED_BLOCK_POOL_H
