#ifndef ARNM_ROARING_INTERN_H
#define ARNM_ROARING_INTERN_H

/*
 * What the three roaring modules share: the block sizes, the reads into a container, the
 * searches, and the source that hands a set out key by key. Not installed and no part of the
 * interface -- arnm/roaring_bitmap.h, arnm/roaring_ops.h and arnm/roaring_query.h are.
 *
 * Everything here carries the arnm_roaring_ prefix although it is private: it crosses
 * translation units, so it needs a name nobody else takes.
 */

#include "arnm/bitmap.h"
#include "arnm/graded_block_pool.h"
#include "arnm/result.h"
#include "arnm/roaring_bitmap.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROARING_BITMAP_BYTES (ARNM_ROARING_BITMAP_WORDS * sizeof(uint64_t))
#define ROARING_BITMAP_LOG2 13u         /* 8 KiB, a bitmap container */
#define ROARING_ARRAY_LOG2_FIRST 4u     /* 16 bytes, an array of 8 values */
#define ROARING_DIRECTORY_LOG2_FIRST 4u /* 16 bytes, one container entry */
#define ROARING_SPARSE_LOG2_FIRST 4u    /* 16 bytes, four values */

static_assert(
    sizeof(arnm_roaring_container) == (1u << ROARING_DIRECTORY_LOG2_FIRST),
    "a container entry is 16 bytes, the directory's first block holds exactly one"
);
static_assert(
    ARNM_ROARING_SPARSE_MAX < ARNM_ROARING_ARRAY_MAX,
    "a sparse set turned into containers has arrays only, never a bitmap"
);
static_assert(
    ARNM_ROARING_ARRAY_MAX * sizeof(uint16_t) == (1u << ROARING_BITMAP_LOG2),
    "a full array takes the same 8 KiB as a bitmap, so it turns into one instead of growing"
);

// ********** blocks *******************

/* by exponent: every block here is a power of two the set already knows, nothing to classify */
static inline arnm_result arnm_roaring_block_alloc(
    arnm_graded_block_pool *pool, uint8_t log2, uint8_t **out
) {
  return arnm_graded_block_pool_alloc_log2(pool, out, log2);
}

static inline void arnm_roaring_block_free(
    arnm_graded_block_pool *pool, uint8_t *block, uint8_t log2
) {
  // cannot be refused: the exponent is the one the block was taken with
  (void)arnm_graded_block_pool_free_log2(pool, block, log2);
}

/** Smallest log2 of a block that holds @p bytes, never below the first array block. */
static inline uint8_t arnm_roaring_log2_for_bytes(uint32_t bytes) {
  uint8_t log2 = ROARING_ARRAY_LOG2_FIRST;
  while (((uint32_t)1u << log2) < bytes) { ++log2; }
  return log2;
}

// ********** small helpers *******************

static inline uint16_t *arnm_roaring_array_of(const arnm_roaring_container *container) {
  return (uint16_t *)(void *)container->data;
}

static inline uint64_t *arnm_roaring_words_of(const arnm_roaring_container *container) {
  return (uint64_t *)(void *)container->data;
}

/*
 * A third kind of container that only ever exists as a view while a union is read: the values
 * of one key of a sparse set, read in place as uint32_t, the low part of each being the array
 * value. Never stored, never handed to the set operations; see key_source.
 */
#define ROARING_KIND_WIDE 2u

static inline const uint32_t *arnm_roaring_wide_of(const arnm_roaring_container *container) {
  return (const uint32_t *)(const void *)container->data;
}

static inline uint16_t arnm_roaring_key_of(uint32_t value) {
  return (uint16_t)(value >> 16);
}

static inline uint16_t arnm_roaring_low_of(uint32_t value) {
  return (uint16_t)(value & 0xffffu);
}

static inline uint32_t arnm_roaring_value_of(uint16_t key, uint32_t low) {
  return ((uint32_t)key << 16) | low;
}

/** Bits of word @p low / 64 from @p low upwards. */
static inline uint64_t arnm_roaring_first_word_mask(uint16_t low) {
  return ~0ull << (low & 63u);
}

/** Bits of word @p high / 64 up to and including @p high. */
static inline uint64_t arnm_roaring_last_word_mask(uint16_t high) {
  return ~0ull >> (63u - (high & 63u));
}

/** First index with array[i] >= value. */
static inline uint32_t arnm_roaring_lower_bound16(
    const uint16_t *array, uint32_t count, uint16_t value
) {
  // a bound at or before the first value -- a range that starts at the edge -- needs no search
  if (!count || value <= array[0]) { return 0; }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = (low + high) >> 1;
    if (array[middle] < value) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

/** First index with array[i] > value. */
static inline uint32_t arnm_roaring_upper_bound16(
    const uint16_t *array, uint32_t count, uint16_t value
) {
  if (!count || value >= array[count - 1u]) { return count; }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = (low + high) >> 1;
    if (array[middle] <= value) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

/** First index with values[i] >= value. */
static inline uint32_t arnm_roaring_lower_bound32(
    const uint32_t *values, uint32_t count, uint32_t value
) {
  if (!count || value <= values[0]) { return 0; }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = (low + high) >> 1;
    if (values[middle] < value) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

/** First index with values[i] > value. */
static inline uint32_t arnm_roaring_upper_bound32(
    const uint32_t *values, uint32_t count, uint32_t value
) {
  if (!count || value >= values[count - 1u]) { return count; }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = (low + high) >> 1;
    if (values[middle] <= value) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

/** No containers: one sorted array of values, or nothing at all. */
static inline bool arnm_roaring_is_sparse(const arnm_roaring_bitmap *set) {
  return 0u == set->count;
}

/** First container whose key is >= @p key. */
static inline uint32_t arnm_roaring_first_container(const arnm_roaring_bitmap *set, uint16_t key) {
  if (!set->count || key <= set->containers[0].key) { return 0; }
  uint32_t low = 0;
  uint32_t top = set->count;
  while (low < top) {
    const uint32_t middle = (low + top) >> 1;
    if (set->containers[middle].key < key) {
      low = middle + 1u;
    } else {
      top = middle;
    }
  }
  return low;
}

/** The part of `[min, max]` that falls into the container of @p key, as low parts. */
static inline void arnm_roaring_container_bounds(
    uint16_t key, uint32_t min, uint32_t max, uint16_t *low, uint16_t *high
) {
  *low = key == arnm_roaring_key_of(min) ? arnm_roaring_low_of(min) : 0u;
  *high = key == arnm_roaring_key_of(max) ? arnm_roaring_low_of(max) : 0xffffu;
}

/**
 * Set bits of @p words inside [low, high]: the two edge words masked, the ones between counted
 * as they are, so the loop in the middle has no branch.
 */
static inline uint32_t arnm_roaring_words_cardinality(
    const uint64_t *words, uint16_t low, uint16_t high
) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  if (first == last) {
    return (uint32_t)arnm_popcountll(
        words[first] & arnm_roaring_first_word_mask(low) & arnm_roaring_last_word_mask(high)
    );
  }
  uint32_t count = (uint32_t)arnm_popcountll(words[first] & arnm_roaring_first_word_mask(low));
  for (uint32_t w = first + 1u; w < last; ++w) { count += (uint32_t)arnm_popcountll(words[w]); }
  return count + (uint32_t)arnm_popcountll(words[last] & arnm_roaring_last_word_mask(high));
}

/** Masks the edge words of a range built in @p words, then counts it. */
static inline uint32_t arnm_roaring_clip_and_count(uint64_t *words, uint16_t low, uint16_t high) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  words[first] &= arnm_roaring_first_word_mask(low);
  words[last] &= arnm_roaring_last_word_mask(high);
  uint32_t count = 0;
  for (uint32_t w = first; w <= last; ++w) { count += (uint32_t)arnm_popcountll(words[w]); }
  return count;
}

/** First container whose key is > @p key. */
static inline uint32_t arnm_roaring_container_after(const arnm_roaring_bitmap *set, uint16_t key) {
  // a page from the top starts past the last container, where CRoaring's iterator starts too
  if (!set->count || key >= set->containers[set->count - 1u].key) { return set->count; }
  uint32_t low = 0;
  uint32_t top = set->count;
  while (low < top) {
    const uint32_t middle = (low + top) >> 1;
    if (set->containers[middle].key <= key) {
      low = middle + 1u;
    } else {
      top = middle;
    }
  }
  return low;
}

/**
 * A set read key by key inside a range of keys, in one direction, one container at a time. With
 * containers that is the container itself; for a sparse set it is a view -- an array container
 * over the low parts of that key's values, copied into @c buffer -- so every algorithm that
 * takes containers takes a sparse set as well. The view lasts until the source moves on.
 */

// ********** what roaring_bitmap.c implements for the other two *******************

/** Values of @p container inside [low, high]; the whole key answers from the stored count. */
uint32_t arnm_roaring_container_range_cardinality(
    const arnm_roaring_container *container, uint16_t low, uint16_t high
);

/** Appends @p container to @p set's directory, doubling it when full. Unchanged on failure. */
arnm_result arnm_roaring_push_container(
    arnm_roaring_bitmap *set, const arnm_roaring_container *container, arnm_graded_block_pool *pool
);

/** Appends the values as one array container of @p key; nothing to append is a success. */
arnm_result arnm_roaring_emit_array(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint16_t *values,
    uint32_t count,
    arnm_graded_block_pool *pool
);

/** Appends the bits of [first, last] as one container of @p key, array or bitmap by their count. */
arnm_result arnm_roaring_emit_words(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint64_t *words,
    uint32_t first,
    uint32_t last,
    uint32_t cardinality,
    arnm_graded_block_pool *pool
);

/** Appends @p values, sorted, to @p out as containers, one array per key. */
arnm_result arnm_roaring_values_to_containers(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
);

/** @p out, empty, as the sparse or container set of @p values, sorted. */
arnm_result arnm_roaring_values_result(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
);

// ********** a set read key by key *******************

/** One set being walked key by key inside a range, whatever form it is in. */
typedef struct arnm_roaring_source {
  const arnm_roaring_bitmap *set; /**< The set read; never NULL while the source lives. */
  uint16_t min_key;               /**< First key of the range. */
  uint16_t max_key;               /**< Last key of the range. */
  bool descending;                /**< Walking from the largest key down. */
  /** containers: the current one ascending, one past it descending; sparse: the first value of
   *  the current key ascending, one past its last descending */
  uint32_t position;
  uint32_t next; /**< sparse: where the following key starts */
  /** sparse: where the view's low parts are copied to, ARNM_ROARING_SPARSE_MAX of them; NULL
   *  reads the values in place instead, as a wide view */
  uint16_t *buffer;
  const arnm_roaring_container *current; /**< NULL once the range is done */
  arnm_roaring_container view;           /**< What @c current points at for a sparse set. */
} arnm_roaring_source;

/** Positions @p source at the first key of [min, max] in walking order and loads its container. */
void arnm_roaring_source_begin(
    arnm_roaring_source *source,
    const arnm_roaring_bitmap *set,
    uint32_t min,
    uint32_t max,
    bool descending,
    uint16_t *buffer
);

/** Moves to the next key in walking order; @c current is NULL once the range is done. */
void arnm_roaring_source_advance(arnm_roaring_source *source);

#ifdef __cplusplus
}
#endif

#endif // ARNM_ROARING_INTERN_H
