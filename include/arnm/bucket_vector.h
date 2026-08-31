#ifndef ARNM_BUCKET_VECTOR_H
#define ARNM_BUCKET_VECTOR_H

#include "arnm/memory.h"
#include "arnm/result.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/*
 * ARNM_BVEC_DEFINE generates the whole set of typed accessors, and a caller that wants three of
 * them gets all of them -- as `static inline` in its own translation unit, where clang's
 * -Wunused-function reports every one it did not call. That is noise a consumer cannot fix
 * without abandoning the macro, so the generated wrappers carry the attribute that says so.
 * gcc does not warn about unused `static inline` in the first place, and MSVC's C4514 is off at
 * every warning level, so the empty fallback costs nothing.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ARNM_BVEC_MAYBE_UNUSED __attribute__((unused))
#else
#define ARNM_BVEC_MAYBE_UNUSED
#endif

/**
 * @defgroup arnm_bucket_vector arnm_bucket_vector
 * @brief A growable sequence whose elements never move.
 *
 * Elements live in fixed size buckets rather than in one block, so growing means opening
 * another bucket and never copying what is already there. That is the reason to reach for this
 * over a flat array: a pointer into it stays valid for the life of the element, and appending
 * costs the same whether the sequence holds ten items or a million. The price is one indirection
 * per lookup by index -- `arnm_bvec_bucket_data()` walks bucket by bucket instead, which
 * gives back contiguous runs.
 *
 * ### One struct, typed accessors
 *
 * Every vector is an @ref arnm_bvec whatever it holds; the element size is an
 * @ref arnm_bvec_init() argument. @ref ARNM_BVEC_DEFINE generates a typed set of wrappers
 * around it, so `my_vec_push(&v, value)` is checked by the compiler while the storage stays one
 * implementation.
 *
 * ### Where the memory comes from, and what bounds it
 *
 * The allocator is named at @ref arnm_bvec_init() and used for both the buckets and the index
 * array of pointers to them. That index is capped at @ref ARNM_BVEC_MAX_INDEX_CAPACITY slots,
 * which is the real ceiling on a vector: bucket capacity times that many buckets. Choose the
 * bucket exponent with the expected element count in mind -- small buckets do not merely waste
 * fewer bytes, they also run out sooner.
 *
 * ### NULL
 *
 * Uniform, so a wrapper built on top needs no table: every call that returns an
 * @ref arnm_result answers @ref ARNM_ERROR_NULL_POINTER for a NULL vector or a NULL output,
 * and the two that return nothing -- @ref arnm_bvec_clear() and @ref arnm_bvec_free() -- do
 * nothing. No call in this header dereferences a pointer it was told about without checking it.
 *
 * @note Behind an arena, call @ref arnm_bvec_reserve() once up front. The index array regrows
 *       in steps, and an arena cannot take a superseded one back, so every earlier copy stays
 *       stranded in it.
 * @note Nothing here is thread safe. One vector belongs to one thread at a time.
 * @{
 */

/** @brief Initial number of bucket pointer slots in the index array. */
#define ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE 8

/**
 * @brief Largest index array the allocator can hand out, counted in bucket pointer slots.
 */
#define ARNM_BVEC_MAX_INDEX_CAPACITY ((uint16_t)((UINT16_MAX - 7u) / sizeof(void *)))

/**
 * @brief A bucket vector, whatever it holds.
 *
 * The element size and the bucket exponent live here rather than in the type, so one
 * implementation serves every payload and @ref ARNM_BVEC_DEFINE only adds the typing. Fields
 * are readable -- @c size and @c buckets are how a debugger or a test inspects one -- but
 * writing any of them is the caller stepping outside the interface.
 *
 * Not usable until @ref arnm_bvec_init(): a zeroed descriptor has no element size, so it reads
 * as empty through every accessor and refuses every write with @ref ARNM_ERROR_INVALID_STATE.
 */
typedef struct arnm_bvec {
  arnm *allocator;          /**< Where buckets and the index array come from; NULL is the host. */
  void **buckets;           /**< Index array: one pointer per allocated bucket. */
  uint16_t element_size;    /**< Byte Size per Element */
  uint16_t bucket_capacity; /**< Pointer slots available in @c buckets. */
  uint16_t tail_index;      /**< Index of @c tail within @c buckets. */
  uint16_t tail_used;       /**< Slots in @c tail; BUCKET_CAPACITY when full or @c tail NULL. */
  void *tail;               /**< Bucket currently being filled; NULL while empty. */
  uint32_t size;            /**< Total element count. */
  uint8_t bucket_capacity_max_log2; /**< Elements per bucket as a power of two, 1 to 15. */
  uint8_t index_grow_step_size;     /**< Slots @c buckets grows by when it runs out. */
  /**
   * Buckets the vector holds, whether they carry elements or not.
   *
   * @c tail_index answers the other question -- how far the elements reach, which is what
   * drops back to 0 on @ref arnm_bvec_clear() while the buckets stay. Slots below this count
   * hold a bucket and slots above it are NULL, which is the invariant every call here keeps.
   *
   * Counted rather than derived, and that is a fix rather than a saving: reading it off the
   * index array meant scanning forward while the slots were non-empty, and a vector whose
   * buckets are all there before the first push -- the one @ref arnm_bvec_reserve() makes --
   * scanned to the end of the array on every single one of them.
   */
  uint16_t allocated_count;
} arnm_bvec;

/**
 * @brief Prepare an empty vector. Allocates nothing; the first push opens the first bucket.
 *
 * @param[out]    v                    Descriptor to initialize; not NULL. Every field is
 *                                     written and none is read, so uninitialized storage is a
 *                                     valid input.
 * @param[in]     bucket_capacity_log2 Elements per bucket as a power of two; 1 to 15. Bigger
 *                                     buckets mean fewer allocations and a higher ceiling (see
 *                                     @ref ARNM_BVEC_MAX_INDEX_CAPACITY); smaller ones waste
 *                                     less on a vector that stays short.
 * @param[in]     index_grow_step_size Bucket slots the index array grows by at a time, or 0 for
 *                                     @ref ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE.
 * @param[in]     element_size         Bytes per element; 1 to `UINT16_MAX`, and small enough
 *                                     that a whole bucket still fits a uint32_t.
 * @param[in,out] allocator            Where buckets and the index array come from, or NULL for
 *                                     the host. Kept for the vector's whole life.
 * @retval ARNM_SUCCESS             Ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER  @p v is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM A size or an exponent is outside the bounds above.
 * @warning Calling this on a vector that still holds buckets leaks them. Use
 *          @ref arnm_bvec_free() first.
 * @note 15 and not 16: a full bucket's element count is carried in a uint16_t.
 * @whisper An empty shelf, its compartments already measured
 */
arnm_result arnm_bvec_init(
    arnm_bvec *v,
    uint8_t bucket_capacity_log2,
    uint8_t index_grow_step_size,
    size_t element_size,
    arnm *allocator
);

/**
 * @brief Allocate buckets and index slots for @p element_count elements ahead of time.
 *
 * Idempotent and never shrinks: asking for less than the vector already holds succeeds and
 * changes nothing. Worth doing whenever the size is roughly known, and near mandatory behind an
 * arena -- see the note on the module.
 *
 * @param[in,out] bvec          Vector; not NULL, initialized.
 * @param[in]     element_count Elements to make room for; must be > 0.
 * @retval ARNM_SUCCESS                   The room is there.
 * @retval ARNM_ERROR_NULL_POINTER        @p bvec is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p element_count is 0 -- a count computed to nothing
 *                                        is the caller's arithmetic going wrong, not a
 *                                        reservation of nothing.
 * @retval ARNM_ERROR_INVALID_STATE       @p bvec never saw @ref arnm_bvec_init().
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW More than the index array can address, or more bytes
 *                                        than the allocator counts in. Nothing was taken.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator ran out part way. What it did hand over
 *                                        stays with the vector and is used by later pushes.
 * @whisper Room made before it is needed
 */
arnm_result arnm_bvec_reserve(arnm_bvec *bvec, uint32_t element_count);

/**
 * @brief Release every bucket that holds no element and tighten the index array onto the rest.
 *
 * The counterpart to @ref arnm_bvec_clear(): that one keeps the buckets for reuse, this one
 * gives them back. Buckets still holding elements are never touched, so the sequence is
 * unchanged and every pointer into it stays valid.
 *
 * @param[in,out] v Vector; not NULL.
 * @retval ARNM_SUCCESS             Whatever could be released was. Also the answer for a vector
 *                                  that holds no bucket at all, including one that never saw
 *                                  @ref arnm_bvec_init() -- there is nothing to give back and
 *                                  nothing to object to.
 * @retval ARNM_ERROR_NULL_POINTER  @p v is NULL.
 * @retval ARNM_ERROR_INVALID_STATE @p v holds buckets but its counters do not describe them;
 *                                  reachable only by writing the descriptor's fields directly.
 * @note Behind an arena only the most recent buckets come back. Release walks newest first and
 *       stops at the first block the arena will not take, which keeps the rest reusable rather
 *       than stranding it -- and that is a success, not a partial failure. On the host nothing
 *       is ever refused, so every unused bucket goes back; it walks oldest first there, for the
 *       reason on @ref arnm_bvec_free().
 * @whisper What holds nothing is handed back
 */
arnm_result arnm_bvec_shrink(arnm_bvec *v);

/**
 * @brief Drop every element and keep every bucket for immediate reuse. O(1).
 *
 * Three counters move and nothing is freed, which is what makes this the call for work that
 * arrives in rounds: the next round refills storage that is already warm.
 *
 * @param[in,out] v Vector; NULL is a no-op.
 * @warning Every pointer into the vector is dangling afterwards, and
 *          `arnm_bvec_bucket_count()` drops to 0 while the buckets are still held.
 * @whisper Swept clean, the shelves left standing
 */
void arnm_bvec_clear(arnm_bvec *v);

/**
 * @brief Release every bucket and the index array, leaving a reusable descriptor.
 *
 * What stays is what @ref arnm_bvec_init() wrote: the allocator, the element size, the bucket
 * exponent and the growth step. So a freed vector can be filled again without another init.
 *
 * @param[in,out] v Vector; NULL is a no-op, as is one that never allocated.
 * @warning Every pointer into the vector is dangling afterwards.
 * @note Which end the buckets are released from follows the allocator. An arena is unwound
 *       newest first, the only order that returns anything to it; the host is given them
 *       oldest first, because releasing the newest block first walks the top of its heap
 *       downwards and makes it trim page by page. @ref arnm_bvec_shrink() splits the same way.
 * @whisper The shelves come down, the measurements are remembered
 */
void arnm_bvec_free(arnm_bvec *v);

/**
 * @brief Cold path of @ref arnm_bvec_emplace(): open the next bucket and claim its first slot.
 *
 * Public because it is where every allocation this container performs happens, which makes it
 * the one to reach for when a caller wants to see them. Ordinary appends go through
 * @ref arnm_bvec_emplace() or @ref arnm_bvec_push_ptr(), which come here only when the open
 * bucket is full.
 *
 * @param[in,out] v        Vector; not NULL, initialized.
 * @param[out]    out_slot Receives the new element's storage; not NULL. Uninitialized memory --
 *                         the caller writes it.
 * @retval ARNM_SUCCESS                   A bucket was opened and @p out_slot points into it.
 * @retval ARNM_ERROR_NULL_POINTER        @p v or @p out_slot is NULL.
 * @retval ARNM_ERROR_INVALID_STATE       @p v never saw @ref arnm_bvec_init().
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The index array is at
 *                                        @ref ARNM_BVEC_MAX_INDEX_CAPACITY; this vector holds
 *                                        all it ever can.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator had no bucket to give. The vector is
 *                                        unchanged and still holds what it held.
 * @whisper A new compartment, opened only when the last one filled
 */
arnm_result arnm_bvec_grow(arnm_bvec *v, void **out_slot);

/**
 * @brief Claim the next slot without writing it -- build large payloads in place.
 *
 * The append to use for anything bigger than a machine word: the element is constructed once,
 * where it will live, instead of being built on the stack and copied in.
 *
 * @param[in,out] v        Vector; not NULL, initialized.
 * @param[out]    out_slot Receives the element's storage; not NULL. Uninitialized memory.
 * @retval ARNM_SUCCESS            The slot is yours, and the vector already counts it.
 * @retval ARNM_ERROR_NULL_POINTER @p v or @p out_slot is NULL.
 * @return Otherwise whatever @ref arnm_bvec_grow() answered -- a failure adds no element.
 * @note The returned address is stable for as long as the element exists. Growing the vector
 *       never moves it; only @ref arnm_bvec_clear(), @ref arnm_bvec_free() and a
 *       @ref arnm_bvec_pop() that reaches it end its life.
 * @whisper Built where it will stand, not carried there
 */
arnm_result arnm_bvec_emplace(arnm_bvec *v, void **out_slot);

/**
 * @brief Append a copy of @p value, read through a pointer.
 *
 * @ref arnm_bvec_emplace() followed by a copy of one element. The pointer form avoids handing a
 * bulky payload through a parameter; the generated `name##_push` takes it by value instead.
 *
 * @param[in,out] v     Vector; not NULL, initialized.
 * @param[in]     value Element to copy in; not NULL, and at least `element_size` bytes.
 * @retval ARNM_SUCCESS            Appended.
 * @retval ARNM_ERROR_NULL_POINTER @p v or @p value is NULL.
 * @return Otherwise whatever @ref arnm_bvec_grow() answered -- a failure adds no element.
 * @whisper The shape is read once, and set down
 */
arnm_result arnm_bvec_push_ptr(arnm_bvec *v, const void *value);

/**
 * @brief Remove the last element. The vacated bucket stays allocated for the next push.
 *
 * @param[in,out] v Vector; not NULL.
 * @retval ARNM_SUCCESS                          One element fewer.
 * @retval ARNM_ERROR_NULL_POINTER               @p v is NULL.
 * @retval ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS  The vector was empty.
 * @note Emptying a bucket does not release it -- use @ref arnm_bvec_shrink() for that. So a
 *       drain and refill costs no allocation at all.
 * @whisper The last one set down is the first taken back
 */
arnm_result arnm_bvec_pop(arnm_bvec *v);

/** Elements currently held. */
static inline uint32_t arnm_bvec_size(const arnm_bvec *v) {
  return v->size;
}

/** Unchecked access -- @p index must be < _size(). Two loads: the bucket, then the element. */
static inline void *arnm_bvec_get(const arnm_bvec *v, uint32_t index) {
  const uint8_t log2_bucket_capacity = v->bucket_capacity_max_log2;
  const uint16_t bucket_index = (uint16_t)(index >> log2_bucket_capacity);
  const uint32_t index_in_bucket = index & (((uint32_t)1 << log2_bucket_capacity) - 1);
  return (uint8_t *)v->buckets[bucket_index] + (size_t)index_in_bucket * v->element_size;
}

/** Bounds-checked access; NULL when @p index has no element. */
static inline void *arnm_bvec_at(const arnm_bvec *v, uint32_t index) {
  if (index >= v->size) return NULL;
  return arnm_bvec_get(v, index);
}

/** First element, or NULL while empty. */
static inline void *arnm_bvec_front(const arnm_bvec *v) {
  return v->size ? v->buckets[0] : NULL;
}

/** Last element, or NULL while empty. */
static inline void *arnm_bvec_back(const arnm_bvec *v) {
  return v->tail ? (uint8_t *)v->tail + (size_t)(v->tail_used - 1) * v->element_size : NULL;
}

/** Number of buckets holding elements -- the outer bound for bucket-wise iteration. */
static inline uint16_t arnm_bvec_bucket_count(const arnm_bvec *v) {
  return v->tail ? v->tail_index + 1 : 0;
}

/** Contiguous start of bucket @p bucket; @p bucket must be < _bucket_count(). */
static inline void *arnm_bvec_bucket_data(const arnm_bvec *v, uint16_t bucket) {
  return v->buckets[bucket];
}

/** Elements held in bucket @p bucket; full except possibly the last one. */
static inline uint32_t arnm_bvec_bucket_size(const arnm_bvec *v, uint16_t bucket) {
  return bucket == v->tail_index ? v->tail_used : (uint16_t)1 << v->bucket_capacity_max_log2;
}

/**
 * @brief Flatten the whole sequence into one contiguous array, bucket by bucket.
 *
 * The bridge to anything that wants a plain array -- a write, an API that takes a pointer and a
 * count. Copies whole buckets rather than elements, so it costs one memcpy per bucket.
 *
 * @param[in]  v            Vector; not NULL.
 * @param[out] dst          Destination; not NULL, and room for at least @c size elements.
 * @param[in]  dst_capacity Elements @p dst holds -- elements, not bytes.
 * @retval ARNM_SUCCESS                          Copied; @p dst holds @c size elements.
 * @retval ARNM_ERROR_NULL_POINTER               @p v or @p dst is NULL.
 * @retval ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL @p dst_capacity is below @c size. Nothing was
 *                                               written.
 * @note An empty vector copies nothing and succeeds, whatever @p dst_capacity says.
 * @whisper Scattered compartments poured into one line
 */
arnm_result arnm_bvec_copy_to(const arnm_bvec *v, void *dst, uint32_t dst_capacity);

/**
 * @brief Generate a typed accessor set called @p name for elements of @p type.
 *
 * Every wrapper forwards to the function of the same name below, with `sizeof(type)` filled in
 * and the void pointers replaced by `type *`. The storage stays one implementation; what this
 * buys is the compiler checking that a vector of one type is not read as another.
 *
 * @code
 * ARNM_BVEC_DEFINE(u32_vec, uint32_t)
 * arnm_bvec v;
 * u32_vec_init(&v, 6, 0, NULL);   // 64 elements per bucket, default growth, host memory
 * u32_vec_push(&v, 42u);
 * const uint32_t *first = u32_vec_front(&v);
 * u32_vec_free(&v);
 * @endcode
 *
 * @param name Prefix for the generated functions.
 * @param type Element type. Its size has to satisfy @ref arnm_bvec_init().
 * @note Generates one extra wrapper with no counterpart below: `name##_push`, which takes the
 *       element by value where @ref arnm_bvec_push_ptr() takes its address.
 */
#define ARNM_BVEC_DEFINE(name, type)                                                               \
  /** Prepare an empty vector. Allocates nothing; the first push opens the first bucket. */        \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_init(                                    \
      arnm_bvec *v, uint8_t bucket_capacity_log2, uint8_t index_grow_step_size, arnm *allocator    \
  ) {                                                                                              \
    return arnm_bvec_init(v, bucket_capacity_log2, index_grow_step_size, sizeof(type), allocator); \
  }                                                                                                \
                                                                                                   \
  /** Allocate buckets and index slots for @p element_count elements ahead of time. */             \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_reserve(                                 \
      arnm_bvec *v, uint32_t element_count                                                         \
  ) {                                                                                              \
    return arnm_bvec_reserve(v, element_count);                                                    \
  }                                                                                                \
                                                                                                   \
  /** Release every bucket that holds no element and tighten the index array onto the rest. */     \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_shrink(arnm_bvec *v) {                   \
    return arnm_bvec_shrink(v);                                                                    \
  }                                                                                                \
                                                                                                   \
  /** Drop all elements, keep every allocated bucket for immediate reuse. O(1). */                 \
  ARNM_BVEC_MAYBE_UNUSED static inline void name##_clear(arnm_bvec *v) {                           \
    arnm_bvec_clear(v);                                                                            \
  }                                                                                                \
                                                                                                   \
  /** Release every bucket and the index array, leaving a zeroed, reusable descriptor.   */        \
  ARNM_BVEC_MAYBE_UNUSED static inline void name##_free(arnm_bvec *v) {                            \
    arnm_bvec_free(v);                                                                             \
  }                                                                                                \
                                                                                                   \
  /** Cold path of @c _emplace: open the next bucket, reusing an already allocated one. */         \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_grow(arnm_bvec *v, type **out_slot) {    \
    return arnm_bvec_grow(v, (void **)out_slot);                                                   \
  }                                                                                                \
                                                                                                   \
  /** Claim the next slot without writing it -- construct large payloads in place. */              \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_emplace(arnm_bvec *v, type **out_slot) { \
    return arnm_bvec_emplace(v, (void **)out_slot);                                                \
  }                                                                                                \
                                                                                                   \
  /** Append a value. */                                                                           \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_push(arnm_bvec *v, type value) {         \
    type *slot;                                                                                    \
    arnm_result result = name##_emplace(v, &slot);                                                 \
    if (result != ARNM_SUCCESS) return result;                                                     \
    *slot = value;                                                                                 \
    return ARNM_SUCCESS;                                                                           \
  }                                                                                                \
                                                                                                   \
  /** Append a value read through a pointer -- avoids passing bulky payloads by value.         */  \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_push_ptr(                                \
      arnm_bvec *v, const type *value                                                              \
  ) {                                                                                              \
    return arnm_bvec_push_ptr(v, value);                                                           \
  }                                                                                                \
                                                                                                   \
  /** Remove the last element. The vacated bucket stays allocated for the next push.     */        \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_pop(arnm_bvec *v) {                      \
    return arnm_bvec_pop(v);                                                                       \
  }                                                                                                \
                                                                                                   \
  /** Number of elements currently held. */                                                        \
  ARNM_BVEC_MAYBE_UNUSED static inline uint32_t name##_size(const arnm_bvec *v) {                  \
    return arnm_bvec_size(v);                                                                      \
  }                                                                                                \
                                                                                                   \
  /** Unchecked access -- @p index must be < size. */                                              \
  ARNM_BVEC_MAYBE_UNUSED static inline type *name##_get(const arnm_bvec *v, uint32_t index) {      \
    return (type *)arnm_bvec_get(v, index);                                                        \
  }                                                                                                \
                                                                                                   \
  /** Bounds-checked access; NULL when @p index has no element. */                                 \
  ARNM_BVEC_MAYBE_UNUSED static inline type *name##_at(const arnm_bvec *v, uint32_t index) {       \
    return (type *)arnm_bvec_at(v, index);                                                         \
  }                                                                                                \
                                                                                                   \
  /** First element, or NULL while empty. */                                                       \
  ARNM_BVEC_MAYBE_UNUSED static inline type *name##_front(const arnm_bvec *v) {                    \
    return (type *)arnm_bvec_front(v);                                                             \
  }                                                                                                \
                                                                                                   \
  /** Last element, or NULL while empty. */                                                        \
  ARNM_BVEC_MAYBE_UNUSED static inline type *name##_back(const arnm_bvec *v) {                     \
    return (type *)arnm_bvec_back(v);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Number of buckets holding elements -- the outer bound for bucket-wise iteration. */          \
  ARNM_BVEC_MAYBE_UNUSED static inline uint16_t name##_bucket_count(const arnm_bvec *v) {          \
    return arnm_bvec_bucket_count(v);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Contiguous start of bucket @p bucket; @p bucket must be < _bucket_count(). */                \
  ARNM_BVEC_MAYBE_UNUSED static inline type *name##_bucket_data(                                   \
      const arnm_bvec *v, uint16_t bucket                                                          \
  ) {                                                                                              \
    return (type *)arnm_bvec_bucket_data(v, bucket);                                               \
  }                                                                                                \
                                                                                                   \
  /** Elements held in bucket @p bucket; full except possibly the last one. */                     \
  ARNM_BVEC_MAYBE_UNUSED static inline uint32_t name##_bucket_size(                                \
      const arnm_bvec *v, uint16_t bucket                                                          \
  ) {                                                                                              \
    return arnm_bvec_bucket_size(v, bucket);                                                       \
  }                                                                                                \
                                                                                                   \
  /** Flatten the sequence into one contiguous array, bucket by bucket. */                         \
  ARNM_BVEC_MAYBE_UNUSED static inline arnm_result name##_copy_to(                                 \
      const arnm_bvec *v, type *dst, uint32_t dst_capacity                                         \
  ) {                                                                                              \
    return arnm_bvec_copy_to(v, dst, dst_capacity);                                                \
  }

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif // ARNM_BUCKET_VECTOR_H
