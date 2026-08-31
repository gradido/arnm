#ifndef ARNM_FIXED_RING_H
#define ARNM_FIXED_RING_H

#include "arnm/memory.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Same reasoning as ARNM_BVEC_MAYBE_UNUSED in arnm/bucket_vector.h: ARNM_FIXED_RING_DEFINE
 * expands the whole accessor set into the consumer's translation unit, and clang reports every
 * one it did not call. The attribute says so; gcc and MSVC do not need it and do not mind it.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ARNM_FIXED_RING_MAYBE_UNUSED __attribute__((unused))
#else
#define ARNM_FIXED_RING_MAYBE_UNUSED
#endif

/**
 * @defgroup arnm_fixed_ring arnm_fixed_ring
 * @brief A bounded queue: first in, first out, and the room for it taken once.
 *
 * Elements enter at the back and leave at the front, in one block of storage that is reserved
 * at @ref arnm_fixed_ring_init() and never asked for again. What that block holds does move --
 * the front walks forward as elements are taken and wraps around the end -- but the block does
 * not, so the peak is known the moment init returns and known exactly: `capacity *
 * element_size`.
 *
 * ### Why a ring, where @ref arnm_bucket_vector would do
 *
 * A bucket vector is the right container while a round ends all at once: `_clear()` drops
 * everything, keeps the buckets warm, and the next round costs no allocation. A queue that is
 * consumed continuously has no such moment -- something is always still in flight -- and an
 * append-only container that is never entirely empty grows without bound. A ring reuses the
 * slot the front just left, so a queue that is drained as fast as it is filled sits still.
 *
 * The other half of the choice is the ceiling. A vector answers a burst by growing; a ring
 * answers it with @ref ARNM_ERROR_RESOURCE_EXHAUSTED, which is what lets a producer decide what
 * a backlog should mean rather than discovering it as memory.
 *
 * ### Full is an answer
 *
 * @ref arnm_fixed_ring_emplace() refuses a full ring and changes nothing. It does not drop the
 * oldest element to make room, because dropping it is a decision with consequences the ring
 * cannot see -- an entry may hold an arena, a file, a reference someone still owes a release
 * to. A caller who wants the oldest gone pops it first, and then the drop is at its own call
 * site with whatever cleanup belongs to it.
 *
 * ### Order comes out for free
 *
 * Elements leave in the order they arrived, so a queue whose entries all wait the same span is
 * also sorted by when their wait ends: the front is the earliest, and asking whether anything
 * is due is a look at one element rather than a walk. That is a property of the insertion
 * order, not of anything the ring does, and it lapses the moment entries carry different spans.
 *
 * ### The allocator is handed in, never kept
 *
 * @ref arnm_fixed_ring_init() is given one and @ref arnm_fixed_ring_free() is given it again,
 * the way @ref arnm_fixed_arena_pool works and unlike @ref arnm_bvec, which keeps one for its
 * whole life. Two things follow from that, and the second is the reason for it.
 *
 * The order things are released in is the caller's to know. A ring inside an arena the caller
 * resets wholesale never needs its block handed back at all; one drawn from a chain that
 * outlives it does. A stored allocator would make the ring look like that was its decision.
 *
 * And every other call in this header takes no allocator, which is not an omission but the
 * statement itself: a fixed ring does not grow and does not shrink, so between init and free
 * there is no call that could ask anyone for memory. That promise is readable in the
 * signatures rather than only in this paragraph -- a push cannot allocate, because it was
 * handed nothing to allocate from.
 *
 * What it asks of the caller is the duty every size in this library already carries: which
 * allocator a ring was built on is the caller's to remember. See the warning on
 * @ref arnm_fixed_ring_free().
 *
 * ### NULL
 *
 * As in @ref arnm_bucket_vector, and for the same reason: a wrapper on top needs no table.
 * Every call returning an @ref arnm_result answers @ref ARNM_ERROR_NULL_POINTER for a NULL ring
 * or a NULL output, and @ref arnm_fixed_ring_clear() does nothing. The inline accessors are the
 * exception on purpose -- they are a load and a comparison, and a check would cost more than
 * they do; they read a ring the caller holds.
 *
 * @note Elements are packed, so an element is 8 byte aligned exactly when its size is a
 *       multiple of 8 -- which `sizeof` already guarantees for any struct holding an 8 byte
 *       member. A payload with weaker needs, a `uint32_t` slot index say, packs tight.
 * @note Nothing here is thread safe. One ring belongs to one thread, or to whoever holds the
 *       lock around it.
 *
 * @whisper What enters at the back leaves at the front, and the ground beneath stays the same
 *
 * @{
 */

/**
 * @brief A bounded FIFO: one block, a front that walks, and the count between them.
 *
 * The back is not stored. It is `head + size`, wrapped once, which is what keeps a full ring
 * and an empty one from reading alike -- the ambiguity a stored tail index has to spend a slot
 * or a flag on.
 *
 * Not usable until @ref arnm_fixed_ring_init(): a zeroed descriptor holds no storage, so it
 * reads as empty through every accessor and refuses every write with
 * @ref ARNM_ERROR_NOT_INITIALIZED.
 *
 * @note Do not write these fields; read them through the API.
 */
typedef struct arnm_fixed_ring {
  /** The one block, `capacity * element_size` bytes, seen as slot 0. */
  uint8_t *slots;
  /** Slots in the block. Fixed at init and never changed. */
  uint32_t capacity;
  /** Slot the front element sits in; always < @c capacity. Where an emptied ring left it. */
  uint32_t head;
  /** Elements held right now, from 0 to @c capacity. */
  uint32_t size;
  /** Bytes per element, as handed to init. */
  uint16_t element_size;
} arnm_fixed_ring;

/**
 * @brief Reserve room for @p capacity elements, all of it, now.
 *
 * One allocation carries the whole ring, and the host is not asked again for as long as it
 * lives. A partial reservation is not a state this container has: either the block is there
 * when this returns @ref ARNM_SUCCESS, or nothing was kept and @p ring is untouched.
 *
 * Writes every field and reads none, so uninitialized storage is a valid input.
 *
 * @param[in,out] ring         Ring to initialize; not NULL. Need not be zeroed.
 * @param[in]     capacity     Elements it will ever hold at once; must be > 0. This is the
 *                             ceiling, for good.
 * @param[in]     element_size Bytes per element; 1 to `UINT16_MAX`.
 * @param[in,out] allocator    Where the block comes from, or NULL for the host. Not
 *                             remembered; @ref arnm_fixed_ring_free() has to be handed the
 *                             same one.
 * @retval ARNM_SUCCESS                   Empty and ready, the room already taken.
 * @retval ARNM_ERROR_NULL_POINTER        @p ring is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p capacity is 0, or @p element_size is 0 or above
 *                                        `UINT16_MAX`.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The block would not fit @ref ARNM_MAX_ALLOC_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       @p allocator had no room for the whole block.
 * @return Otherwise whatever @ref arnm_alloc() answered -- a capped chain that may open no more
 *         arena, an arena that was released. Nothing is kept and @p ring is untouched.
 * @warning Calling this on a ring that still holds its block leaks it. Use
 *          @ref arnm_fixed_ring_free() first.
 * @warning @p allocator is not stored. Which one a ring was built on is the caller's to keep,
 *          the way the sizes everything else here is freed with are.
 * @note Behind an arena this is the only allocation the ring makes, so it can be taken at
 *       startup and nothing after it competes for the tail. It is also the last call that
 *       touches an allocator until the ring is freed -- nothing between the two can.
 * @whisper The bed is dug once, and the water finds its own way around it
 */
arnm_result arnm_fixed_ring_init(
    arnm_fixed_ring *ring, uint32_t capacity, size_t element_size, arnm *allocator
);

/**
 * @brief Give the block back to the allocator it came from, leaving nothing reserved.
 *
 * The ring returns to the state it had before init: no storage, no capacity, and every write
 * refused with @ref ARNM_ERROR_NOT_INITIALIZED until @ref arnm_fixed_ring_init() is called
 * again. Unlike @ref arnm_bvec_free() nothing is kept back for a second round, because a ring
 * without its block has no shape left to reuse.
 *
 * When this is called, and whether it is called at all, is the caller's to decide -- a ring
 * inside an arena that is about to be reset can simply be left where it is.
 *
 * @param[in,out] ring      Ring to empty; not NULL. One that holds no block is already there
 *                          and answers @ref ARNM_SUCCESS.
 * @param[in,out] allocator The allocator the block came from -- the same one
 *                          @ref arnm_fixed_ring_init() was handed, NULL for the host.
 * @retval ARNM_SUCCESS            The block is back and the ring holds nothing.
 * @retval ARNM_ERROR_NULL_POINTER @p ring is NULL.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED @p allocator is an arena and the block is not
 *                                 its most recent allocation. The ring is empty either way;
 *                                 those bytes come back on that arena's own reset.
 * @warning An @p allocator other than the one the ring was built on leaves the block where it
 *          is and answers with the warning above -- except NULL for memory an arena gave, which
 *          reaches free() unchecked. See arnm_free().
 * @warning Every pointer into the ring is dangling afterwards.
 * @whisper The bed is filled in, and the ground is level again
 */
arnm_result arnm_fixed_ring_free(arnm_fixed_ring *ring, arnm *allocator);

/**
 * @brief Drop every element and keep the block. O(1).
 *
 * Two counters move and nothing is freed or written, so the next element lands in storage that
 * is already warm.
 *
 * @param[in,out] ring Ring to empty; NULL is a no-op.
 * @warning Every pointer into the ring is dangling afterwards.
 * @whisper The channel runs dry, its banks untouched
 */
void arnm_fixed_ring_clear(arnm_fixed_ring *ring);

/**
 * @brief Claim the slot at the back without writing it -- build large payloads in place.
 *
 * The push to use for anything bigger than a machine word: the element is constructed where it
 * will live instead of being built on the stack and copied in. The ring counts the slot before
 * this returns, so a caller that fails half way through filling it pops it back off.
 *
 * @param[in,out] ring     Ring; not NULL, initialized.
 * @param[out]    out_slot Receives the element's storage; not NULL. Holds whatever the previous
 *                         occupant of that slot left behind -- the caller writes it.
 * @retval ARNM_SUCCESS                  The slot is yours, and the ring already counts it.
 * @retval ARNM_ERROR_NULL_POINTER       @p ring or @p out_slot is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED    @p ring holds no block.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED The ring is full. Nothing is wrong with the request and
 *                                       nothing was changed; the ring is simply the size it is.
 * @note The address is stable only until the element is popped. Storage is reused, so a pointer
 *       held across a pop and a later push reads someone else's element.
 * @whisper A place at the back, taken before it is filled
 */
arnm_result arnm_fixed_ring_emplace(arnm_fixed_ring *ring, void **out_slot);

/**
 * @brief Append a copy of @p value at the back, read through a pointer.
 *
 * @ref arnm_fixed_ring_emplace() followed by a copy of one element. The pointer form avoids
 * handing a bulky payload through a parameter; the generated `name##_push` takes it by value.
 *
 * @param[in,out] ring  Ring; not NULL, initialized.
 * @param[in]     value Element to copy in; not NULL, and at least `element_size` bytes.
 * @retval ARNM_SUCCESS            Appended.
 * @retval ARNM_ERROR_NULL_POINTER @p ring or @p value is NULL.
 * @return Otherwise whatever @ref arnm_fixed_ring_emplace() answered -- a refusal adds nothing.
 * @whisper The shape is read once, and set down at the back
 */
arnm_result arnm_fixed_ring_push_ptr(arnm_fixed_ring *ring, const void *value);

/**
 * @brief Remove the front element. Its slot is the next one handed out. O(1).
 *
 * @param[in,out] ring Ring; not NULL.
 * @retval ARNM_SUCCESS                         One element fewer, the front moved on.
 * @retval ARNM_ERROR_NULL_POINTER              @p ring is NULL.
 * @retval ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS The ring was empty.
 * @note Nothing is overwritten or cleared. The bytes stay until a later push claims the slot,
 *       so an element read through @ref arnm_fixed_ring_front() before the pop is readable for
 *       exactly as long as the ring is not pushed to -- which is a thin promise to lean on, and
 *       @ref arnm_fixed_ring_pop_copy() is the call that does not need it.
 * @whisper The first to arrive is the first to go
 */
arnm_result arnm_fixed_ring_pop(arnm_fixed_ring *ring);

/**
 * @brief Copy the front element out and remove it, in one step.
 *
 * What a consumer holding a lock wants: the element leaves the ring and lands in storage of the
 * caller's own, so the lock can be released before the work on it starts. Nothing here can fail
 * half way -- either @p dst holds the element and the ring is one shorter, or neither happened.
 *
 * @param[in,out] ring Ring; not NULL.
 * @param[out]    dst  Receives the element; not NULL, and at least `element_size` bytes.
 * @retval ARNM_SUCCESS                         Copied out and removed.
 * @retval ARNM_ERROR_NULL_POINTER              @p ring or @p dst is NULL.
 * @retval ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS The ring was empty. @p dst is untouched.
 * @whisper Carried out of the current before the bank is let go
 */
arnm_result arnm_fixed_ring_pop_copy(arnm_fixed_ring *ring, void *dst);

/** Elements currently held. */
static inline uint32_t arnm_fixed_ring_size(const arnm_fixed_ring *ring) {
  return ring->size;
}

/** Elements it can hold at once, settled at init. */
static inline uint32_t arnm_fixed_ring_capacity(const arnm_fixed_ring *ring) {
  return ring->capacity;
}

/** Room left before the next push is refused. */
static inline uint32_t arnm_fixed_ring_available(const arnm_fixed_ring *ring) {
  return ring->capacity - ring->size;
}

/** True while the ring holds nothing. */
static inline bool arnm_fixed_ring_is_empty(const arnm_fixed_ring *ring) {
  return 0 == ring->size;
}

/** True when the next push would be refused. */
static inline bool arnm_fixed_ring_is_full(const arnm_fixed_ring *ring) {
  return ring->size == ring->capacity;
}

/**
 * Unchecked access, counted from the front -- @p index must be < _size().
 *
 * One addition and one comparison: @c head is below the capacity and @p index below the size,
 * so their sum passes the end at most once and a single subtraction brings it back. That is
 * what the ring buys by not requiring a power of two capacity, which would round a queue of
 * 1000 up to 1024 and reserve the difference for the rest of the process.
 */
static inline void *arnm_fixed_ring_get(const arnm_fixed_ring *ring, uint32_t index) {
  uint32_t slot = ring->head + index;
  if (slot >= ring->capacity) { slot -= ring->capacity; }
  return ring->slots + (size_t)slot * ring->element_size;
}

/** Bounds-checked access, counted from the front; NULL when @p index has no element. */
static inline void *arnm_fixed_ring_at(const arnm_fixed_ring *ring, uint32_t index) {
  if (index >= ring->size) { return NULL; }
  return arnm_fixed_ring_get(ring, index);
}

/** The element that leaves next, or NULL while empty. */
static inline void *arnm_fixed_ring_front(const arnm_fixed_ring *ring) {
  return ring->size ? arnm_fixed_ring_get(ring, 0) : NULL;
}

/** The element that arrived last, or NULL while empty. */
static inline void *arnm_fixed_ring_back(const arnm_fixed_ring *ring) {
  return ring->size ? arnm_fixed_ring_get(ring, ring->size - 1) : NULL;
}

/**
 * @brief Bytes this ring holds from its allocator.
 *
 * The whole footprint, known from init and constant until it is freed -- what a host sizes its
 * own budget against.
 *
 * @param[in] ring Ring to measure; may be NULL.
 * @return `capacity * element_size`, or 0 if @p ring is NULL or holds no block.
 */
uint32_t arnm_fixed_ring_reserved(const arnm_fixed_ring *ring);

/**
 * @brief Copy the whole queue into one contiguous array, front first.
 *
 * The bridge to anything that wants a plain array. The elements are in at most two runs -- from
 * the front to the end of the block, and from the start of the block to the back -- so this
 * costs one memcpy, or two when the queue straddles the wrap.
 *
 * @param[in]  ring         Ring; not NULL.
 * @param[out] dst          Destination; not NULL, and room for at least @c size elements.
 * @param[in]  dst_capacity Elements @p dst holds -- elements, not bytes.
 * @retval ARNM_SUCCESS                           Copied; @p dst holds @c size elements in queue
 *                                                order.
 * @retval ARNM_ERROR_NULL_POINTER                @p ring or @p dst is NULL.
 * @retval ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL @p dst_capacity is below @c size. Nothing was
 *                                                written.
 * @note An empty ring copies nothing and succeeds, whatever @p dst_capacity says.
 * @whisper The wrapped line laid out straight
 */
arnm_result arnm_fixed_ring_copy_to(const arnm_fixed_ring *ring, void *dst, uint32_t dst_capacity);

/**
 * @brief Generate a typed accessor set called @p name for elements of @p type.
 *
 * Every wrapper forwards to the function of the same name above, with `sizeof(type)` filled in
 * and the void pointers replaced by `type *`. The storage stays one implementation; what this
 * buys is the compiler checking that a ring of one type is not read as another.
 *
 * @code
 * ARNM_FIXED_RING_DEFINE(slot_ring, uint32_t)
 * arnm_fixed_ring ring;
 * slot_ring_init(&ring, 4096, NULL);      // 4096 slot indices, host memory, taken now
 * slot_ring_push(&ring, 17u);
 * const uint32_t *oldest = slot_ring_front(&ring);
 * slot_ring_pop(&ring);
 * slot_ring_free(&ring, NULL);   // the same allocator init was handed
 * @endcode
 *
 * @param name Prefix for the generated functions.
 * @param type Element type. Its size has to satisfy @ref arnm_fixed_ring_init().
 * @note Generates one extra wrapper with no counterpart above: `name##_push`, which takes the
 *       element by value where @ref arnm_fixed_ring_push_ptr() takes its address.
 */
#define ARNM_FIXED_RING_DEFINE(name, type)                                                         \
  /** Reserve room for @p capacity elements, all of it, now. */                                    \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_init(                              \
      arnm_fixed_ring *ring, uint32_t capacity, arnm *allocator                                    \
  ) {                                                                                              \
    return arnm_fixed_ring_init(ring, capacity, sizeof(type), allocator);                          \
  }                                                                                                \
                                                                                                   \
  /** Give the block back to the allocator it came from, leaving nothing reserved. */              \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_free(                              \
      arnm_fixed_ring *ring, arnm *allocator                                                       \
  ) {                                                                                              \
    return arnm_fixed_ring_free(ring, allocator);                                                  \
  }                                                                                                \
                                                                                                   \
  /** Drop every element and keep the block. O(1). */                                              \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline void name##_clear(arnm_fixed_ring *ring) {            \
    arnm_fixed_ring_clear(ring);                                                                   \
  }                                                                                                \
                                                                                                   \
  /** Claim the slot at the back without writing it. */                                            \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_emplace(                           \
      arnm_fixed_ring *ring, type **out_slot                                                       \
  ) {                                                                                              \
    return arnm_fixed_ring_emplace(ring, (void **)out_slot);                                       \
  }                                                                                                \
                                                                                                   \
  /** Append a value at the back. */                                                               \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_push(                              \
      arnm_fixed_ring *ring, type value                                                            \
  ) {                                                                                              \
    type *slot;                                                                                    \
    arnm_result result = name##_emplace(ring, &slot);                                              \
    if (result != ARNM_SUCCESS) return result;                                                     \
    *slot = value;                                                                                 \
    return ARNM_SUCCESS;                                                                           \
  }                                                                                                \
                                                                                                   \
  /** Append a value read through a pointer -- avoids passing bulky payloads by value. */          \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_push_ptr(                          \
      arnm_fixed_ring *ring, const type *value                                                     \
  ) {                                                                                              \
    return arnm_fixed_ring_push_ptr(ring, value);                                                  \
  }                                                                                                \
                                                                                                   \
  /** Remove the front element. Its slot is the next one handed out. */                            \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_pop(arnm_fixed_ring *ring) {       \
    return arnm_fixed_ring_pop(ring);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Copy the front element out and remove it, in one step. */                                    \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_pop_copy(                          \
      arnm_fixed_ring *ring, type *dst                                                             \
  ) {                                                                                              \
    return arnm_fixed_ring_pop_copy(ring, dst);                                                    \
  }                                                                                                \
                                                                                                   \
  /** Elements currently held. */                                                                  \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline uint32_t name##_size(const arnm_fixed_ring *ring) {   \
    return arnm_fixed_ring_size(ring);                                                             \
  }                                                                                                \
                                                                                                   \
  /** Elements it can hold at once, settled at init. */                                            \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline uint32_t name##_capacity(                             \
      const arnm_fixed_ring *ring                                                                  \
  ) {                                                                                              \
    return arnm_fixed_ring_capacity(ring);                                                         \
  }                                                                                                \
                                                                                                   \
  /** Room left before the next push is refused. */                                                \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline uint32_t name##_available(                            \
      const arnm_fixed_ring *ring                                                                  \
  ) {                                                                                              \
    return arnm_fixed_ring_available(ring);                                                        \
  }                                                                                                \
                                                                                                   \
  /** True while the ring holds nothing. */                                                        \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline bool name##_is_empty(const arnm_fixed_ring *ring) {   \
    return arnm_fixed_ring_is_empty(ring);                                                         \
  }                                                                                                \
                                                                                                   \
  /** True when the next push would be refused. */                                                 \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline bool name##_is_full(const arnm_fixed_ring *ring) {    \
    return arnm_fixed_ring_is_full(ring);                                                          \
  }                                                                                                \
                                                                                                   \
  /** Unchecked access, counted from the front -- @p index must be < size. */                      \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline type *name##_get(                                     \
      const arnm_fixed_ring *ring, uint32_t index                                                  \
  ) {                                                                                              \
    return (type *)arnm_fixed_ring_get(ring, index);                                               \
  }                                                                                                \
                                                                                                   \
  /** Bounds-checked access from the front; NULL when @p index has no element. */                  \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline type *name##_at(                                      \
      const arnm_fixed_ring *ring, uint32_t index                                                  \
  ) {                                                                                              \
    return (type *)arnm_fixed_ring_at(ring, index);                                                \
  }                                                                                                \
                                                                                                   \
  /** The element that leaves next, or NULL while empty. */                                        \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline type *name##_front(const arnm_fixed_ring *ring) {     \
    return (type *)arnm_fixed_ring_front(ring);                                                    \
  }                                                                                                \
                                                                                                   \
  /** The element that arrived last, or NULL while empty. */                                       \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline type *name##_back(const arnm_fixed_ring *ring) {      \
    return (type *)arnm_fixed_ring_back(ring);                                                     \
  }                                                                                                \
                                                                                                   \
  /** Bytes this ring holds from its allocator. */                                                 \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline uint32_t name##_reserved(                             \
      const arnm_fixed_ring *ring                                                                  \
  ) {                                                                                              \
    return arnm_fixed_ring_reserved(ring);                                                         \
  }                                                                                                \
                                                                                                   \
  /** Copy the whole queue into one contiguous array, front first. */                              \
  ARNM_FIXED_RING_MAYBE_UNUSED static inline arnm_result name##_copy_to(                           \
      const arnm_fixed_ring *ring, type *dst, uint32_t dst_capacity                                \
  ) {                                                                                              \
    return arnm_fixed_ring_copy_to(ring, dst, dst_capacity);                                       \
  }

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_FIXED_RING_H
