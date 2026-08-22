#include "arnm/bucket_vector.h"

#include "arnm/memory.h"
#include "arnm/result.h"
#include <stdint.h>

static inline uint32_t bucket_bytes(size_t type_size, uint8_t bucket_capacity_log2) {
  return ((uint32_t)(type_size << bucket_capacity_log2));
}

static inline uint32_t bucket_capacity(uint8_t bucket_capacity_log2) {
  return (uint32_t)1 << bucket_capacity_log2;
}

static inline uint32_t bucket_mask(uint8_t bucket_capacity_log2) {
  return bucket_capacity(bucket_capacity_log2) - 1;
}

static inline uint16_t bucket_count_allocated(const arnm_bvec *v) {
  if (!v->buckets) { return 0; }
  uint16_t bucket_count = arnm_bvec_bucket_count(v);
  while (bucket_count < v->bucket_capacity && v->buckets[bucket_count]) { ++bucket_count; }
  return bucket_count;
}

/** Slots to bytes. The callers keep capacities inside the bound checked in _index_grow. */
static inline uint32_t index_bytes(uint16_t capacity) {
  return (uint32_t)((uint32_t)capacity * sizeof(void *));
}

static inline bool is_state_valid(const arnm_bvec *v) {
  uint16_t bucket_count = bucket_count_allocated(v);
  // empty buckets
  if (!v->buckets) {
    if (bucket_count || v->bucket_capacity || v->tail_index || v->tail || v->size) { return false; }
    return true;
  } else {
    if (!v->bucket_capacity) return false;
  }
  if (bucket_count > v->bucket_capacity) return false;
  if (v->tail_index >= bucket_count) return false;
  if (!v->tail && v->tail_index) return false;
  if (v->size > bucket_count * bucket_capacity(v->bucket_capacity_max_log2)) return false;

  return true;
}

arnm_result arnm_bvec_init(
    arnm_bvec *v,
    uint8_t bucket_capacity_log2,
    uint8_t index_grow_step_size,
    size_t element_size,
    arnm *allocator
) {
  if (!v) return ARNM_ERROR_NULL_POINTER;
  if (!element_size || !bucket_capacity_log2 || bucket_capacity_log2 > 16 ||
      element_size > (size_t)(UINT32_MAX >> (bucket_capacity_log2)) || element_size > UINT16_MAX)
    return ARNM_ERROR_INVALID_PARAM;

  v->allocator = allocator;
  v->buckets = NULL;
  v->element_size = element_size;
  v->bucket_capacity = 0;
  v->tail_index = 0;
  /* no bucket is open, so tail_used counts nothing: CAP is the one value that means "no */
  /* room here", the same a full tail carries, and it sends the first _emplace to _grow */
  v->tail_used = bucket_capacity(bucket_capacity_log2);
  v->tail = NULL;
  v->size = 0;
  v->bucket_capacity_max_log2 = bucket_capacity_log2;
  v->index_grow_step_size =
      index_grow_step_size ? index_grow_step_size : ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE;

  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_reserve(arnm_bvec *v, uint32_t element_count) {
  if (!v) return ARNM_ERROR_NULL_POINTER;
  if (!element_count) return ARNM_ERROR_INVALID_PARAM;
  if (!is_state_valid(v)) return ARNM_ERROR_INVALID_STATE;

  /* one bound for two demands: the whole payload stays addressable in the allocator's */
  /* uint32_t, and rounding up to whole buckets cannot wrap */
  if (element_count > (UINT32_MAX - bucket_mask(v->bucket_capacity_max_log2)) / v->element_size) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }
  uint16_t needed =
      (element_count + bucket_mask(v->bucket_capacity_max_log2)) >> v->bucket_capacity_max_log2;
  const uint16_t previous_bucket_count = bucket_count_allocated(v);
  if (needed <= previous_bucket_count) return ARNM_SUCCESS;
  if (needed > v->bucket_capacity) {
    const uint32_t old_capacity = index_bytes(v->bucket_capacity);
    const uint32_t new_capacity = index_bytes(needed);
    if (new_capacity > ARNM_BVEC_MAX_INDEX_CAPACITY) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    arnm_result result =
        arnm_realloc((uint8_t **)v->buckets, old_capacity, new_capacity, v->allocator);
    if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) {
      return result;
    }
    // set new index slots to NULL
    memset(v->buckets + old_capacity, 0, new_capacity);
    v->bucket_capacity = needed;
  }
  uint16_t bucket_count = previous_bucket_count;
  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  while (bucket_count < needed) {
    uint8_t *bucket = NULL;
    arnm_result result = arnm_alloc(&bucket, bucket_size, v->allocator);
    if (ARNM_SUCCESS != result) return result;
    v->buckets[bucket_count++] = bucket;
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_shrink(arnm_bvec *v) {
  if (!v) return ARNM_ERROR_NULL_POINTER;
  if (!v->buckets) return ARNM_SUCCESS;
  if (!is_state_valid(v)) return ARNM_ERROR_INVALID_STATE;

  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  const uint32_t used_before = arnm_bvec_bucket_count(v);
  /* top down, so an arena unwinds in allocation order; it stops at the first bucket it   */
  /* refuses to reclaim, and nothing is lost when it does -- that block is simply kept.    */
  uint32_t i = bucket_count_allocated(v);
  for (; i > used_before; --i) {
    if (!v->buckets[i - 1]) continue;
    if (ARNM_SUCCESS != arnm_free((uint8_t *)v->buckets[i - 1], bucket_size, v->allocator)) break;
    v->buckets[i - 1] = NULL;
  }

  if (!i) {
    /* the index array leaves the descriptor only when it really came back; a refusal  */
    /* keeps address and allocated size, so a later _shrink can hand it back instead   */
    /* of stranding it                                                                 */
    if (ARNM_SUCCESS ==
        arnm_free((uint8_t *)v->buckets, index_bytes(v->bucket_capacity), v->allocator)) {
      v->buckets = NULL;
      v->bucket_capacity = 0;
    }
    return ARNM_SUCCESS;
  }
  if (i == v->bucket_capacity) return ARNM_SUCCESS;
  const uint32_t old_capacity = index_bytes(v->bucket_capacity);
  const uint32_t new_capacity = index_bytes(i);
  /* a refused tightening costs only unused pointer slots, so it is not an error */
  arnm_result result =
      arnm_realloc((uint8_t **)&v->buckets, old_capacity, new_capacity, v->allocator);
  if (ARNM_SUCCESS == result) {
    v->bucket_capacity = i;
  } else if (ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) {
    return result;
  }
  return ARNM_SUCCESS;
}

void arnm_bvec_clear(arnm_bvec *v) {
  v->tail = NULL;
  v->tail_index = 0;
  v->tail_used = bucket_capacity(v->bucket_capacity_max_log2); /* no bucket open, as after _init */
  v->size = 0;
}

void arnm_bvec_free(arnm_bvec *v) {
  if (!v || !v->buckets || !v->element_size) return;
  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  /* buckets backwards then the index array, the reverse of how they were taken */
  uint32_t i = bucket_count_allocated(v);
  for (; i > 0; --i) {
    if (!v->buckets[i - 1]) continue;
    arnm_free((uint8_t *)v->buckets[i - 1], bucket_size, v->allocator);
  }
  arnm_free((uint8_t *)v->buckets, index_bytes(v->bucket_capacity), v->allocator);
  /* the empty state is what _init writes, and the allocator stays attached */
  (void)arnm_bvec_init(
      v, v->bucket_capacity_max_log2, v->index_grow_step_size, v->element_size, v->allocator
  );
}

arnm_result arnm_bvec_grow(arnm_bvec *v, void **out_slot) {
  if (!v || !out_slot) return ARNM_ERROR_NULL_POINTER;
  if (!is_state_valid(v)) return ARNM_ERROR_INVALID_STATE;

  /* the buckets in use are exactly the ones behind us; the next one starts where they end */
  uint32_t next = arnm_bvec_bucket_count(v);
  const uint16_t previous_bucket_count = bucket_count_allocated(v);
  uint16_t bucket_count = previous_bucket_count;
  if (next >= previous_bucket_count) {
    if (previous_bucket_count == v->bucket_capacity) {
      uint32_t new_capacity = v->bucket_capacity + v->index_grow_step_size;
      if (new_capacity > ARNM_BVEC_MAX_INDEX_CAPACITY) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
      arnm_result result = arnm_realloc(
          (uint8_t **)index, index_bytes(v->bucket_capacity), index_bytes(new_capacity),
          v->allocator
      );
      if (result != ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED && result != ARNM_SUCCESS)
        return result;
      v->bucket_capacity = new_capacity;
    }
    // if (ARNM_SUCCESS != arnm_alloc(&buffer, size, allocator)) return NULL;
    void *bucket = NULL;
    arnm_result result = arnm_alloc(
        (uint8_t **)&bucket, bucket_bytes(v->element_size, v->bucket_capacity_max_log2),
        v->allocator
    );
    if (result != ARNM_SUCCESS) return result;
    v->buckets[bucket_count++] = bucket;
  }
  v->tail = v->buckets[next];
  v->tail_index = next;
  v->tail_used = 1;
  v->size++;
  *out_slot = v->tail;
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_emplace(arnm_bvec *v, void **out_slot) {
  if (v->tail && v->tail_used < bucket_capacity(v->bucket_capacity_max_log2)) {
    *out_slot = v->tail + v->tail_used++;
    v->size++;
    return ARNM_SUCCESS;
  }
  return arnm_bvec_grow(v, out_slot);
}

arnm_result arnm_bvec_push_ptr(arnm_bvec *v, const void *value) {
  void *slot;
  arnm_result result = arnm_bvec_emplace(v, &slot);
  if (result != ARNM_SUCCESS) return result;
  memcpy(slot, value, v->element_size);
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_pop(arnm_bvec *v) {
  if (!v->size) return ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS;
  v->size--;
  if (--v->tail_used == 0) {
    if (v->size) {
      v->tail_index--;
      v->tail = v->buckets[v->tail_index];
    } else {
      v->tail = NULL;
      v->tail_index = 0;
    }
    /* the bucket we stepped back into is full by definition; with none left it is the */
    /* same value standing for "no bucket open", so tail_used is never 0 between calls */
    v->tail_used = bucket_capacity(v->bucket_capacity_max_log2);
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_copy_to(const arnm_bvec *v, void *dst, uint32_t dst_capacity) {
  uint32_t bucket, buckets, written = 0;
  if (!v || !dst) return ARNM_ERROR_NULL_POINTER;
  if (dst_capacity < v->size) return ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL;
  buckets = arnm_bvec_bucket_count(v);
  for (bucket = 0; bucket < buckets; ++bucket) {
    uint32_t count = arnm_bvec_bucket_size(v, bucket);
    memcpy(dst + written, v->buckets[bucket], count * v->element_size);
    written += count;
  }
  return ARNM_SUCCESS;
}
