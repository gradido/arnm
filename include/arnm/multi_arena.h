#ifndef HOSTMEM_MULTI_ARENA_H
#define HOSTMEM_MULTI_ARENA_H

#include <stdint.h>

#include "hostmem/bucket_vector.h"
#include "hostmem/memory.h"
#include "hostmem/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup hostmem_multi_arena hostmem_multi_arena
 * @brief A chain of arenas that grows instead of running dry.
 *
 * One @ref hostmem arena is a single stretch of memory with a capacity fixed at birth: when it
 * fills, it is full. A multi arena keeps a sequence of them in a @ref hostmem_bucket_vector and
 * opens the next one when the current stretch no longer has room. The bump pointer never stops
 * moving; it simply moves on.
 *
 * Everything a single arena promises still holds inside each stretch -- 8 byte aligned pointers,
 * sizes rounded up to 8, sizes passed in and never stored -- and two properties are added by the
 * chain itself:
 *
 * - **Pointer stability**: an arena, once opened, is never moved or resized, and the descriptors
 *   live in bucket storage that does not move either. Every pointer handed out stays valid until
 *   hostmem_multi_arena_reset(), _shrink() or _release().
 * - **No ceiling but the request**: a request larger than the configured arena capacity opens an
 *   arena sized exactly for it, rather than being refused. The only limit left is the `uint32_t`
 *   a single allocation is measured in.
 *
 * ### Where an allocation lands
 *
 * First fit, scanned from the earliest arena that may still have room. That front marker only
 * walks forward -- past arenas whose remainder has fallen to the chain's full threshold or below
 * -- so a long lived allocator does not re-walk the arenas it filled years of allocations ago.
 * The scan is O(1) amortized, O(open arenas) in the worst case, when many arenas hold a
 * remainder too small for the current request but too large to be called full.
 *
 * ### The full threshold
 *
 * That worst case is the one figure worth tuning, and it is named at init:
 * @p full_remaining says how many bytes an arena may keep and still be written off. Every
 * arena whose remainder falls to it drops out of the scan for good; every arena above it is
 * walked over by each allocation that does not fit there.
 *
 * One question settles it: what is the smallest request this chain should still be able to place
 * in a leftover? An arena serves a request of @c n bytes while its remainder is at least @c n,
 * and is written off while its remainder is at most the threshold, so `n - 8` writes an arena
 * off exactly when it can no longer hold that request -- the size counted after rounding up to 8,
 * the way the arenas count it.
 *
 * A chain fed uniform records therefore has nothing to trade: one alignment step below the
 * record size is both the shortest scan and the smallest waste, because a leftover under one
 * record can never serve anyone. A chain fed a spread of sizes has to choose, and every choice
 * sits between two costs. Near the small end nothing usable is given up, and arenas linger in
 * the scan holding remainders only the smallest requests can use -- roughly half a nanosecond per
 * arena walked, which is nothing until a thousand of them stand in the way. Near the large end
 * the scan stays short, and up to a threshold worth of bytes per arena goes unused until the
 * next reset. @ref hostmem_multi_arena_measure() reports what that came to: `reserved - used` is
 * what the chain holds and does not hand out.
 *
 * ### Memory that comes back
 *
 * | call | what it gives back | what it keeps |
 * |---|---|---|
 * | `_free` | the block, if it is the tail of its own arena | everything else, until a reset |
 * | `_reset` | every allocation at once, O(arena count) | all arenas, ready to be filled again |
 * | `_shrink` | the trailing arenas that hold nothing | the arenas still in use |
 * | `_release` | every arena and the descriptor vector | the descriptor itself, reusable |
 *
 * `_reset` followed by more allocations is the cycle this allocator is built for: the arenas
 * are already there, so a second pass over the same workload asks the host for nothing.
 *
 * ### Empty states
 *
 * Empty after `_init`, `_release` -- and when zero-initialized: `hostmem_multi_arena m = {0};` is
 * valid and usable, with the default arena capacity and malloc/free for the bookkeeping. No
 * arena is opened before the first allocation asks for one. Prefer `_init` when a capacity or a
 * bookkeeping allocator is involved: it is the only way to set them.
 *
 * @note NULL is not an allocator here. @ref hostmem reads a NULL allocator as malloc/free; a
 * NULL multi arena is a mistake and returns HOSTMEM_ERROR_NULL_POINTER.
 *
 * @note A container that expects a `hostmem *` -- @ref hostmem_bucket_vector among them -- cannot
 * be handed a multi arena. Give it one arena, or its own; a multi arena serves raw blocks.
 *
 * @note No hidden state and no locks. Two multi arenas share nothing; one multi arena used from
 * two threads at once is a data race.
 *
 * @whisper Ground runs out, so the path lays down more of it
 *
 * @{
 */

/** @brief Bytes a regular arena reserves when the caller names no capacity -- 1 MiB. */
#define HOSTMEM_MULTI_ARENA_DEFAULT_CAPACITY ((uint32_t)1 << 20)

/**
 * @brief Full threshold a chain uses when the caller names none -- 128 bytes.
 *
 * An arena leaves the scan once fewer than 136 bytes are left in it, a remainder always being a
 * multiple of 8. That fits a chain of small structs and short strings and gives up little of
 * what it writes off. Name a threshold at init once the request sizes are known.
 */
#define HOSTMEM_MULTI_ARENA_DEFAULT_FULL_REMAINING ((uint32_t)128)

/** @brief Arena descriptors per bucket of the descriptor vector. */
#define HOSTMEM_MULTI_ARENA_BUCKET_LOG2 6

/**
 * @brief The descriptor vector: arenas by value, in the order they were opened.
 *
 * Generated here and defined once in `src/multi_arena.c`. Descriptors are stored by value and
 * buckets never move, so a `hostmem *` taken from it stays valid while the chain grows -- which
 * is the whole reason the arenas live in a bucket vector and not in a reallocated array.
 */
HOSTMEM_BVEC_DECLARE(hostmem_arena_vec, hostmem, HOSTMEM_MULTI_ARENA_BUCKET_LOG2, extern)

/**
 * @brief Chained arenas with one bump front.
 *
 * hostmem_multi_arena_init() writes every field and reads none, so uninitialized storage is a
 * valid input. Everything else needs an allocator that is initialized or zeroed.
 *
 * @note Do not write these fields; read them through the API.
 */
typedef struct hostmem_multi_arena {
  hostmem_arena_vec arenas; /**< Every arena, oldest first. Never reordered. */
  uint32_t arena_capacity;  /**< Bytes a regular arena reserves; 0 means the default. */
  uint32_t full_remaining;  /**< Remainder that counts as used up; 0 means the default. */
  uint32_t first_open;      /**< Earliest arena that may still have room; only walks forward. */
} hostmem_multi_arena;

/** @brief What the chain currently holds, gathered in one pass. */
typedef struct hostmem_multi_arena_stats {
  uint64_t reserved;    /**< Bytes held from the host: the capacities of all arenas. */
  uint64_t used;        /**< Bytes handed out, rounded up to 8 the way the arenas count them. */
  uint32_t arena_count; /**< Arenas in the chain. */
  uint32_t open_count;  /**< Arenas holding more than the chain's full threshold. */
} hostmem_multi_arena_stats;

// ********** manage the allocator itself *******************

/**
 * @brief Prepare an empty chain. Opens no arena.
 *
 * Writes every field and reads none, so uninitialized storage is a valid input. The first
 * allocation opens the first arena; a chain that is never used costs nothing but its descriptor.
 *
 * @param[in,out] m              Allocator to initialize; not NULL. Need not be zeroed.
 * @param[in]     arena_capacity Bytes a regular arena reserves, rounded up to 8. 0 selects
 *                               @ref HOSTMEM_MULTI_ARENA_DEFAULT_CAPACITY. A single request
 *                               larger than this still gets an arena of its own.
 * @param[in]     full_remaining Bytes an arena may keep and still be written off, taking it out
 *                               of the first fit scan for good. 0 selects
 *                               @ref HOSTMEM_MULTI_ARENA_DEFAULT_FULL_REMAINING. Set it one
 *                               alignment step below the smallest request this chain should
 *                               still place in a leftover -- see the module text on what moving
 *                               it in either direction costs.
 * @param[in]     bookkeeping    Allocator for the descriptor vector, or NULL for malloc/free.
 *                               It never provides arena storage. An arena the chain opens for
 *                               itself is an owned heap block; one handed to
 *                               hostmem_multi_arena_borrow() stays the caller's throughout.
 * @retval HOSTMEM_SUCCESS                  Chain ready, no arena open.
 * @retval HOSTMEM_ERROR_NULL_POINTER       @p m is NULL.
 * @retval HOSTMEM_ERROR_ARITHMETIC_OVERFLOW Rounding @p arena_capacity up to 8 would wrap.
 * @retval HOSTMEM_ERROR_INVALID_PARAM      @p full_remaining reaches the effective arena
 *                                          capacity, which would write every arena off at birth
 *                                          and give each allocation an arena of its own.
 * @warning Calling this on a chain that already holds arenas leaks them. Use
 *          hostmem_multi_arena_release() first.
 * @note An arena as @p bookkeeping cannot reclaim a superseded index array, so
 *       hostmem_multi_arena_reserve() up front is worth it there.
 * @note A remainder is always a multiple of 8, so only multiples of 8 are distinguishable here:
 *       a threshold of 130 acts exactly like 128. Passing 0 asks for the default rather than for
 *       "write nothing off"; a chain that wants every last byte chased passes 8.
 * @whisper The first basin is not dug until someone asks for water
 */
hostmem_result hostmem_multi_arena_init(
    hostmem_multi_arena *m, uint32_t arena_capacity, uint32_t full_remaining, hostmem *bookkeeping
);

/**
 * @brief Allocate a chain descriptor and initialize it.
 *
 * Two allocators, and they answer different questions. @p bookkeeping feeds the descriptor
 * vector *inside* the chain, growing as arenas are opened; @p allocator hands out the chain
 * descriptor itself, once, here. A host that wants every byte of this library inside storage it
 * owns passes both; passing NULL for either takes malloc for that part alone.
 *
 * @param[in]     arena_capacity As in hostmem_multi_arena_init().
 * @param[in]     full_remaining As in hostmem_multi_arena_init().
 * @param[in,out] bookkeeping    As in hostmem_multi_arena_init(): for the descriptor vector.
 * @param[in,out] allocator      Allocator to take this descriptor from, or NULL for malloc.
 * @return Initialized allocator, or NULL when @p allocator had no room or
 *         hostmem_multi_arena_init() refused the arguments.
 * @note Pair with hostmem_multi_arena_destroy() **and hand it the same @p allocator**. See
 *       hostmem_free() for what a mismatch costs.
 * @whisper A vessel for vessels, drawn from whichever stream the host points to
 */
hostmem_multi_arena *hostmem_multi_arena_create(
    uint32_t arena_capacity, uint32_t full_remaining, hostmem *bookkeeping, hostmem *allocator
);

/**
 * @brief Make room in the descriptor vector for @p arena_count arenas.
 *
 * Touches only the bookkeeping -- no arena is opened and no arena memory is reserved. Worth
 * calling when the bookkeeping allocator is an arena, which cannot take back the index array
 * a later growth supersedes.
 *
 * @param[in,out] m           Allocator to prepare; not NULL.
 * @param[in]     arena_count Descriptor slots to have ready; a smaller count than the current
 *                            one changes nothing.
 * @retval HOSTMEM_SUCCESS              Slots available.
 * @retval HOSTMEM_ERROR_NULL_POINTER   @p m is NULL.
 * @retval HOSTMEM_ERROR_OUT_OF_MEMORY  The bookkeeping allocator had no room.
 * @whisper Space for the ledger, before the entries arrive
 */
hostmem_result hostmem_multi_arena_reserve(hostmem_multi_arena *m, uint32_t arena_count);

/**
 * @brief Borrow a caller owned buffer and append it to the chain as one more arena.
 *
 * The same borrowing hostmem_init_arena_borrow() does for a single arena, one link further out:
 * the block is filled but never freed. hostmem_multi_arena_release() lets it go untouched and
 * hostmem_multi_arena_shrink() stops at it rather than dropping it, so a host can feed its own
 * storage into the chain and keep ownership of it.
 *
 * The arena joins at the end, so it is used only once the arenas before it can no longer serve
 * a request. Borrow before the first allocation to have it filled first.
 *
 * @param[in,out] m        Allocator to extend; not NULL.
 * @param[in]     data     Buffer to bump through; not NULL, 8 byte aligned (@c alignas(8)).
 * @param[in]     capacity Usable bytes in @p data; must be > 0 and a multiple of 8.
 * @retval HOSTMEM_SUCCESS              Arena appended, empty and ready.
 * @retval HOSTMEM_ERROR_NULL_POINTER   @p m or @p data is NULL.
 * @retval HOSTMEM_ERROR_INVALID_PARAM  @p capacity is 0 or not a multiple of 8, or @p data is
 *                                      not 8 byte aligned.
 * @retval HOSTMEM_ERROR_OUT_OF_MEMORY  The descriptor vector could not grow; @p data is untouched.
 * @note @p data must outlive the chain, or be released from it by hostmem_multi_arena_release().
 * @whisper Borrowed ground joins the path, and stays borrowed
 */
hostmem_result hostmem_multi_arena_borrow(hostmem_multi_arena *m, uint8_t *data, uint32_t capacity);

/**
 * @brief Drop every allocation in every arena, keeping the arenas. O(arena count).
 *
 * Rewinds each bump index to 0 and lets the front marker fall back to the first arena. Nothing
 * is given back to the host and nothing is asked of it -- the next pass over the same workload
 * runs without a single allocation.
 *
 * @param[in,out] m Allocator to rewind; may be NULL.
 * @warning Every pointer this chain handed out dangles afterwards.
 * @whisper The tide goes out of every basin at once
 */
void hostmem_multi_arena_reset(hostmem_multi_arena *m);

/**
 * @brief Release the trailing arenas that hold nothing.
 *
 * Unwinds from the youngest arena and stops at the first one that still holds an allocation or
 * that the chain does not own. What sits before that arena stays, however empty -- the order of
 * the chain is part of its contract, and an arena removed from the middle would take the
 * pointers of the arenas after it with nothing gained. The descriptor vector is tightened onto
 * what remains.
 *
 * Typically called after hostmem_multi_arena_reset(), where every arena is empty and the whole
 * chain but the borrowed ground goes back to the host.
 *
 * @param[in,out] m Allocator to trim; not NULL.
 * @retval HOSTMEM_SUCCESS            Trailing empty arenas released; none is also success.
 * @retval HOSTMEM_ERROR_NULL_POINTER @p m is NULL.
 * @whisper What was never filled returns to the stream
 */
hostmem_result hostmem_multi_arena_shrink(hostmem_multi_arena *m);

/**
 * @brief Release every arena and the bookkeeping, but not the descriptor itself.
 *
 * Owned arenas are freed, borrowed ones are let go untouched, and the descriptor is left in the
 * empty state with its arena capacity kept -- ready to be filled again from nothing.
 *
 * @param[in,out] m Allocator to empty; may be NULL.
 * @warning Every pointer this chain handed out dangles afterwards.
 * @whisper Waters recede; the basins return to silence
 */
void hostmem_multi_arena_release(hostmem_multi_arena *m);

/**
 * @brief hostmem_multi_arena_release(), then give the descriptor itself back.
 *
 * @param[in]     m         From hostmem_multi_arena_create(), never stack or static storage;
 *                          may be NULL.
 * @param[in,out] allocator The allocator @p m came from -- the same one
 *                          hostmem_multi_arena_create() was handed, NULL for malloc. Not the
 *                          bookkeeping allocator, which the release step has already dealt with.
 * @retval HOSTMEM_SUCCESS  Released, or @p m was NULL and there was nothing to do.
 * @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED @p allocator is an arena and this
 *                          descriptor is not its most recent allocation. Every arena in the
 *                          chain is released either way; only the descriptor's own bytes stay
 *                          until that arena's reset.
 * @warning An @p allocator other than the one that handed the descriptor out leaves it where it
 *          is and answers with the warning above; NULL in place of an arena reaches free()
 *          unchecked. See hostmem_free().
 * @whisper The vessel that held vessels returns to the stream it came from
 */
hostmem_result hostmem_multi_arena_destroy(hostmem_multi_arena *m, hostmem *allocator);

/**
 * @brief Number of arenas in the chain.
 *
 * @param[in] m Allocator to query; may be NULL.
 * @return Arenas opened and borrowed and not yet released, or 0 if @p m is NULL.
 */
static inline uint32_t hostmem_multi_arena_arena_count(const hostmem_multi_arena *m) {
  return m ? hostmem_arena_vec_size(&m->arenas) : 0;
}

/**
 * @brief Walk the chain once and record what it holds.
 *
 * O(arena count) and free of side effects. @c reserved minus @c used is what the chain is
 * holding but not handing out -- the price of the last arena being only partly filled.
 *
 * @param[in]  m    Allocator to measure; not NULL.
 * @param[out] out  Receives the figures; not NULL. Untouched on failure.
 * @retval HOSTMEM_SUCCESS            Figures written.
 * @retval HOSTMEM_ERROR_NULL_POINTER @p m or @p out is NULL.
 * @whisper A count of what the ground still carries
 */
hostmem_result hostmem_multi_arena_measure(
    const hostmem_multi_arena *m, hostmem_multi_arena_stats *out
);

// ********** allocations, with data ptr and size explicit *******************

/**
 * @brief Allocate a raw buffer from the first arena with room, opening one if none has.
 *
 * The chain reserves HOSTMEM_ALIGN8(size) for the block. A request the configured arena
 * capacity cannot hold is not refused: it opens an arena sized exactly for it, which is full
 * from that moment on and stays out of the way of everything else.
 *
 * @param[out]    buffer Receives the allocation; not NULL.
 * @param[in]     size   Bytes to allocate; must be > 0.
 * @param[in,out] m      Allocator to draw from; not NULL -- NULL is not a malloc fallback here.
 * @retval HOSTMEM_SUCCESS                   Buffer allocated.
 * @retval HOSTMEM_ERROR_NULL_POINTER        @p buffer or @p m is NULL.
 * @retval HOSTMEM_ERROR_INVALID_PARAM       @p size is 0.
 * @retval HOSTMEM_ERROR_ARITHMETIC_OVERFLOW Rounding @p size up to 8 would wrap uint32_t.
 * @retval HOSTMEM_ERROR_OUT_OF_MEMORY       No arena had room and a new one could not be opened;
 *                                           the chain is unchanged.
 * @note The memory is not zeroed and holds whatever the previous tenant left.
 * @whisper Where the ground ends, new ground is opened
 */
hostmem_result hostmem_multi_arena_alloc(uint8_t **buffer, uint32_t size, hostmem_multi_arena *m);

/**
 * @brief hostmem_multi_arena_alloc() plus memcpy; copies exactly @p size bytes.
 *
 * @param[out]    dst_buffer Receives the copy; not NULL.
 * @param[in]     src        Source holding @p size bytes; not NULL.
 * @param[in]     size       Bytes to copy; must be > 0.
 * @param[in,out] m          Allocator to draw from; not NULL.
 * @retval HOSTMEM_SUCCESS             Copy allocated and filled.
 * @retval HOSTMEM_ERROR_NULL_POINTER  @p dst_buffer, @p src or @p m is NULL.
 * @retval HOSTMEM_ERROR_INVALID_PARAM @p size is 0.
 * @retval Anything hostmem_multi_arena_alloc() can return.
 * @whisper Water poured into a vessel newly shaped
 */
hostmem_result hostmem_multi_arena_clone(
    uint8_t **dst_buffer, const uint8_t *src, uint32_t size, hostmem_multi_arena *m
);

/**
 * @brief Give a block back, if it is the tail of the arena it came from.
 *
 * Finds the owning arena by address -- one comparison per arena -- and hands the block to
 * hostmem_free() there. Only the arena's most recent allocation can move its bump index back;
 * anything before it stays reserved until hostmem_multi_arena_reset(). Reclaiming may reopen an
 * arena the front marker had already passed, and the marker follows back to it.
 *
 * A chain is a bump allocator still: freeing in reverse order works, freeing in any other order
 * mostly does not, and `_reset` is how memory really comes back.
 *
 * @param[in]     buffer Buffer to release; may be NULL, which reclaims nothing.
 * @param[in]     size   Size the buffer was allocated with.
 * @param[in,out] m      Allocator the buffer came from; not NULL.
 * @retval HOSTMEM_SUCCESS             The arena took its bytes back.
 * @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Not the tail of its arena, so the block is
 *                                     still there -- do not treat it as released. A NULL
 *                                     @p buffer reports this too: NULL is never a tail.
 * @retval HOSTMEM_ERROR_NULL_POINTER  @p m is NULL.
 * @retval HOSTMEM_ERROR_INVALID_PARAM @p buffer lies in no arena of this chain.
 * @warning A @p size that does not match the allocation moves the index by the wrong amount and
 *          hands the same bytes out twice.
 * @whisper Form dissolves, but only at the water's edge
 */
hostmem_result hostmem_multi_arena_free(uint8_t *buffer, uint32_t size, hostmem_multi_arena *m);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // HOSTMEM_MULTI_ARENA_H
