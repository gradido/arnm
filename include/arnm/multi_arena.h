#ifndef ARNM_MULTI_ARENA_H
#define ARNM_MULTI_ARENA_H

#include <stdint.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_multi_arena arnm_multi_arena
 * @brief A chain of arenas that opens more ground instead of saying no.
 *
 * The allocator to reach for when the peak is *not* known up front -- which is the usual case,
 * and the reason to pick this over a single @ref arnm_arena. A request is served from the first
 * arena in the chain that still has room; when none does, the chain opens another and serves it
 * from there. It keeps an arena's speed and drops its ceiling.
 *
 * Once created, a chain is used through @ref arnm_memory like any other handle:
 * @ref arnm_alloc(), @ref arnm_free() and friends all take it. This header covers bringing one
 * into being and asking it what it currently holds.
 *
 * ### The scan, and the threshold that bounds it
 *
 * The chain remembers the earliest arena that may still have room and starts every search
 * there. An arena leaves that search for good once its remainder falls to the **full
 * threshold** -- @ref arnm_multi_arena_options::full_remaining. Those last bytes are written
 * off on purpose: without a threshold the search would keep visiting arenas that can only ever
 * serve the smallest requests, and the scan would grow with the chain.
 *
 * So the threshold is the knob between memory and speed. Small: little is given up, the scan
 * gets longer. Large: the scan stays short, more tails go unused. The default (64 bytes) suits
 * chains of small structs and short strings; name your own once the request sizes are known.
 *
 * An arena whose remainder is *above* the threshold but too small for the request at hand is
 * neither served from nor written off -- it is walked over, on this request and the next. That
 * is the cost the threshold is there to bound.
 *
 * ### Requests larger than an arena
 *
 * Not refused: a request bigger than `arena_capacity` gets an arena sized exactly for it, full
 * the moment it is handed over and out of the way of everything after it.
 *
 * ### Giving memory back
 *
 * @ref arnm_free() finds the arena a block came from and takes it back if it sits at that
 * arena's tail; an arena that has room again rejoins the search. Anything buried answers
 * @ref ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED, and an address from outside the chain is
 * refused outright with @ref ARNM_ERROR_INVALID_PARAM. @ref arnm_reset() empties every arena
 * and keeps them; @ref arnm_multi_arena_shrink() hands the empty trailing ones back to the
 * host; @ref arnm_release() gives up everything.
 *
 * @{
 */

/** @brief Bytes a regular arena reserves when the caller names no capacity -- 1 KiB. */
#define ARNM_MULTI_ARENA_DEFAULT_CAPACITY ((uint32_t)1 << 10)

/**
 * @brief Full threshold a chain uses when the caller names none -- 64 bytes.
 *
 * An arena leaves the scan once 64 or fewer bytes are left in it, a remainder always being a
 * multiple of 8. That fits a chain of small structs and short strings and gives up little of
 * what it writes off. Name a threshold at init once the request sizes are known.
 */
#define ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING ((uint32_t)64)

/** @brief Arena descriptors per bucket of the descriptor vector -- 2^6 = 64. */
#define ARNM_MULTI_ARENA_DEFAULT_BUCKET_LOG2 6

/**
 * @brief How a chain is shaped. Every field takes its default from 0, so `{0}` is a valid set.
 *
 * Pass the same struct to @ref arnm_multi_arena_options_validate() to find out *why* a set was
 * refused; @ref arnm_create_multi_arena() only answers whether it was.
 */
typedef struct arnm_multi_arena_options {
  /** Bytes a regular arena reserves, rounded up to 8. 0 means
   *  @ref ARNM_MULTI_ARENA_DEFAULT_CAPACITY. Larger arenas mean fewer of them and a shorter
   *  scan; smaller ones waste less when the chain is barely used. */
  uint32_t arena_capacity;
  /** Ceiling on the number of arenas, or 0 for as many as the host will give. A chain at its
   *  cap answers @ref ARNM_ERROR_RESOURCE_EXHAUSTED instead of opening another -- which turns
   *  the chain into a bounded budget with a known peak. */
  uint32_t arena_max_count;
  /** Remainder at or below which an arena leaves the scan for good; see the notes above. 0
   *  means @ref ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING, and it must stay below
   *  @p arena_capacity. */
  uint32_t full_remaining;
  /** Arena descriptors per bucket of the internal descriptor vector, as a power of two. 0
   *  means @ref ARNM_MULTI_ARENA_DEFAULT_BUCKET_LOG2, maximum 15. Bookkeeping only; it costs
   *  nothing to leave alone. */
  uint8_t bucket_size_log2;
  /** Bucket slots that vector's index array grows by at a time. 0 means the library default.
   *  Bookkeeping only. */
  uint8_t index_grow_step_size;
} arnm_multi_arena_options;

// ********** manage the allocator itself *******************

/**
 * @brief Check a set of options and fill its 0 fields in with the defaults.
 *
 * Answers the question @ref arnm_create_multi_arena() cannot: which field was wrong. @p options
 * is written back with the effective values, so it also shows what a chain built from it would
 * actually use.
 *
 * @param[in,out] options Options to check; not NULL. Updated in place.
 * @retval ARNM_SUCCESS                   Usable, and @p options now holds the effective values.
 * @retval ARNM_ERROR_NULL_POINTER        @p options is NULL.
 * @retval ARNM_ERROR_INVALID_STATE       @p full_remaining reaches @p arena_capacity, which
 *                                        would write every arena off before it served anything.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p arena_capacity exceeds @ref ARNM_MAX_ALLOC_SIZE, or
 *                                        @p bucket_size_log2 is above 15.
 */
arnm_result arnm_multi_arena_options_validate(arnm_multi_arena_options *options);

/**
 * @brief Build a chain. It holds no memory until the first allocation.
 *
 * @param[in,out] options   How to shape it; not NULL. `{0}` gives the defaults throughout.
 *                          Filled in with the effective values on return.
 * @param[in,out] allocator Where the chain's own handle and its descriptor vector come from, or
 *                          NULL for the host.
 * @return The chain, or NULL if @p options was refused or @p allocator had no room. Use
 *         @ref arnm_multi_arena_options_validate() to tell those apart.
 * @note @p allocator carries the bookkeeping only. The arenas themselves always come from the
 *       host -- a chain that drew them from a fixed allocator could not outgrow it, which is
 *       the one thing it exists to do.
 * @note Give it back with @ref arnm_destroy(), naming the same @p allocator.
 * @whisper Not one field, but as many as the work turns out to need
 */
arnm *arnm_create_multi_arena(arnm_multi_arena_options *options, arnm *allocator);

/**
 * @brief Is @p memory a chain?
 *
 * @param[in] memory Handle to ask; NULL answers false.
 * @return true only for a chain. @ref arnm_is_arena() is the broader question and answers true
 *         for chains as well. A true answer implies `memory != NULL`.
 */
bool arnm_is_multi_arena(const arnm *memory);

/** @brief What a chain currently holds, gathered in one pass. */
typedef struct arnm_multi_arena_stats {
  uint64_t reserved;    /**< Bytes held from the host: the capacities of all arenas. */
  uint64_t used;        /**< Bytes handed out, rounded up to 8 the way the arenas count them. */
  uint32_t arena_count; /**< Arenas in the chain. */
  uint32_t open_count;  /**< Arenas still in the scan, i.e. holding more than the threshold. */
} arnm_multi_arena_stats;

/**
 * @brief Take the descriptor slots for @p arena_count arenas ahead of time.
 *
 * Bookkeeping only -- no arena is opened and no payload memory is touched. Worth doing when the
 * chain sits on an arena for its bookkeeping: the descriptor vector regrows in steps, an arena
 * cannot take a superseded index array back, and reserving once avoids stranding every earlier
 * one.
 *
 * @param[in,out] m           Chain; not NULL.
 * @param[in]     arena_count Arenas to make room for.
 * @retval ARNM_SUCCESS                  Room is there.
 * @retval ARNM_ERROR_NULL_POINTER       @p m is NULL.
 * @retval ARNM_ERROR_INVALID_STATE      @p m is not a chain.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED @p arena_count is past the chain's `arena_max_count`.
 * @retval ARNM_ERROR_OUT_OF_MEMORY      The bookkeeping allocator had no room.
 */
arnm_result arnm_multi_arena_reserve(arnm *m, uint32_t arena_count);

/**
 * @brief Add a block the caller owns to the chain, as a borrowed arena.
 *
 * A way to give a chain ground it did not have to ask the host for -- static storage, a stack
 * buffer, a slice of something bigger. It joins the end of the chain and is used like any other
 * arena, but is never freed: not by @ref arnm_release(), not by
 * @ref arnm_multi_arena_shrink(), which stops when it reaches one.
 *
 * @param[in,out] m        Chain; not NULL.
 * @param[in]     data     The block; not NULL, 8 byte aligned.
 * @param[in]     capacity Bytes in @p data; > 0 and a multiple of 8.
 * @retval ARNM_SUCCESS                  Appended; the block outlives the chain.
 * @retval ARNM_ERROR_NULL_POINTER       @p m or @p data is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM      @p data or @p capacity is not 8 byte aligned, or
 *                                       @p capacity is 0. Nothing is appended.
 * @retval ARNM_ERROR_INVALID_STATE      @p m is not a chain.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED The chain is at its `arena_max_count`.
 * @whisper Ground lent, and returned unturned
 */
arnm_result arnm_multi_arena_borrow(arnm *m, uint8_t *data, uint32_t capacity);

/**
 * @brief Hand the empty arenas at the end of the chain back to the host.
 *
 * Walks from the youngest arena backwards and stops at the first that still holds something or
 * that the chain does not own. Arenas in the middle are left alone -- removing them would
 * renumber the chain for nothing gained. So this is what to call after an
 * @ref arnm_reset(), when a peak has passed and the ground taken to reach it is no longer
 * wanted.
 *
 * @param[in,out] m Chain; not NULL.
 * @retval ARNM_SUCCESS             Whatever could be released was.
 * @retval ARNM_ERROR_NULL_POINTER  @p m is NULL.
 * @retval ARNM_ERROR_INVALID_STATE @p m is not a chain.
 * @whisper What the tide left behind goes back to the sea
 */
arnm_result arnm_multi_arena_shrink(arnm *m);

/**
 * @brief Arenas currently in the chain.
 *
 * @param[in] m Chain to ask.
 * @return The count, or 0 for NULL and for anything that is not a chain.
 */
uint32_t arnm_multi_arena_arena_count(const arnm *m);

/**
 * @brief Read what the chain holds, in one walk over its arenas.
 *
 * The figure to size a chain by: `reserved` against `used` says how much of what was taken from
 * the host is actually carrying anything, and `open_count` against `arena_count` says how much
 * of the chain the scan still has to walk. O(arena_count) -- a measurement, not a counter to
 * poll in a hot loop.
 *
 * @param[in]  m   Chain; not NULL.
 * @param[out] out Receives the figures; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS             Filled in.
 * @retval ARNM_ERROR_NULL_POINTER  @p m or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_STATE @p m is not a chain.
 * @whisper A count of the ground held, and of the ground still open
 */
arnm_result arnm_multi_arena_measure(const arnm *m, arnm_multi_arena_stats *out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_MULTI_ARENA_H
