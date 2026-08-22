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

/** @brief Initial number of bucket pointer slots in the index array. */
#define ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE 8

/**
 * @brief Largest index array the allocator can hand out, counted in bucket pointer slots.
 */
#define ARNM_BVEC_MAX_INDEX_CAPACITY ((uint16_t)((UINT16_MAX - 7u) / sizeof(void *)))


typedef struct arnm_bvec {
  arnm *allocator;
  void **buckets;           /**< Index array: one pointer per allocated bucket. */
  uint16_t bucket_count;    /**< Buckets allocated (may exceed the buckets in use). */
  uint16_t bucket_capacity; /**< Pointer slots available in @c buckets. */
  uint16_t tail_index;      /**< Index of @c tail within @c buckets. */
  uint16_t tail_used;       /**< Slots in @c tail; BUCKET_CAPACITY when full or @c tail NULL. */
  void *tail;               /**< Bucket currently being filled; NULL while empty. */
  uint32_t size;            /**< Total element count. */
  uint8_t bucket_capacity_max_log2;
  uint8_t index_grow_step_size;
} arnm_bvec;

arnm_result arnm_bvec_init(arnm_bvec *v, uint8_t bucket_capacity_log2, uint8_t index_grow_step_size, size_t type_size, arnm *allocator);

arnm_result arnm_bvec_reserve(
  arnm_bvec* bvec,
  uint32_t element_count,
  size_t type_size
);

arnm_result arnm_bvec_shrink(arnm_bvec *v, size_t type_size);

void arnm_bvec_clear(arnm_bvec *v);

void arnm_bvec_free(arnm_bvec *v, size_t type_size);

arnm_result arnm_bvec_grow(arnm_bvec *v, size_t type_size, void **out_slot);

arnm_result arnm_bvec_emplace(arnm_bvec *v, size_t type_size, void **out_slot);

arnm_result arnm_bvec_push_ptr(arnm_bvec *v, size_t type_size, const void *value);

arnm_result arnm_bvec_pop(arnm_bvec *v);

static inline uint32_t arnm_bvec_size(const arnm_bvec *v) {                                                      \
  return v->size;                                                                                \
}

static inline void *arnm_bvec_get(const arnm_bvec *v, uint32_t index) {
  const uint8_t log2_bucket_capacity = v->bucket_capacity_max_log2;
  const uint8_t buckt_index = index >> (uint32_t)(log2_bucket_capacity);
  const uint32_t index_in_bucket = index & (((uint32_t)1 << log2_bucket_capacity) - 1);
  return v->buckets[buckt_index] + index_in_bucket;
}

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
  return v->tail ? v->tail + v->tail_used - 1 : NULL;
}

/** Number of buckets holding elements -- the outer bound for bucket-wise iteration. */
static inline uint32_t arnm_bvec_bucket_count(const arnm_bvec *v) {
  return v->tail ? v->tail_index + 1 : 0;
}

/** Contiguous start of bucket @p bucket; @p bucket must be < _bucket_count(). */
static inline void *arnm_bvec_bucket_data(const arnm_bvec *v, uint32_t bucket) {
  return v->buckets[bucket];
}

/** Elements held in bucket @p bucket; full except possibly the last one. */
static inline uint32_t arnm_bvec_bucket_size(const arnm_bvec *v, uint32_t bucket) {
  return bucket == v->tail_index ? v->tail_used : (uint32_t)1 << v->bucket_capacity_max_log2;
}

arnm_result arnm_bvec_copy_to(const arnm_bvec *v, size_t type_size, void *dst, uint32_t dst_capacity);

#define ARNM_BVEC_DECLARE(name, type, scope)                                                       \
  scope arnm_result name##_init(arnm_bvec *v, uint8_t bucket_capacity_log2, uint8_t index_grow_step_size, arnm *allocator);                                         \
  scope arnm_result name##_reserve(arnm_bvec *v, uint32_t element_count);                               \
  scope arnm_result name##_shrink(arnm_bvec *v);                                                        \
  scope void name##_clear(arnm_bvec *v);                                                                \
  scope void name##_free(arnm_bvec *v);                                                                 \
  scope arnm_result name##_grow(arnm_bvec *v, type **out_slot);                                         \
  scope arnm_result name##_emplace(arnm_bvec *v, type **out_slot);                                      \
  scope arnm_result name##_push(arnm_bvec *v, type value);                                              \
  scope arnm_result name##_push_ptr(arnm_bvec *v, const type *value);                                   \
  scope arnm_result name##_pop(arnm_bvec *v);                                                           \
  scope uint32_t name##_size(const arnm_bvec *v);                                                       \
  scope type *name##_get(const arnm_bvec *v, uint32_t index);                                           \
  scope type *name##_at(const arnm_bvec *v, uint32_t index);                                            \
  scope type *name##_front(const arnm_bvec *v);                                                         \
  scope type *name##_back(const arnm_bvec *v);                                                          \
  scope uint32_t name##_bucket_count(const arnm_bvec *v);                                               \
  scope type *name##_bucket_data(const arnm_bvec *v, uint32_t bucket);                                  \
  scope uint32_t name##_bucket_size(const arnm_bvec *v, uint32_t bucket);                               \
  scope arnm_result name##_copy_to(const arnm_bvec *v, type *dst, uint32_t dst_capacity);

#define ARNM_BVEC_DEFINE(name, type, scope)                                                        \
  /** Prepare an empty vector. Allocates nothing; the first push opens the first bucket. */        \
  scope arnm_result name##_init(arnm_bvec *v, uint8_t bucket_capacity_log2, uint8_t index_grow_step_size, arnm *allocator) {          \
    return arnm_bvec_init(v, bucket_capacity_log2, index_grow_step_size, sizeof(type), allocator);                       \
  }                                                                                                \
                                                                                                   \
  /** Allocate buckets and index slots for @p element_count elements ahead of time. */             \
  scope arnm_result name##_reserve(arnm_bvec *v, uint32_t element_count) {                              \
    return arnm_bvec_reserve(v, element_count, sizeof(type));                                      \                                                                         \
  }                                                                                                \
                                                                                                   \
  /** Release every bucket that holds no element and tighten the index array onto the rest. */     \
  scope arnm_result name##_shrink(arnm_bvec *v) {                                                       \
    return arnm_bvec_shrink(v, sizeof(type));                                                                             \                                                                        \
  }                                                                                                \
                                                                                                   \
  /** Drop all elements, keep every allocated bucket for immediate reuse. O(1). */                 \
  scope void name##_clear(arnm_bvec *v) {                                                               \
    arnm_bvec_clear(v);                                                                            \
  }                                                                                                \
                                                                                                   \
  /** Release every bucket and the index array, leaving a zeroed, reusable descriptor.   */        \
  scope void name##_free(arnm_bvec *v) {                                                                \
    arnm_bvec_free(v, sizeof(type));                                                               \
  }                                                                                                \
                                                                                                   \
  /** Cold path of @c _emplace: open the next bucket, reusing an already allocated one. */         \
  scope arnm_result name##_grow(arnm_bvec *v, type **out_slot) {                                        \
    return arnm_bvec_grow(v, sizeof(type), out_slot);                                              \
  }                                                                                                \
                                                                                                   \
  /** Claim the next slot without writing it -- construct large payloads in place. */              \
  scope arnm_result name##_emplace(arnm_bvec *v, type **out_slot) {                                     \
    return arnm_bvec_emplace(v, sizeof(type), out_slot);                                           \
  }                                                                                                \
                                                                                                   \
  /** Append a value. */                                                                           \
  scope arnm_result name##_push(arnm_bvec *v, type value) {                                             \
    type *slot;                                                                                    \
    arnm_result result = name##_emplace(v, &slot);                                                 \
    if (result != ARNM_SUCCESS) return result;                                                     \
    *slot = value;                                                                                 \
    return ARNM_SUCCESS;                                                                           \
  }                                                                                                \
                                                                                                   \
  /** Append a value read through a pointer -- avoids passing bulky payloads by value.         */  \
  scope arnm_result name##_push_ptr(arnm_bvec *v, const type *value) {                                  \
    return arnm_bvec_push_ptr(v, sizeof(type), value);                                             \
  }                                                                                                \
                                                                                                   \
  /** Remove the last element. The vacated bucket stays allocated for the next push.     */        \
  scope arnm_result name##_pop(arnm_bvec *v) {                                                          \
    return arnm_bvec_pop(v);                                                                       \
  }                                                                                                \
                                                                                                   \
  /** Number of elements currently held. */                                                        \
  scope uint32_t name##_size(const arnm_bvec *v) {                                                      \
    return arnm_bvec_size(v);                                                                      \
  }                                                                                                \
                                                                                                   \
  /** Unchecked access -- @p index must be < size. */                                              \
  scope type *name##_get(const arnm_bvec *v, uint32_t index) {                                          \
    return arnm_bvec_get(v, index);                                                                \
  }                                                                                                \
                                                                                                   \
  /** Bounds-checked access; NULL when @p index has no element. */                                 \
  scope type *name##_at(const arnm_bvec *v, uint32_t index) {                                           \
    return arnm_bvec_at(v, index);                                                                 \
  }                                                                                                \
                                                                                                   \
  /** First element, or NULL while empty. */                                                       \
  scope type *name##_front(const arnm_bvec *v) {                                                        \
    return arnm_bvec_front(v);                                                                     \
  }                                                                                                \
                                                                                                   \
  /** Last element, or NULL while empty. */                                                        \
  scope type *name##_back(const arnm_bvec *v) {                                                         \
    return arnm_bvec_back(v);                                                                      \
  }                                                                                                \
                                                                                                   \
  /** Number of buckets holding elements -- the outer bound for bucket-wise iteration. */          \
  scope uint32_t name##_bucket_count(const arnm_bvec *v) {                                              \
    return arnm_bvec_bucket_count(v);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Contiguous start of bucket @p bucket; @p bucket must be < _bucket_count(). */                \
  scope type *name##_bucket_data(const arnm_bvec *v, uint32_t bucket) {                                 \
    return arnm_bvec_bucket_data(v, bucket);                                                       \
  }                                                                                                \
                                                                                                   \
  /** Elements held in bucket @p bucket; full except possibly the last one. */                     \
  scope uint32_t name##_bucket_size(const arnm_bvec *v, uint32_t bucket) {                              \
    return arnm_bvec_bucket_size(v, bucket);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Flatten the sequence into one contiguous array, bucket by bucket. */                         \
  scope arnm_result name##_copy_to(const arnm_bvec *v, type *dst, uint32_t dst_capacity) {              \
    return arnm_bvec_copy_to(v, sizeof(type), dst, dst_capacity);                                  \
  }

#define ARNM_BVEC_STATIC(name, type)                                                  \
  ARNM_BVEC_DECLARE(name, type, static inline)                                        \
  ARNM_BVEC_DEFINE(name, type, static inline)


#define ARNM_BVEC_FOREACH(name, vec, item_ptr, index_var)                                          \
  for (uint32_t index_var = 0;                                                                     \
       index_var < (vec)->size && (((item_ptr) = name##_get((vec), index_var)), 1); ++index_var)

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif // ARNM_BUCKET_VECTOR_H
