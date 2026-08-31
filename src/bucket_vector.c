#include "arnm/bucket_vector.h"

#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/result.h"
#include <stdint.h>

static inline uint32_t bucket_bytes(size_t type_size, uint8_t bucket_capacity_log2) {
  return ((uint32_t)(type_size << bucket_capacity_log2));
}

/* _init caps the exponent at 15, so a full bucket always fits the uint16_t it is counted in */
static inline uint16_t bucket_capacity(uint8_t bucket_capacity_log2) {
  return (uint16_t)(1u << bucket_capacity_log2);
}

static inline uint32_t bucket_mask(uint8_t bucket_capacity_log2) {
  return (uint32_t)bucket_capacity(bucket_capacity_log2) - 1u;
}

/*
 * Which end to release the buckets from, and why the two allocators want different answers.
 *
 * An arena takes back only its most recent allocation, so it has to be unwound newest first --
 * anything else hands it a block that is not at its tail and it keeps every one of them until
 * the next reset. That is not a preference, it is the only order that returns anything.
 *
 * The host has no such rule and one of its own. glibc merges a freed chunk into the top chunk
 * when it is adjacent to it, and trims the heap with brk() once top passes the trim threshold
 * -- so releasing the newest block first walks the top of the heap downwards and pays a shrink
 * per bucket, with the pages faulted back in the next time they are used. Oldest first puts
 * every bucket in a bin instead, consolidates them as neighbours meet, and reaches the top
 * chunk once, at the end. Measured with 7813 buckets of 4 KiB: 32.1 ms newest first against
 * 2.2 ms oldest first, in a process where nothing sits above them.
 *
 * A vector that grew into its buckets is not exposed to that, because its index array was
 * reallocated last and sits above every bucket, shielding them from the top chunk. One that was
 * reserved has the index array below them and is fully exposed. Both are the same call and only
 * the history differs, which is exactly why the order is chosen here rather than left to it.
 */
static inline bool releases_newest_first(const arnm_bvec *v) {
  return arnm_is_arena(v->allocator);
}

/*
 * Buckets the vector holds, in use or merely reserved.
 *
 * A field read. It used to be a scan forward from the buckets in use, while the index slots
 * were non-empty, and that was O(1) for a vector growing into fresh slots -- the next one is
 * NULL and the walk ends at once -- and O(remaining) for one whose buckets all exist already.
 * _grow asks twice, once here and once through is_state_valid(), so a reserved vector paid
 * about `bucket_count^2` pointer loads across a fill: 61 M of them for the 7813 buckets of
 * bench_vector's 4 M element run, which measured as 12 ms of its 22 ms append.
 *
 * The count is therefore carried in the descriptor and written wherever the set of buckets
 * changes: _init, _reserve, _grow, _shrink and _free. The invariant it stands for is unchanged
 * -- slots below it hold a bucket, slots above it are NULL.
 */
static inline uint16_t bucket_count_allocated(const arnm_bvec *v) {
  return v->buckets ? v->allocated_count : (uint16_t)0;
}

/** Slots to bytes. The callers keep capacities inside the bound checked in _index_grow. */
static inline uint32_t index_bytes(uint16_t capacity) {
  return (uint32_t)((uint32_t)capacity * sizeof(void *));
}

static inline bool is_state_valid(const arnm_bvec *v) {
  /* What _init writes and nothing else can produce. A descriptor that never saw _init reads */
  /* as all zeroes, and _reserve divides by the element size -- so this has to be the first   */
  /* question asked, before any of the shape below is looked at.                              */
  if (!v->element_size || !v->bucket_capacity_max_log2) { return false; }
  uint16_t bucket_count = bucket_count_allocated(v);
  // empty buckets
  if (!v->buckets) {
    if (bucket_count || v->bucket_capacity || v->tail_index || v->tail || v->size) { return false; }
    return true;
  } else {
    if (!v->bucket_capacity) return false;
  }
  if (bucket_count > v->bucket_capacity) return false;
  /* tail_index only means something while a bucket is open; with none open it must read 0.  */
  /* _grow leaves exactly that behind when the index array grew but the bucket alloc failed. */
  if (v->tail ? v->tail_index >= bucket_count : v->tail_index != 0) return false;
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
  /* 15 and not 16: tail_used carries the full bucket capacity as its "no room here" value, */
  /* and 1 << 16 does not fit in the uint16_t it is stored in                                 */
  if (!element_size || !bucket_capacity_log2 || bucket_capacity_log2 > 15 ||
      element_size > (size_t)(UINT32_MAX >> (bucket_capacity_log2)) || element_size > UINT16_MAX)
    return ARNM_ERROR_INVALID_PARAM;

  v->allocator = allocator;
  v->buckets = NULL;
  v->element_size = (uint16_t)element_size; /* bounded by the UINT16_MAX check above */
  v->bucket_capacity = 0;
  v->tail_index = 0;
  /* no bucket is open, so tail_used counts nothing: CAP is the one value that means "no */
  /* room here", the same a full tail carries, and it sends the first _emplace to _grow */
  v->tail_used = bucket_capacity(bucket_capacity_log2);
  v->tail = NULL;
  v->size = 0;
  v->allocated_count = 0;
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
  /* uint32_t, and rounding up to whole buckets cannot wrap. is_state_valid above is what */
  /* makes the division safe -- it refuses a descriptor whose element size is still 0 */
  if (element_count > (UINT32_MAX - bucket_mask(v->bucket_capacity_max_log2)) / v->element_size) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }
  /* the count of whole buckets can outrun uint16_t, so it is bounded before it is narrowed */
  const uint32_t needed_buckets =
      (element_count + bucket_mask(v->bucket_capacity_max_log2)) >> v->bucket_capacity_max_log2;
  if (needed_buckets > ARNM_BVEC_MAX_INDEX_CAPACITY) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  const uint16_t needed = (uint16_t)needed_buckets;
  const uint16_t previous_bucket_count = bucket_count_allocated(v);
  if (needed <= previous_bucket_count) return ARNM_SUCCESS;
  if (needed > v->bucket_capacity) {
    const uint32_t old_capacity = index_bytes(v->bucket_capacity);
    const uint32_t new_capacity = index_bytes(needed);

    arnm_result result =
        arnm_realloc((uint8_t **)&v->buckets, old_capacity, new_capacity, v->allocator);
    if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) {
      return result;
    }
    // set new index slots to NULL
    memset((uint8_t *)v->buckets + old_capacity, 0, new_capacity - old_capacity);
    v->bucket_capacity = needed;
  }
  uint16_t bucket_count = previous_bucket_count;
  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  while (bucket_count < needed) {
    uint8_t *bucket = NULL;
    arnm_result result = arnm_alloc(&bucket, bucket_size, v->allocator);
    if (ARNM_SUCCESS != result) return result;
    v->buckets[bucket_count++] = bucket;
    /* inside the loop, not after it: a refusal returns above, and what was handed over by then
       stays with the vector and has to be counted */
    v->allocated_count = bucket_count;
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_shrink(arnm_bvec *v) {
  if (!v) return ARNM_ERROR_NULL_POINTER;
  if (!v->buckets) return ARNM_SUCCESS;
  if (!is_state_valid(v)) return ARNM_ERROR_INVALID_STATE;

  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  const uint16_t used_before = arnm_bvec_bucket_count(v);
  uint16_t i = bucket_count_allocated(v);
  if (releases_newest_first(v)) {
    /* top down, so an arena unwinds in allocation order; it stops at the first bucket it   */
    /* refuses to reclaim, and nothing is lost when it does -- that block is simply kept.    */
    for (; i > used_before; --i) {
      if (!v->buckets[i - 1]) continue;
      if (ARNM_SUCCESS != arnm_free((uint8_t *)v->buckets[i - 1], bucket_size, v->allocator)) {
        break;
      }
      v->buckets[i - 1] = NULL;
    }
  } else {
    /* oldest first on the host, for the reason at releases_newest_first(). There is no early
       exit to mirror: the one answer arnm_free() gives besides ARNM_SUCCESS is the arena
       warning, which is what the other branch is for, so this walk always reaches used_before. */
    for (uint16_t k = used_before; k < i; ++k) {
      if (!v->buckets[k]) continue;
      arnm_free((uint8_t *)v->buckets[k], bucket_size, v->allocator);
      v->buckets[k] = NULL;
    }
    i = used_before;
  }
  /* where the walk stopped is what the vector still holds, whether it ran out of buckets to
     release or met one the allocator would not take back */
  v->allocated_count = i;

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
  if (!v) return;
  v->tail = NULL;
  v->tail_index = 0;
  v->tail_used = bucket_capacity(v->bucket_capacity_max_log2); /* no bucket open, as after _init */
  v->size = 0;
}

void arnm_bvec_free(arnm_bvec *v) {
  if (!v || !v->buckets || !v->element_size) return;
  const uint32_t bucket_size = bucket_bytes(v->element_size, v->bucket_capacity_max_log2);
  const uint16_t count = bucket_count_allocated(v);
  /* every bucket goes either way; only the order differs, and why it does is above */
  if (releases_newest_first(v)) {
    for (uint16_t i = count; i > 0; --i) {
      if (!v->buckets[i - 1]) continue;
      arnm_free((uint8_t *)v->buckets[i - 1], bucket_size, v->allocator);
    }
  } else {
    for (uint16_t i = 0; i < count; ++i) {
      if (!v->buckets[i]) continue;
      arnm_free((uint8_t *)v->buckets[i], bucket_size, v->allocator);
    }
  }
  /* the index array last in both cases: it was taken last where it matters, and on the host it
     is what the freed buckets consolidate up to */
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
  uint16_t next = arnm_bvec_bucket_count(v);
  const uint16_t previous_bucket_count = bucket_count_allocated(v);
  uint16_t bucket_count = previous_bucket_count;
  if (next >= previous_bucket_count) {
    if (previous_bucket_count == v->bucket_capacity) {
      uint32_t new_capacity = v->bucket_capacity + v->index_grow_step_size;
      if (new_capacity > ARNM_BVEC_MAX_INDEX_CAPACITY) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
      const uint32_t old_index_bytes = index_bytes(v->bucket_capacity);
      const uint32_t new_index_bytes = index_bytes((uint16_t)new_capacity);
      arnm_result result =
          arnm_realloc((uint8_t **)&v->buckets, old_index_bytes, new_index_bytes, v->allocator);
      if (result != ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED && result != ARNM_SUCCESS)
        return result;
      /* above allocated_count every slot has to say "no bucket here": _shrink and _free walk
         them, and the count alone does not tell an empty slot from a stale pointer */
      memset((uint8_t *)v->buckets + old_index_bytes, 0, new_index_bytes - old_index_bytes);
      v->bucket_capacity = (uint16_t)new_capacity;
    }
    uint8_t *bucket = NULL;
    arnm_result result = arnm_alloc(
        &bucket, bucket_bytes(v->element_size, v->bucket_capacity_max_log2), v->allocator
    );
    if (result != ARNM_SUCCESS) return result;
    v->buckets[bucket_count++] = bucket;
    v->allocated_count = bucket_count;
  }
  v->tail = v->buckets[next];
  v->tail_index = next;
  v->tail_used = 1;
  v->size++;
  *out_slot = v->tail;
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_emplace(arnm_bvec *v, void **out_slot) {
  if (!v || !out_slot) return ARNM_ERROR_NULL_POINTER;
  if (v->tail && v->tail_used < bucket_capacity(v->bucket_capacity_max_log2)) {
    *out_slot = (uint8_t *)v->tail + (size_t)v->tail_used * v->element_size;
    v->tail_used++;
    v->size++;
    return ARNM_SUCCESS;
  }
  return arnm_bvec_grow(v, out_slot);
}

arnm_result arnm_bvec_push_ptr(arnm_bvec *v, const void *value) {
  if (!v || !value) return ARNM_ERROR_NULL_POINTER;
  void *slot;
  arnm_result result = arnm_bvec_emplace(v, &slot);
  if (result != ARNM_SUCCESS) return result;
  memcpy(slot, value, v->element_size);
  return ARNM_SUCCESS;
}

arnm_result arnm_bvec_pop(arnm_bvec *v) {
  if (!v) return ARNM_ERROR_NULL_POINTER;
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
  uint16_t bucket, buckets;
  uint32_t written = 0;
  if (!v || !dst) return ARNM_ERROR_NULL_POINTER;
  if (dst_capacity < v->size) return ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL;
  buckets = arnm_bvec_bucket_count(v);
  for (bucket = 0; bucket < buckets; ++bucket) {
    uint32_t count = arnm_bvec_bucket_size(v, bucket);
    memcpy(
        (uint8_t *)dst + (size_t)written * v->element_size, v->buckets[bucket],
        (size_t)count * v->element_size
    );
    written += count;
  }
  return ARNM_SUCCESS;
}
