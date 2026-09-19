#include "arnm/roaring_bitmap.h"

#include "arnm/bitmap.h"
#include "arnm/graded_block_pool.h"
#include "arnm/result.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Layout, with containers: a directory of 16 byte container entries, sorted by key, in one pool
 * block that doubles; each entry points at its own pool block with the values. Sparse: one pool
 * block of sorted uint32_t values that doubles, and nothing else. `count` tells the two apart --
 * a set with containers has at least one -- so a set being built in either form is never
 * mistaken for the other, whatever its cardinality says on the way.
 *
 * A sparse set never holds more than ARNM_ROARING_SPARSE_MAX values -- add turns it into
 * containers past that, and an operation builds a sparse result only up to it -- so the buffers
 * the fast paths below keep on the stack always fit one. The other way round is not promised: a
 * small result of an operation on containers keeps its containers, since turning it into an
 * array would cost an allocation and a copy on every query for no answer that differs.
 *
 * Every block is a power of two the set asks for itself -- 2^block_log2 or 2^directory_log2 --
 * and frees with the same exponent, so the pool always finds the same grade for both. A pool whose
 * smallest grade is larger than asked for hands out more room than the set uses; that costs
 * memory, never correctness.
 *
 * The set operations build each result container in a buffer on the stack first -- at most one
 * array of 4096 values and one bitmap, 16 KiB together -- and only then take a block of exactly
 * the size the result needs. A result can therefore never be left with a block it outgrew.
 */

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
static inline arnm_result block_alloc(arnm_graded_block_pool *pool, uint8_t log2, uint8_t **out) {
  return arnm_graded_block_pool_alloc_log2(pool, out, log2);
}

static inline void block_free(arnm_graded_block_pool *pool, uint8_t *block, uint8_t log2) {
  // cannot be refused: the exponent is the one the block was taken with
  (void)arnm_graded_block_pool_free_log2(pool, block, log2);
}

/** Smallest log2 of a block that holds @p bytes, never below the first array block. */
static inline uint8_t log2_for_bytes(uint32_t bytes) {
  uint8_t log2 = ROARING_ARRAY_LOG2_FIRST;
  while (((uint32_t)1u << log2) < bytes) { ++log2; }
  return log2;
}

// ********** small helpers *******************

static inline uint16_t *array_of(const arnm_roaring_container *container) {
  return (uint16_t *)(void *)container->data;
}

static inline uint64_t *words_of(const arnm_roaring_container *container) {
  return (uint64_t *)(void *)container->data;
}

/*
 * A third kind of container that only ever exists as a view while a union is read: the values
 * of one key of a sparse set, read in place as uint32_t, the low part of each being the array
 * value. Never stored, never handed to the set operations; see key_source.
 */
#define ROARING_KIND_WIDE 2u

static inline const uint32_t *wide_of(const arnm_roaring_container *container) {
  return (const uint32_t *)(const void *)container->data;
}

static inline uint16_t key_of(uint32_t value) {
  return (uint16_t)(value >> 16);
}

static inline uint16_t low_of(uint32_t value) {
  return (uint16_t)(value & 0xffffu);
}

static inline uint32_t value_of(uint16_t key, uint32_t low) {
  return ((uint32_t)key << 16) | low;
}

/** Bits of word @p low / 64 from @p low upwards. */
static inline uint64_t first_word_mask(uint16_t low) {
  return ~0ull << (low & 63u);
}

/** Bits of word @p high / 64 up to and including @p high. */
static inline uint64_t last_word_mask(uint16_t high) {
  return ~0ull >> (63u - (high & 63u));
}

/** First index with array[i] >= value. */
static uint32_t lower_bound16(const uint16_t *array, uint32_t count, uint16_t value) {
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
static uint32_t upper_bound16(const uint16_t *array, uint32_t count, uint16_t value) {
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
static uint32_t lower_bound32(const uint32_t *values, uint32_t count, uint32_t value) {
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
static uint32_t upper_bound32(const uint32_t *values, uint32_t count, uint32_t value) {
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
static inline bool is_sparse(const arnm_roaring_bitmap *set) {
  return 0u == set->count;
}

/** First container whose key is >= @p key. */
static uint32_t first_container(const arnm_roaring_bitmap *set, uint16_t key) {
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
static inline void container_bounds(
    uint16_t key, uint32_t min, uint32_t max, uint16_t *low, uint16_t *high
) {
  *low = key == key_of(min) ? low_of(min) : 0u;
  *high = key == key_of(max) ? low_of(max) : 0xffffu;
}

/**
 * Set bits of @p words inside [low, high]: the two edge words masked, the ones between counted
 * as they are, so the loop in the middle has no branch.
 */
static uint32_t words_cardinality(const uint64_t *words, uint16_t low, uint16_t high) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  if (first == last) {
    return (uint32_t)arnm_popcountll(words[first] & first_word_mask(low) & last_word_mask(high));
  }
  uint32_t count = (uint32_t)arnm_popcountll(words[first] & first_word_mask(low));
  for (uint32_t w = first + 1u; w < last; ++w) { count += (uint32_t)arnm_popcountll(words[w]); }
  return count + (uint32_t)arnm_popcountll(words[last] & last_word_mask(high));
}

/** Masks the edge words of a range built in @p words, then counts it. */
static uint32_t clip_and_count(uint64_t *words, uint16_t low, uint16_t high) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  words[first] &= first_word_mask(low);
  words[last] &= last_word_mask(high);
  uint32_t count = 0;
  for (uint32_t w = first; w <= last; ++w) { count += (uint32_t)arnm_popcountll(words[w]); }
  return count;
}

/** Word and bit of the @p rank-th set bit (0 based) of a bitmap container. */
static void words_select(
    const uint64_t *words, uint32_t rank, uint32_t *word_index, uint32_t *bit
) {
  uint32_t w = 0;
  for (;; ++w) {
    const uint32_t count = (uint32_t)arnm_popcountll(words[w]);
    if (rank < count) { break; }
    rank -= count;
  }
  uint64_t word = words[w];
  while (rank--) { word &= word - 1u; }
  *word_index = w;
  *bit = (uint32_t)arnm_ctzll(word);
}

// ********** building *******************

void arnm_roaring_init(arnm_roaring_bitmap *set) {
  if (!set) { return; }
  memset(set, 0, sizeof(*set));
}

void arnm_roaring_free(arnm_roaring_bitmap *set, arnm_graded_block_pool *pool) {
  if (!set) { return; }
  if (is_sparse(set)) {
    if (set->values && pool) { block_free(pool, (uint8_t *)set->values, set->directory_log2); }
  } else if (pool) {
    for (uint32_t i = 0; i < set->count; ++i) {
      block_free(pool, set->containers[i].data, set->containers[i].block_log2);
    }
    block_free(pool, (uint8_t *)set->containers, set->directory_log2);
  }
  memset(set, 0, sizeof(*set));
}

/** Append @p container to the directory, doubling it when full. The set is unchanged on failure. */
static arnm_result push_container(
    arnm_roaring_bitmap *set, const arnm_roaring_container *container, arnm_graded_block_pool *pool
) {
  const uint32_t capacity = set->containers ? ((uint32_t)1u << set->directory_log2) /
                                                  (uint32_t)sizeof(arnm_roaring_container)
                                            : 0u;
  if (set->count == capacity) {
    const uint8_t log2 = set->containers ? (uint8_t)(set->directory_log2 + 1u)
                                         : (uint8_t)ROARING_DIRECTORY_LOG2_FIRST;
    uint8_t *block = NULL;
    const arnm_result result = block_alloc(pool, log2, &block);
    if (ARNM_SUCCESS != result) { return result; }
    if (set->containers) {
      memcpy(block, set->containers, (size_t)set->count * sizeof(arnm_roaring_container));
      block_free(pool, (uint8_t *)set->containers, set->directory_log2);
    }
    set->containers = (arnm_roaring_container *)(void *)block;
    set->directory_log2 = log2;
  }
  set->containers[set->count++] = *container;
  return ARNM_SUCCESS;
}

/** A full array of 4096 values becomes a bitmap, @p low set in it as well. */
static arnm_result array_to_bitmap(
    arnm_roaring_container *last, uint16_t low, arnm_graded_block_pool *pool
) {
  uint8_t *block = NULL;
  const arnm_result result = block_alloc(pool, ROARING_BITMAP_LOG2, &block);
  if (ARNM_SUCCESS != result) { return result; }
  memset(block, 0, ROARING_BITMAP_BYTES);
  uint64_t *words = (uint64_t *)(void *)block;
  const uint16_t *array = array_of(last);
  for (uint32_t i = 0; i < last->cardinality; ++i) {
    words[array[i] >> 6] |= 1ull << (array[i] & 63u);
  }
  words[low >> 6] |= 1ull << (low & 63u);
  block_free(pool, last->data, last->block_log2);
  last->data = block;
  last->block_log2 = ROARING_BITMAP_LOG2;
  last->kind = ARNM_ROARING_BITMAP;
  return ARNM_SUCCESS;
}

/** A full array below 4096 values moves into a block twice the size. */
static arnm_result array_grow(arnm_roaring_container *last, arnm_graded_block_pool *pool) {
  uint8_t *block = NULL;
  const uint8_t log2 = (uint8_t)(last->block_log2 + 1u);
  const arnm_result result = block_alloc(pool, log2, &block);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(block, last->data, (size_t)last->cardinality * sizeof(uint16_t));
  block_free(pool, last->data, last->block_log2);
  last->data = block;
  last->block_log2 = log2;
  return ARNM_SUCCESS;
}

/** Appends to a sparse set below the limit, doubling its block when full. Unchanged on failure. */
static arnm_result sparse_add(
    arnm_roaring_bitmap *set, uint32_t value, arnm_graded_block_pool *pool
) {
  const uint32_t capacity =
      set->values ? ((uint32_t)1u << set->directory_log2) / (uint32_t)sizeof(uint32_t) : 0u;
  if (set->cardinality == capacity) {
    const uint8_t log2 =
        set->values ? (uint8_t)(set->directory_log2 + 1u) : (uint8_t)ROARING_SPARSE_LOG2_FIRST;
    uint8_t *block = NULL;
    const arnm_result result = block_alloc(pool, log2, &block);
    if (ARNM_SUCCESS != result) { return result; }
    if (set->values) {
      memcpy(block, set->values, (size_t)set->cardinality * sizeof(uint32_t));
      block_free(pool, (uint8_t *)set->values, set->directory_log2);
    }
    set->values = (uint32_t *)(void *)block;
    set->directory_log2 = log2;
  }
  set->values[set->cardinality++] = value;
  set->maximum = value;
  return ARNM_SUCCESS;
}

static arnm_result sparse_to_containers(arnm_roaring_bitmap *set, arnm_graded_block_pool *pool);

arnm_result arnm_roaring_add(
    arnm_roaring_bitmap *set, uint32_t value, arnm_graded_block_pool *pool
) {
  if (!set || !pool) { return ARNM_ERROR_NULL_POINTER; }
  if (set->cardinality) {
    if (value < set->maximum) { return ARNM_ERROR_INVALID_PARAM; }
    if (value == set->maximum) { return ARNM_SUCCESS; }
    if (UINT32_MAX == set->cardinality) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  }
  if (is_sparse(set)) {
    if (set->cardinality < ARNM_ROARING_SPARSE_MAX) { return sparse_add(set, value, pool); }
    // one past the limit: containers from here on, the value added to them below
    const arnm_result result = sparse_to_containers(set, pool);
    if (ARNM_SUCCESS != result) { return result; }
  }
  const uint16_t key = key_of(value);
  const uint16_t low = low_of(value);

  if (set->count && set->containers[set->count - 1u].key == key) {
    arnm_roaring_container *last = &set->containers[set->count - 1u];
    if (ARNM_ROARING_BITMAP == last->kind) {
      words_of(last)[low >> 6] |= 1ull << (low & 63u);
    } else {
      const uint32_t capacity = ((uint32_t)1u << last->block_log2) / (uint32_t)sizeof(uint16_t);
      if (last->cardinality == ARNM_ROARING_ARRAY_MAX) {
        const arnm_result result = array_to_bitmap(last, low, pool);
        if (ARNM_SUCCESS != result) { return result; }
      } else {
        if (last->cardinality == capacity) {
          const arnm_result result = array_grow(last, pool);
          if (ARNM_SUCCESS != result) { return result; }
        }
        array_of(last)[last->cardinality] = low;
      }
    }
    last->cardinality++;
  } else {
    arnm_roaring_container container = {0};
    arnm_result result = block_alloc(pool, ROARING_ARRAY_LOG2_FIRST, &container.data);
    if (ARNM_SUCCESS != result) { return result; }
    container.key = key;
    container.kind = ARNM_ROARING_ARRAY;
    container.block_log2 = ROARING_ARRAY_LOG2_FIRST;
    container.cardinality = 1;
    array_of(&container)[0] = low;
    result = push_container(set, &container, pool);
    if (ARNM_SUCCESS != result) {
      block_free(pool, container.data, container.block_log2);
      return result;
    }
  }
  set->cardinality++;
  set->maximum = value;
  return ARNM_SUCCESS;
}

// ********** result containers *******************

/*
 * Both emitters take values that are already the final content of one container and pick the
 * kind by count: an array up to 4096 values, a bitmap above. Nothing to emit is a success.
 */

static arnm_result emit_array(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint16_t *values,
    uint32_t count,
    arnm_graded_block_pool *pool
);

/** Words valid in [first, last]; everything outside is taken as zero. */
static arnm_result emit_words(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint64_t *words,
    uint32_t first,
    uint32_t last,
    uint32_t cardinality,
    arnm_graded_block_pool *pool
) {
  if (!cardinality) { return ARNM_SUCCESS; }
  if (cardinality <= ARNM_ROARING_ARRAY_MAX) {
    uint16_t values[ARNM_ROARING_ARRAY_MAX];
    uint32_t count = 0;
    for (uint32_t w = first; w <= last; ++w) {
      uint64_t word = words[w];
      while (word) {
        values[count++] = (uint16_t)((w << 6) + (uint32_t)arnm_ctzll(word));
        word &= word - 1u;
      }
    }
    return emit_array(out, key, values, count, pool);
  }
  if (out->cardinality > UINT32_MAX - cardinality) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  arnm_roaring_container container = {0};
  arnm_result result = block_alloc(pool, ROARING_BITMAP_LOG2, &container.data);
  if (ARNM_SUCCESS != result) { return result; }
  memset(container.data, 0, ROARING_BITMAP_BYTES);
  memcpy(
      words_of(&container) + first, words + first, (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  container.key = key;
  container.kind = ARNM_ROARING_BITMAP;
  container.block_log2 = ROARING_BITMAP_LOG2;
  container.cardinality = cardinality;
  result = push_container(out, &container, pool);
  if (ARNM_SUCCESS != result) {
    block_free(pool, container.data, container.block_log2);
    return result;
  }
  uint32_t top = last;
  while (!words[top]) { --top; }
  out->cardinality += cardinality;
  out->maximum = value_of(key, (top << 6) | (uint32_t)(63 - arnm_clzll(words[top])));
  return ARNM_SUCCESS;
}

static arnm_result emit_array(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint16_t *values,
    uint32_t count,
    arnm_graded_block_pool *pool
) {
  if (!count) { return ARNM_SUCCESS; }
  if (out->cardinality > UINT32_MAX - count) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  arnm_roaring_container container = {0};
  const uint8_t log2 = log2_for_bytes(count * (uint32_t)sizeof(uint16_t));
  arnm_result result = block_alloc(pool, log2, &container.data);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(container.data, values, (size_t)count * sizeof(uint16_t));
  container.key = key;
  container.kind = ARNM_ROARING_ARRAY;
  container.block_log2 = log2;
  container.cardinality = count;
  result = push_container(out, &container, pool);
  if (ARNM_SUCCESS != result) {
    block_free(pool, container.data, container.block_log2);
    return result;
  }
  out->cardinality += count;
  out->maximum = value_of(key, values[count - 1u]);
  return ARNM_SUCCESS;
}

// ********** changing form *******************

/**
 * Appends @p values, sorted, to @p out as containers, one array per key: the form a sparse set
 * or a sparse result takes once it is too large. At most two sparse sets' worth of values, so
 * never 4096 in one key.
 */
static arnm_result values_to_containers(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
) {
  uint16_t lows[2u * ARNM_ROARING_SPARSE_MAX];
  uint32_t i = 0;
  while (i < count) {
    const uint16_t key = key_of(values[i]);
    uint32_t n = 0;
    while (i < count && key_of(values[i]) == key) { lows[n++] = low_of(values[i++]); }
    const arnm_result result = emit_array(out, key, lows, n, pool);
    if (ARNM_SUCCESS != result) { return result; }
  }
  return ARNM_SUCCESS;
}

/** A sparse set becomes containers. Unchanged on failure. */
static arnm_result sparse_to_containers(arnm_roaring_bitmap *set, arnm_graded_block_pool *pool) {
  arnm_roaring_bitmap built;
  memset(&built, 0, sizeof(built));
  const arnm_result result = values_to_containers(&built, set->values, set->cardinality, pool);
  if (ARNM_SUCCESS != result) {
    arnm_roaring_free(&built, pool);
    return result;
  }
  block_free(pool, (uint8_t *)set->values, set->directory_log2);
  *set = built;
  return ARNM_SUCCESS;
}

/** @p out, empty, as the sparse or container set of @p values, sorted. */
static arnm_result values_result(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
) {
  if (!count) { return ARNM_SUCCESS; }
  if (count > ARNM_ROARING_SPARSE_MAX) { return values_to_containers(out, values, count, pool); }
  const uint8_t log2 = log2_for_bytes(count * (uint32_t)sizeof(uint32_t));
  uint8_t *block = NULL;
  const arnm_result result = block_alloc(pool, log2, &block);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(block, values, (size_t)count * sizeof(uint32_t));
  out->values = (uint32_t *)(void *)block;
  out->directory_log2 = log2;
  out->cardinality = count;
  out->maximum = values[count - 1u];
  return ARNM_SUCCESS;
}

// ********** reading *******************

bool arnm_roaring_minimum(const arnm_roaring_bitmap *set, uint32_t *out) {
  if (!set || !set->cardinality) { return false; }
  if (is_sparse(set)) {
    *out = set->values[0];
    return true;
  }
  const arnm_roaring_container *first = &set->containers[0];
  if (ARNM_ROARING_ARRAY == first->kind) {
    *out = value_of(first->key, array_of(first)[0]);
    return true;
  }
  const uint64_t *words = words_of(first);
  uint32_t w = 0;
  while (!words[w]) { ++w; }
  *out = value_of(first->key, (w << 6) | (uint32_t)arnm_ctzll(words[w]));
  return true;
}

bool arnm_roaring_maximum(const arnm_roaring_bitmap *set, uint32_t *out) {
  if (!set || !set->cardinality) { return false; }
  *out = set->maximum;
  return true;
}

bool arnm_roaring_contains(const arnm_roaring_bitmap *set, uint32_t value) {
  if (!set) { return false; }
  if (is_sparse(set)) {
    const uint32_t position = lower_bound32(set->values, set->cardinality, value);
    return position < set->cardinality && set->values[position] == value;
  }
  const uint16_t key = key_of(value);
  const uint16_t low = low_of(value);
  const uint32_t index = first_container(set, key);
  if (index == set->count || set->containers[index].key != key) { return false; }
  const arnm_roaring_container *container = &set->containers[index];
  if (ARNM_ROARING_BITMAP == container->kind) {
    return (words_of(container)[low >> 6] >> (low & 63u)) & 1u;
  }
  const uint32_t position = lower_bound16(array_of(container), container->cardinality, low);
  return position < container->cardinality && array_of(container)[position] == low;
}

static uint32_t container_range_cardinality(
    const arnm_roaring_container *container, uint16_t low, uint16_t high
) {
  if (0u == low && 0xffffu == high) { return container->cardinality; }
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = wide_of(container);
    return upper_bound32(wide, container->cardinality, value_of(container->key, high)) -
           lower_bound32(wide, container->cardinality, value_of(container->key, low));
  }
  if (ARNM_ROARING_ARRAY == container->kind) {
    const uint16_t *array = array_of(container);
    return upper_bound16(array, container->cardinality, high) -
           lower_bound16(array, container->cardinality, low);
  }
  return words_cardinality(words_of(container), low, high);
}

uint32_t arnm_roaring_range_cardinality(
    const arnm_roaring_bitmap *set, uint32_t min, uint32_t max
) {
  if (!set || min > max) { return 0u; }
  if (is_sparse(set)) {
    return upper_bound32(set->values, set->cardinality, max) -
           lower_bound32(set->values, set->cardinality, min);
  }
  const uint16_t max_key = key_of(max);
  uint32_t total = 0;
  for (uint32_t i = first_container(set, key_of(min));
       i < set->count && set->containers[i].key <= max_key; ++i) {
    uint16_t low, high;
    container_bounds(set->containers[i].key, min, max, &low, &high);
    total += container_range_cardinality(&set->containers[i], low, high);
  }
  return total;
}

bool arnm_roaring_select(const arnm_roaring_bitmap *set, uint32_t rank, uint32_t *out) {
  if (!set || rank >= set->cardinality) { return false; }
  if (is_sparse(set)) {
    *out = set->values[rank];
    return true;
  }
  for (uint32_t i = 0; i < set->count; ++i) {
    const arnm_roaring_container *container = &set->containers[i];
    if (rank >= container->cardinality) {
      rank -= container->cardinality;
      continue;
    }
    if (ARNM_ROARING_ARRAY == container->kind) {
      *out = value_of(container->key, array_of(container)[rank]);
    } else {
      uint32_t w, bit;
      words_select(words_of(container), rank, &w, &bit);
      *out = value_of(container->key, (w << 6) | bit);
    }
    return true;
  }
  return false;
}

static uint32_t page_ascending(
    const arnm_roaring_bitmap *set, uint32_t index, uint32_t rank, uint32_t size, uint32_t *out
) {
  uint32_t written = 0;
  for (; index < set->count && written < size; ++index, rank = 0) {
    const arnm_roaring_container *container = &set->containers[index];
    if (ARNM_ROARING_ARRAY == container->kind) {
      const uint16_t *array = array_of(container);
      for (uint32_t i = rank; i < container->cardinality && written < size; ++i) {
        out[written++] = value_of(container->key, array[i]);
      }
      continue;
    }
    const uint64_t *words = words_of(container);
    uint32_t w, bit;
    words_select(words, rank, &w, &bit);
    uint64_t word = words[w] & (~0ull << bit);
    for (;;) {
      while (word && written < size) {
        out[written++] = value_of(container->key, (w << 6) | (uint32_t)arnm_ctzll(word));
        word &= word - 1u;
      }
      if (written == size || ++w == ARNM_ROARING_BITMAP_WORDS) { break; }
      word = words[w];
    }
  }
  return written;
}

static uint32_t page_descending(
    const arnm_roaring_bitmap *set, uint32_t index, uint32_t rank, uint32_t size, uint32_t *out
) {
  uint32_t written = 0;
  for (;;) {
    const arnm_roaring_container *container = &set->containers[index];
    if (ARNM_ROARING_ARRAY == container->kind) {
      const uint16_t *array = array_of(container);
      for (uint32_t i = rank + 1u; i > 0u && written < size; --i) {
        out[written++] = value_of(container->key, array[i - 1u]);
      }
    } else {
      const uint64_t *words = words_of(container);
      uint32_t w, bit;
      words_select(words, rank, &w, &bit);
      uint64_t word = words[w] & (~0ull >> (63u - bit));
      for (;;) {
        while (word && written < size) {
          const uint32_t top = 63u - (uint32_t)arnm_clzll(word);
          out[written++] = value_of(container->key, (w << 6) | top);
          word &= ~(1ull << top);
        }
        if (written == size || 0u == w) { break; }
        word = words[--w];
      }
    }
    if (written == size || 0u == index) { break; }
    --index;
    rank = set->containers[index].cardinality - 1u;
  }
  return written;
}

uint32_t arnm_roaring_page(
    const arnm_roaring_bitmap *set, uint32_t skip, uint32_t size, bool descending, uint32_t *out
) {
  if (!set || !out || !size || skip >= set->cardinality) { return 0u; }
  // the rank of the first value to write, counted from the smallest
  uint32_t rank = descending ? set->cardinality - 1u - skip : skip;
  if (is_sparse(set)) {
    const uint32_t left = set->cardinality - skip;
    const uint32_t take = left < size ? left : size;
    for (uint32_t i = 0; i < take; ++i) { out[i] = set->values[descending ? rank - i : rank + i]; }
    return take;
  }
  uint32_t index = 0;
  while (rank >= set->containers[index].cardinality) {
    rank -= set->containers[index].cardinality;
    ++index;
  }
  return descending ? page_descending(set, index, rank, size, out)
                    : page_ascending(set, index, rank, size, out);
}

// ********** reading a union without building it *******************

/** The containers of one key the union has, and the part of the range that key covers. */
typedef struct key_union {
  const arnm_roaring_container *parts[ARNM_ROARING_UNION_MAX];
  uint32_t count;
  uint16_t key;
  uint16_t low;
  uint16_t high;
  bool any_bitmap;
} key_union;

/** First container whose key is > @p key. */
static uint32_t container_after(const arnm_roaring_bitmap *set, uint16_t key) {
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
typedef struct key_source {
  const arnm_roaring_bitmap *set;
  uint16_t min_key;
  uint16_t max_key;
  bool descending;
  /** containers: the current one ascending, one past it descending; sparse: the first value of
   *  the current key ascending, one past its last descending */
  uint32_t position;
  uint32_t next; /**< sparse: where the following key starts */
  bool wide;     /**< sparse: the view reads the values in place instead of copying the low parts */
  const arnm_roaring_container *current; /**< NULL once the range is done */
  arnm_roaring_container view;
  uint16_t buffer[ARNM_ROARING_SPARSE_MAX];
} key_source;

static void source_load(key_source *source) {
  const arnm_roaring_bitmap *set = source->set;
  source->current = NULL;
  if (!set) { return; }
  if (!is_sparse(set)) {
    if (source->descending) {
      if (source->position && set->containers[source->position - 1u].key >= source->min_key) {
        source->current = &set->containers[source->position - 1u];
      }
    } else if (
        source->position < set->count && set->containers[source->position].key <= source->max_key) {
      source->current = &set->containers[source->position];
    }
    return;
  }
  const uint32_t *values = set->values;
  uint32_t begin, end;
  uint16_t key;
  if (source->descending) {
    if (!source->position) { return; }
    key = key_of(values[source->position - 1u]);
    if (key < source->min_key) { return; }
    end = source->position;
    begin = lower_bound32(values, end, (uint32_t)key << 16);
    source->next = begin;
  } else {
    if (source->position >= set->cardinality) { return; }
    key = key_of(values[source->position]);
    if (key > source->max_key) { return; }
    begin = source->position;
    end = begin + upper_bound32(values + begin, set->cardinality - begin, value_of(key, 0xffffu));
    source->next = end;
  }
  source->view.cardinality = end - begin;
  source->view.key = key;
  source->view.block_log2 = 0;
  if (source->wide) {
    source->view.data = (uint8_t *)(uintptr_t)(values + begin);
    source->view.kind = ROARING_KIND_WIDE;
  } else {
    for (uint32_t i = begin; i < end; ++i) { source->buffer[i - begin] = low_of(values[i]); }
    source->view.data = (uint8_t *)source->buffer;
    source->view.kind = ARNM_ROARING_ARRAY;
  }
  source->current = &source->view;
}

static void source_begin(
    key_source *source,
    const arnm_roaring_bitmap *set,
    uint32_t min,
    uint32_t max,
    bool descending,
    bool wide
) {
  source->set = set;
  source->wide = wide;
  source->min_key = key_of(min);
  source->max_key = key_of(max);
  source->descending = descending;
  source->position = 0;
  if (set) {
    if (!is_sparse(set)) {
      source->position = descending ? container_after(set, source->max_key)
                                    : first_container(set, source->min_key);
    } else {
      source->position =
          descending
              ? upper_bound32(set->values, set->cardinality, value_of(source->max_key, 0xffffu))
              : lower_bound32(set->values, set->cardinality, value_of(source->min_key, 0u));
    }
  }
  source_load(source);
}

static void source_advance(key_source *source) {
  if (!source->current) { return; }
  if (is_sparse(source->set)) {
    source->position = source->next;
  } else {
    source->position = source->descending ? source->position - 1u : source->position + 1u;
  }
  source_load(source);
}

/** Where each set stands while the union is walked key by key, in one direction. */
typedef struct union_walk {
  key_source sources[ARNM_ROARING_UNION_MAX];
  bool taken[ARNM_ROARING_UNION_MAX]; /**< gave a container to the key last returned */
  uint32_t count;
  uint32_t min;
  uint32_t max;
  bool descending;
  /** narrow each key to what its parts reach: a page then never searches a bitmap's empty
   *  words, while a count keeps the whole-key bounds its shortcuts look for */
  bool narrow;
} union_walk;

static void union_walk_begin(
    union_walk *walk,
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    bool descending,
    bool narrow
) {
  walk->narrow = narrow;
  walk->count = count;
  walk->min = min;
  walk->max = max;
  walk->descending = descending;
  for (uint32_t s = 0; s < count; ++s) {
    source_begin(&walk->sources[s], sets[s], min, max, descending, true);
    walk->taken[s] = false;
  }
}

/**
 * Gathers the containers of the next key in walking order; false when the range is done. The
 * sources that gave to the key before move on only now, so its views lasted while it was read.
 */
/**
 * The lowest and highest low part a container can hold, as far as it is known without a scan:
 * an array's or a view's first and last value, a bitmap's top if it is the last container of its
 * set -- that is the set's maximum -- and its whole width otherwise.
 */
static inline void part_reach(
    const key_source *source, const arnm_roaring_container *c, uint16_t *bottom, uint16_t *top
) {
  if (ARNM_ROARING_ARRAY == c->kind) {
    *bottom = array_of(c)[0];
    *top = array_of(c)[c->cardinality - 1u];
  } else if (ROARING_KIND_WIDE == c->kind) {
    *bottom = low_of(wide_of(c)[0]);
    *top = low_of(wide_of(c)[c->cardinality - 1u]);
  } else {
    const arnm_roaring_bitmap *set = source->set;
    *bottom = 0;
    *top = c == &set->containers[set->count - 1u] ? low_of(set->maximum) : 0xffffu;
  }
}

static bool union_walk_next(union_walk *walk, key_union *out) {
  for (;;) {
    bool found = false;
    uint16_t best = 0;
    for (uint32_t s = 0; s < walk->count; ++s) {
      if (walk->taken[s]) {
        source_advance(&walk->sources[s]);
        walk->taken[s] = false;
      }
      const arnm_roaring_container *c = walk->sources[s].current;
      if (!c) { continue; }
      if (!found || (walk->descending ? c->key > best : c->key < best)) {
        best = c->key;
        found = true;
      }
    }
    if (!found) { return false; }
    out->count = 0;
    out->key = best;
    out->any_bitmap = false;
    uint16_t bottom = 0xffffu, top = 0;
    for (uint32_t s = 0; s < walk->count; ++s) {
      const arnm_roaring_container *c = walk->sources[s].current;
      if (!c || c->key != best) { continue; }
      out->parts[out->count++] = c;
      out->any_bitmap |= ARNM_ROARING_BITMAP == c->kind;
      walk->taken[s] = true;
      if (walk->narrow) {
        uint16_t part_bottom, part_top;
        part_reach(&walk->sources[s], c, &part_bottom, &part_top);
        if (part_bottom < bottom) { bottom = part_bottom; }
        if (part_top > top) { top = part_top; }
      }
    }
    container_bounds(best, walk->min, walk->max, &out->low, &out->high);
    if (!walk->narrow) { return true; }
    // nothing lies outside what the parts reach: a bitmap is not searched through empty words
    if (bottom > out->low) { out->low = bottom; }
    if (top < out->high) { out->high = top; }
    if (out->low <= out->high) { return true; }
    // the range misses every value of this key; the next one
  }
}

/*
 * Array values in the range above which a key shared by several sets is counted in bits: set
 * them in a buffer over the range's words and count those, instead of merging value by value.
 * The merge costs some 30 instructions a value, the bits a store a value and a popcount a word.
 */
#define ROARING_UNION_BITS_FROM 128u

// ********** a union of sparse sets *******************

/** The part of each sparse set inside [min, max]; empty sets and empty parts left out. */
typedef struct sparse_spans {
  const uint32_t *values[ARNM_ROARING_UNION_MAX];
  uint32_t begin[ARNM_ROARING_UNION_MAX];
  uint32_t end[ARNM_ROARING_UNION_MAX];
  uint32_t count;
} sparse_spans;

/** Whether every set is sparse or missing, and so the union can be read off the arrays. */
static bool all_sparse(const arnm_roaring_bitmap *const *sets, uint32_t count) {
  for (uint32_t s = 0; s < count; ++s) {
    if (sets[s] && !is_sparse(sets[s])) { return false; }
  }
  return true;
}

static void sparse_spans_of(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    sparse_spans *spans
) {
  spans->count = 0;
  for (uint32_t s = 0; s < count; ++s) {
    const arnm_roaring_bitmap *set = sets[s];
    if (!set || !set->cardinality) { continue; }
    const uint32_t begin = lower_bound32(set->values, set->cardinality, min);
    const uint32_t end = upper_bound32(set->values, set->cardinality, max);
    if (begin == end) { continue; }
    spans->values[spans->count] = set->values;
    spans->begin[spans->count] = begin;
    spans->end[spans->count] = end;
    spans->count++;
  }
}

/** Distinct values of the spans merged, value by value. */
static uint64_t sparse_merged_cardinality(sparse_spans *spans) {
  if (1u == spans->count) { return spans->end[0] - spans->begin[0]; }
  uint64_t count = 0;
  while (spans->count) {
    uint32_t smallest = spans->values[0][spans->begin[0]];
    for (uint32_t p = 1; p < spans->count; ++p) {
      const uint32_t value = spans->values[p][spans->begin[p]];
      if (value < smallest) { smallest = value; }
    }
    ++count;
    for (uint32_t p = 0; p < spans->count;) {
      if (spans->values[p][spans->begin[p]] == smallest && ++spans->begin[p] == spans->end[p]) {
        // a span that ran out takes the last one's place
        --spans->count;
        spans->values[p] = spans->values[spans->count];
        spans->begin[p] = spans->begin[spans->count];
        spans->end[p] = spans->end[spans->count];
        continue;
      }
      ++p;
    }
  }
  return count;
}

/**
 * Distinct values of the spans. Below ROARING_UNION_BITS_FROM values in all they are merged in
 * one go; above, key by key: a key only one span has is counted from its length,
 * a key where the spans hold ROARING_UNION_BITS_FROM values or more is counted in bits over the
 * words its values reach, the rest is merged value by value -- the same choice a key of
 * containers gets.
 */
static uint64_t sparse_union_cardinality(sparse_spans *spans) {
  if (1u == spans->count) { return spans->end[0] - spans->begin[0]; }
  // few values in all -- the usual sparse set spread one or two per key -- merge in one go
  uint32_t values = 0;
  for (uint32_t p = 0; p < spans->count; ++p) { values += spans->end[p] - spans->begin[p]; }
  if (values < ROARING_UNION_BITS_FROM) { return sparse_merged_cardinality(spans); }
  uint64_t total = 0;
  sparse_spans in_key;
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  while (spans->count) {
    uint32_t smallest = spans->values[0][spans->begin[0]];
    for (uint32_t p = 1; p < spans->count; ++p) {
      const uint32_t value = spans->values[p][spans->begin[p]];
      if (value < smallest) { smallest = value; }
    }
    const uint16_t key = key_of(smallest);
    const uint32_t key_last = value_of(key, 0xffffu);
    // each span's part in this key, and every span moved past it
    in_key.count = 0;
    uint32_t values_in_key = 0;
    uint16_t low = 0xffffu, high = 0;
    for (uint32_t p = 0; p < spans->count;) {
      const uint32_t *values = spans->values[p];
      const uint32_t begin = spans->begin[p];
      if (key_of(values[begin]) == key) {
        const uint32_t end = begin + upper_bound32(values + begin, spans->end[p] - begin, key_last);
        in_key.values[in_key.count] = values;
        in_key.begin[in_key.count] = begin;
        in_key.end[in_key.count] = end;
        in_key.count++;
        values_in_key += end - begin;
        if (low_of(values[begin]) < low) { low = low_of(values[begin]); }
        if (low_of(values[end - 1u]) > high) { high = low_of(values[end - 1u]); }
        spans->begin[p] = end;
        if (end == spans->end[p]) {
          --spans->count;
          spans->values[p] = spans->values[spans->count];
          spans->begin[p] = spans->begin[spans->count];
          spans->end[p] = spans->end[spans->count];
          continue;
        }
      }
      ++p;
    }
    if (1u == in_key.count) {
      total += values_in_key;
    } else if (values_in_key >= ROARING_UNION_BITS_FROM) {
      const uint32_t first = low >> 6;
      const uint32_t last = high >> 6;
      memset(words + first, 0, (size_t)(last - first + 1u) * sizeof(uint64_t));
      for (uint32_t p = 0; p < in_key.count; ++p) {
        for (uint32_t i = in_key.begin[p]; i < in_key.end[p]; ++i) {
          const uint16_t bit = low_of(in_key.values[p][i]);
          words[bit >> 6] |= 1ull << (bit & 63u);
        }
      }
      for (uint32_t w = first; w <= last; ++w) { total += (uint32_t)arnm_popcountll(words[w]); }
    } else {
      total += sparse_merged_cardinality(&in_key);
    }
  }
  return total;
}

/** The spans merged in walking order: up to @p size values after passing over @p skip. */
static uint32_t sparse_union_page(
    sparse_spans *spans, uint32_t skip, uint32_t size, bool descending, uint32_t *out
) {
  uint32_t written = 0;
  if (1u == spans->count) {
    const uint32_t available = spans->end[0] - spans->begin[0];
    if (skip >= available) { return 0; }
    const uint32_t left = available - skip;
    const uint32_t take = left < size ? left : size;
    const uint32_t *values = spans->values[0];
    for (uint32_t i = 0; i < take; ++i) {
      out[i] = values[descending ? spans->end[0] - 1u - skip - i : spans->begin[0] + skip + i];
    }
    return take;
  }
  while (written < size && spans->count) {
    // ascending reads each span from its front, descending from its back
    uint32_t pick =
        descending ? spans->values[0][spans->end[0] - 1u] : spans->values[0][spans->begin[0]];
    for (uint32_t p = 1; p < spans->count; ++p) {
      const uint32_t value =
          descending ? spans->values[p][spans->end[p] - 1u] : spans->values[p][spans->begin[p]];
      if (descending ? value > pick : value < pick) { pick = value; }
    }
    if (skip) {
      --skip;
    } else {
      out[written++] = pick;
    }
    for (uint32_t p = 0; p < spans->count;) {
      const uint32_t value =
          descending ? spans->values[p][spans->end[p] - 1u] : spans->values[p][spans->begin[p]];
      if (value == pick) {
        if (descending) {
          spans->end[p]--;
        } else {
          spans->begin[p]++;
        }
        if (spans->begin[p] == spans->end[p]) {
          --spans->count;
          spans->values[p] = spans->values[spans->count];
          spans->begin[p] = spans->begin[spans->count];
          spans->end[p] = spans->end[spans->count];
          continue;
        }
      }
      ++p;
    }
  }
  return written;
}

/** The union of a key's containers as bits in [low, high], edges masked; returns the count. */
static uint32_t key_union_words(const key_union *u, uint64_t *words) {
  const uint32_t first = u->low >> 6;
  const uint32_t last = u->high >> 6;
  memset(words + first, 0, (size_t)(last - first + 1u) * sizeof(uint64_t));
  for (uint32_t p = 0; p < u->count; ++p) {
    const arnm_roaring_container *c = u->parts[p];
    if (ARNM_ROARING_BITMAP == c->kind) {
      const uint64_t *x = words_of(c);
      for (uint32_t w = first; w <= last; ++w) { words[w] |= x[w]; }
    } else if (ROARING_KIND_WIDE == c->kind) {
      const uint32_t *wide = wide_of(c);
      const uint32_t begin = lower_bound32(wide, c->cardinality, value_of(u->key, u->low));
      const uint32_t end = upper_bound32(wide, c->cardinality, value_of(u->key, u->high));
      for (uint32_t i = begin; i < end; ++i) {
        const uint16_t low = low_of(wide[i]);
        words[low >> 6] |= 1ull << (low & 63u);
      }
    } else {
      const uint16_t *array = array_of(c);
      const uint32_t begin = lower_bound16(array, c->cardinality, u->low);
      const uint32_t end = upper_bound16(array, c->cardinality, u->high);
      for (uint32_t i = begin; i < end; ++i) { words[array[i] >> 6] |= 1ull << (array[i] & 63u); }
    }
  }
  return clip_and_count(words, u->low, u->high);
}

/*
 * Index spans of a key's arrays inside [low, high], for merging them. The merges only run below
 * ROARING_UNION_BITS_FROM array values in the range, so a wide part's slice is copied into
 * @c lows as low parts -- a few dozen values -- and merged like any array.
 */
typedef struct array_spans {
  const uint16_t *array[ARNM_ROARING_UNION_MAX];
  uint32_t begin[ARNM_ROARING_UNION_MAX];
  uint32_t end[ARNM_ROARING_UNION_MAX];
  uint16_t lows[ROARING_UNION_BITS_FROM];
} array_spans;

static void key_union_spans(const key_union *u, array_spans *spans) {
  uint32_t used = 0;
  for (uint32_t p = 0; p < u->count; ++p) {
    const arnm_roaring_container *c = u->parts[p];
    if (ROARING_KIND_WIDE == c->kind) {
      const uint32_t *wide = wide_of(c);
      const uint32_t begin = lower_bound32(wide, c->cardinality, value_of(u->key, u->low));
      const uint32_t end = upper_bound32(wide, c->cardinality, value_of(u->key, u->high));
      spans->array[p] = spans->lows + used;
      spans->begin[p] = 0;
      spans->end[p] = end - begin;
      for (uint32_t i = begin; i < end; ++i) { spans->lows[used++] = low_of(wide[i]); }
      continue;
    }
    spans->array[p] = array_of(c);
    spans->begin[p] = u->low ? lower_bound16(array_of(c), c->cardinality, u->low) : 0u;
    spans->end[p] =
        0xffffu == u->high ? c->cardinality : upper_bound16(array_of(c), c->cardinality, u->high);
  }
}

/** Distinct values of a key's arrays merged, all of them only arrays. */
static uint32_t merged_arrays_cardinality(const key_union *u) {
  array_spans spans;
  key_union_spans(u, &spans);
  uint32_t count = 0;
  for (;;) {
    uint32_t smallest = 0x10000u;
    for (uint32_t p = 0; p < u->count; ++p) {
      if (spans.begin[p] < spans.end[p] && spans.array[p][spans.begin[p]] < smallest) {
        smallest = spans.array[p][spans.begin[p]];
      }
    }
    if (0x10000u == smallest) { return count; }
    ++count;
    for (uint32_t p = 0; p < u->count; ++p) {
      if (spans.begin[p] < spans.end[p] && spans.array[p][spans.begin[p]] == smallest) {
        spans.begin[p]++;
      }
    }
  }
}

/** Whether any bitmap among @p bitmaps has @p low set. */
static inline bool any_bit(const uint64_t *const *bitmaps, uint32_t count, uint16_t low) {
  for (uint32_t b = 0; b < count; ++b) {
    if ((bitmaps[b][low >> 6] >> (low & 63u)) & 1u) { return true; }
  }
  return false;
}

/**
 * The count of a key several sets share where a bitmap is among them, without building the
 * union: the bitmaps' OR is counted word by word and nothing is stored, then every array value
 * inside the range counts once if no bitmap has it already.
 */
static uint32_t mixed_union_cardinality(const key_union *u) {
  const uint64_t *bitmaps[ARNM_ROARING_UNION_MAX];
  key_union arrays;
  uint32_t bitmap_count = 0;
  arrays.count = 0;
  arrays.key = u->key;
  arrays.low = u->low;
  arrays.high = u->high;
  arrays.any_bitmap = false;
  for (uint32_t p = 0; p < u->count; ++p) {
    if (ARNM_ROARING_BITMAP == u->parts[p]->kind) {
      bitmaps[bitmap_count++] = words_of(u->parts[p]);
    } else { // arrays and wide views alike: key_union_spans reads both
      arrays.parts[arrays.count++] = u->parts[p];
    }
  }

  uint32_t total;
  if (1u == bitmap_count) {
    total = words_cardinality(bitmaps[0], u->low, u->high);
  } else {
    const uint32_t first = u->low >> 6;
    const uint32_t last = u->high >> 6;
    total = 0;
    for (uint32_t w = first; w <= last; ++w) {
      uint64_t word = 0;
      for (uint32_t b = 0; b < bitmap_count; ++b) { word |= bitmaps[b][w]; }
      if (w == first) { word &= first_word_mask(u->low); }
      if (w == last) { word &= last_word_mask(u->high); }
      total += (uint32_t)arnm_popcountll(word);
    }
  }
  if (!arrays.count) { return total; }

  // the arrays merged, each distinct value checked against the bitmaps once
  array_spans spans;
  key_union_spans(&arrays, &spans);
  for (;;) {
    uint32_t smallest = 0x10000u;
    for (uint32_t p = 0; p < arrays.count; ++p) {
      if (spans.begin[p] < spans.end[p] && spans.array[p][spans.begin[p]] < smallest) {
        smallest = spans.array[p][spans.begin[p]];
      }
    }
    if (0x10000u == smallest) { return total; }
    if (!any_bit(bitmaps, bitmap_count, (uint16_t)smallest)) { ++total; }
    for (uint32_t p = 0; p < arrays.count; ++p) {
      if (spans.begin[p] < spans.end[p] && spans.array[p][spans.begin[p]] == smallest) {
        spans.begin[p]++;
      }
    }
  }
}

/** Array values of a key's containers inside [low, high], counted with repeats. */
static uint32_t array_values_in_range(const key_union *u) {
  uint32_t total = 0;
  for (uint32_t p = 0; p < u->count; ++p) {
    const arnm_roaring_container *c = u->parts[p];
    if (ARNM_ROARING_BITMAP == c->kind) { continue; }
    if (ROARING_KIND_WIDE == c->kind) {
      total += container_range_cardinality(c, u->low, u->high);
      continue;
    }
    const uint16_t *array = array_of(c);
    const uint32_t begin = u->low ? lower_bound16(array, c->cardinality, u->low) : 0u;
    const uint32_t end =
        0xffffu == u->high ? c->cardinality : upper_bound16(array, c->cardinality, u->high);
    total += end - begin;
  }
  return total;
}

static uint32_t key_union_cardinality(const key_union *u) {
  if (1u == u->count) { return container_range_cardinality(u->parts[0], u->low, u->high); }
  if (array_values_in_range(u) >= ROARING_UNION_BITS_FROM) {
    uint64_t words[ARNM_ROARING_BITMAP_WORDS];
    return key_union_words(u, words);
  }
  if (u->any_bitmap) { return mixed_union_cardinality(u); }
  return merged_arrays_cardinality(u);
}

/**
 * Up to @p size values of bits @p words holds in [low, high], after passing over *skip of them;
 * the edge words are masked here, so the words may be a container's own.
 */
static uint32_t words_emit(
    const uint64_t *words,
    uint16_t low,
    uint16_t high,
    uint16_t key,
    uint32_t *skip,
    uint32_t size,
    bool descending,
    uint32_t *out
) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  uint32_t written = 0;
  for (uint32_t step = 0; step <= last - first && written < size; ++step) {
    const uint32_t w = descending ? last - step : first + step;
    uint64_t word = words[w];
    if (w == first) { word &= first_word_mask(low); }
    if (w == last) { word &= last_word_mask(high); }
    const uint32_t bits = (uint32_t)arnm_popcountll(word);
    if (*skip >= bits) {
      *skip -= bits;
      continue;
    }
    while (*skip) { // drop the bits passed over, from the end the walk comes from
      word &= descending ? ~(1ull << (63u - (uint32_t)arnm_clzll(word))) : word - 1u;
      --*skip;
    }
    while (word && written < size) {
      const uint32_t bit =
          descending ? 63u - (uint32_t)arnm_clzll(word) : (uint32_t)arnm_ctzll(word);
      out[written++] = value_of(key, (w << 6) | bit);
      word &= ~(1ull << bit);
    }
  }
  return written;
}

/**
 * One container of a key, read value by value in one direction inside [low, high]: an array by
 * its index, a bitmap word by word. What a page merges when it needs only a few values.
 */
typedef struct cursor {
  const uint32_t *wide;  /**< a wide view's values, NULL otherwise */
  const uint16_t *array; /**< the array's values, NULL for a bitmap or a wide view */
  const uint64_t *words; /**< the bitmap's words, NULL for an array */
  uint32_t index;        /**< array: next index ascending, one past it descending */
  uint32_t stop;         /**< array: the index the walk ends at */
  uint32_t word_index;
  uint64_t word;  /**< bitmap: bits of word_index not yet read, range edges masked */
  uint32_t value; /**< the low part it stands on */
  bool live;
} cursor;

/** Loads the next bitmap word that has a bit left inside the range, or ends the cursor. */
static void cursor_seek_word(cursor *c, uint16_t low, uint16_t high, bool descending) {
  const uint64_t *words = c->words;
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  for (;;) {
    if (c->word) {
      const uint32_t bit =
          descending ? 63u - (uint32_t)arnm_clzll(c->word) : (uint32_t)arnm_ctzll(c->word);
      c->value = (c->word_index << 6) | bit;
      return;
    }
    if (descending ? c->word_index == first : c->word_index == last) {
      c->live = false;
      return;
    }
    c->word_index = descending ? c->word_index - 1u : c->word_index + 1u;
    c->word = words[c->word_index];
    if (c->word_index == first) { c->word &= first_word_mask(low); }
    if (c->word_index == last) { c->word &= last_word_mask(high); }
  }
}

static void cursor_begin(
    cursor *c, const arnm_roaring_container *container, uint16_t low, uint16_t high, bool descending
) {
  c->live = true;
  c->wide = NULL;
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = wide_of(container);
    c->wide = wide;
    c->array = NULL;
    c->words = NULL;
    const uint32_t begin =
        low ? lower_bound32(wide, container->cardinality, value_of(container->key, low)) : 0u;
    const uint32_t end =
        0xffffu == high
            ? container->cardinality
            : upper_bound32(wide, container->cardinality, value_of(container->key, high));
    c->index = descending ? end : begin;
    c->stop = descending ? begin : end;
    c->live = c->index != c->stop;
    if (c->live) { c->value = low_of(wide[descending ? c->index - 1u : c->index]); }
    return;
  }
  if (ARNM_ROARING_ARRAY == container->kind) {
    const uint16_t *array = array_of(container);
    c->array = array;
    c->words = NULL;
    const uint32_t begin = low ? lower_bound16(array, container->cardinality, low) : 0u;
    const uint32_t end = 0xffffu == high ? container->cardinality
                                         : upper_bound16(array, container->cardinality, high);
    c->index = descending ? end : begin;
    c->stop = descending ? begin : end;
    c->live = c->index != c->stop;
    if (c->live) { c->value = array[descending ? c->index - 1u : c->index]; }
    return;
  }
  c->array = NULL;
  c->words = words_of(container);
  c->word_index = descending ? high >> 6 : low >> 6;
  c->word = c->words[c->word_index];
  if (c->word_index == (uint32_t)(low >> 6)) { c->word &= first_word_mask(low); }
  if (c->word_index == (uint32_t)(high >> 6)) { c->word &= last_word_mask(high); }
  cursor_seek_word(c, low, high, descending);
}

static inline void cursor_next(cursor *c, uint16_t low, uint16_t high, bool descending) {
  if (c->wide) {
    c->index = descending ? c->index - 1u : c->index + 1u;
    c->live = c->index != c->stop;
    if (c->live) { c->value = low_of(c->wide[descending ? c->index - 1u : c->index]); }
    return;
  }
  if (c->array) {
    c->index = descending ? c->index - 1u : c->index + 1u;
    c->live = c->index != c->stop;
    if (c->live) { c->value = c->array[descending ? c->index - 1u : c->index]; }
    return;
  }
  c->word &= ~(1ull << (c->value & 63u));
  cursor_seek_word(c, low, high, descending);
}

/** Merges a key's containers value by value in walking order, passing over *skip first. */
static uint32_t merged_cursors_emit(
    const key_union *u, uint32_t *skip, uint32_t size, bool descending, uint32_t *out
) {
  cursor cursors[ARNM_ROARING_UNION_MAX];
  uint32_t live = 0;
  for (uint32_t p = 0; p < u->count; ++p) {
    cursor_begin(&cursors[live], u->parts[p], u->low, u->high, descending);
    if (cursors[live].live) { ++live; }
  }
  uint32_t written = 0;
  while (written < size && live > 1u) {
    uint32_t pick = cursors[0].value;
    for (uint32_t i = 1; i < live; ++i) {
      if (descending ? cursors[i].value > pick : cursors[i].value < pick) {
        pick = cursors[i].value;
      }
    }
    if (*skip) {
      --*skip;
    } else {
      out[written++] = value_of(u->key, pick);
    }
    // every cursor standing on it moves on; one that runs out is dropped
    for (uint32_t i = 0; i < live;) {
      if (cursors[i].value == pick) {
        cursor_next(&cursors[i], u->low, u->high, descending);
        if (!cursors[i].live) {
          cursors[i] = cursors[--live];
          continue;
        }
      }
      ++i;
    }
  }
  // one container left: the rest comes from it alone, no comparing
  if (1u == live) {
    cursor *c = &cursors[0];
    while (written < size && c->live) {
      if (*skip) {
        --*skip;
      } else {
        out[written++] = value_of(u->key, c->value);
      }
      cursor_next(c, u->low, u->high, descending);
    }
  }
  return written;
}

/*
 * Values a page reads out of one shared key -- skipped and written together -- up to which it
 * merges the containers value by value; past it the key's union is built in bits first. The
 * merge costs a few instructions per value and container, the bits a pass over the key.
 */
#define ROARING_UNION_MERGE_UP_TO 1024u

static uint32_t key_union_emit(
    const key_union *u, uint32_t *skip, uint32_t size, bool descending, uint32_t *out
) {
  if (1u == u->count && ROARING_KIND_WIDE == u->parts[0]->kind) {
    const arnm_roaring_container *c = u->parts[0];
    const uint32_t *wide = wide_of(c);
    const uint32_t begin =
        u->low ? lower_bound32(wide, c->cardinality, value_of(u->key, u->low)) : 0u;
    const uint32_t end = 0xffffu == u->high
                             ? c->cardinality
                             : upper_bound32(wide, c->cardinality, value_of(u->key, u->high));
    const uint32_t available = end - begin;
    if (*skip >= available) {
      *skip -= available;
      return 0;
    }
    const uint32_t left = available - *skip;
    const uint32_t take = left < size ? left : size;
    for (uint32_t i = 0; i < take; ++i) {
      out[i] = wide[descending ? end - 1u - *skip - i : begin + *skip + i];
    }
    *skip = 0;
    return take;
  }
  if (1u == u->count && ARNM_ROARING_ARRAY == u->parts[0]->kind) {
    const arnm_roaring_container *c = u->parts[0];
    const uint16_t *array = array_of(c);
    const uint32_t begin = u->low ? lower_bound16(array, c->cardinality, u->low) : 0u;
    const uint32_t end =
        0xffffu == u->high ? c->cardinality : upper_bound16(array, c->cardinality, u->high);
    const uint32_t available = end - begin;
    if (*skip >= available) {
      *skip -= available;
      return 0;
    }
    const uint32_t left = available - *skip;
    const uint32_t take = left < size ? left : size;
    for (uint32_t i = 0; i < take; ++i) {
      out[i] = value_of(u->key, array[descending ? end - 1u - *skip - i : begin + *skip + i]);
    }
    *skip = 0;
    return take;
  }
  if (1u == u->count) {
    return words_emit(words_of(u->parts[0]), u->low, u->high, u->key, skip, size, descending, out);
  }
  if ((uint64_t)(*skip + (uint64_t)size) * u->count <= ROARING_UNION_MERGE_UP_TO) {
    return merged_cursors_emit(u, skip, size, descending, out);
  }
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  (void)key_union_words(u, words);
  return words_emit(words, u->low, u->high, u->key, skip, size, descending, out);
}

arnm_result arnm_roaring_union_cardinality(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    uint64_t *out
) {
  if (!sets || !out) { return ARNM_ERROR_NULL_POINTER; }
  if (count > ARNM_ROARING_UNION_MAX) { return ARNM_ERROR_INVALID_PARAM; }
  uint64_t total = 0;
  if (min <= max && all_sparse(sets, count)) {
    sparse_spans spans;
    sparse_spans_of(sets, count, min, max, &spans);
    total = spans.count ? sparse_union_cardinality(&spans) : 0u;
  } else if (min <= max) {
    union_walk walk;
    union_walk_begin(&walk, sets, count, min, max, false, false);
    key_union u;
    while (union_walk_next(&walk, &u)) { total += key_union_cardinality(&u); }
  }
  *out = total;
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_union_page(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written
) {
  if (!sets || !written || (size && !out)) { return ARNM_ERROR_NULL_POINTER; }
  if (count > ARNM_ROARING_UNION_MAX) { return ARNM_ERROR_INVALID_PARAM; }
  uint32_t total = 0;
  if (size && min <= max && all_sparse(sets, count)) {
    sparse_spans spans;
    sparse_spans_of(sets, count, min, max, &spans);
    total = spans.count ? sparse_union_page(&spans, skip, size, descending, out) : 0u;
  } else if (size && min <= max) {
    union_walk walk;
    union_walk_begin(&walk, sets, count, min, max, descending, true);
    key_union u;
    while (total < size && union_walk_next(&walk, &u)) {
      // a key the skip passes over whole is only counted, never read value by value
      if (skip) {
        const uint32_t in_key = key_union_cardinality(&u);
        if (skip >= in_key) {
          skip -= in_key;
          continue;
        }
      }
      total += key_union_emit(&u, &skip, size - total, descending, out + total);
    }
  }
  *written = total;
  return ARNM_SUCCESS;
}

// ********** set operations *******************

/** A container restricted to [low, high]; for an array also the index span inside it. */
typedef struct clip {
  const arnm_roaring_container *container;
  uint16_t low;
  uint16_t high;
  uint32_t begin; /**< arrays: first index inside the range */
  uint32_t end;   /**< arrays: one past the last index inside the range */
} clip;

static clip make_clip(const arnm_roaring_container *container, uint16_t low, uint16_t high) {
  clip result = {container, low, high, 0u, 0u};
  if (ARNM_ROARING_ARRAY == container->kind) {
    const uint16_t *array = array_of(container);
    result.begin = low ? lower_bound16(array, container->cardinality, low) : 0u;
    result.end = 0xffffu == high ? container->cardinality
                                 : upper_bound16(array, container->cardinality, high);
  }
  return result;
}

static arnm_result emit_clip(
    arnm_roaring_bitmap *out, const clip *c, arnm_graded_block_pool *pool
) {
  const arnm_roaring_container *container = c->container;
  if (ARNM_ROARING_ARRAY == container->kind) {
    return emit_array(out, container->key, array_of(container) + c->begin, c->end - c->begin, pool);
  }
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  const uint32_t first = c->low >> 6;
  const uint32_t last = c->high >> 6;
  memcpy(
      words + first, words_of(container) + first, (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  const uint32_t cardinality = clip_and_count(words, c->low, c->high);
  return emit_words(out, container->key, words, first, last, cardinality, pool);
}

static arnm_result and_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = array_of(a->container) + a->begin;
  const uint16_t *y = array_of(b->container) + b->begin;
  uint32_t nx = a->end - a->begin;
  uint32_t ny = b->end - b->begin;
  if (nx > ny) {
    const uint16_t *swap = x;
    x = y;
    y = swap;
    const uint32_t swap_count = nx;
    nx = ny;
    ny = swap_count;
  }
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  if ((uint64_t)nx * 32u < ny) {
    // skewed: binary search every value of the small one in what is left of the large one
    uint32_t from = 0;
    for (uint32_t i = 0; i < nx && from < ny; ++i) {
      from += lower_bound16(y + from, ny - from, x[i]);
      if (from < ny && y[from] == x[i]) { values[count++] = x[i]; }
    }
  } else {
    uint32_t i = 0, j = 0;
    while (i < nx && j < ny) {
      if (x[i] < y[j]) {
        ++i;
      } else if (x[i] > y[j]) {
        ++j;
      } else {
        values[count++] = x[i];
        ++i;
        ++j;
      }
    }
  }
  return emit_array(out, a->container->key, values, count, pool);
}

/** Values of array @p a whose bit in bitmap @p b is set (@p keep) or clear (!@p keep). */
static arnm_result filter_array_by_words(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, bool keep, arnm_graded_block_pool *pool
) {
  const uint16_t *x = array_of(a->container);
  const uint64_t *words = words_of(b->container);
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  // a branch, not the branchless append sparse_filter uses: measured, the array's values run in
  // long streaks of kept or dropped often enough that the branch is the faster of the two here
  for (uint32_t i = a->begin; i < a->end; ++i) {
    const bool set = (words[x[i] >> 6] >> (x[i] & 63u)) & 1u;
    if (set == keep) { values[count++] = x[i]; }
  }
  return emit_array(out, a->container->key, values, count, pool);
}

typedef enum word_op { WORD_AND, WORD_OR, WORD_ANDNOT } word_op;

/* the operation is a constant at every call site, so each loop compiles without the switch */
static inline arnm_result combine_words(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, word_op op, arnm_graded_block_pool *pool
) {
  const uint64_t *x = words_of(a->container);
  const uint64_t *y = words_of(b->container);
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  const uint32_t first = a->low >> 6;
  const uint32_t last = a->high >> 6;
  switch (op) {
  case WORD_AND:
    for (uint32_t w = first; w <= last; ++w) { words[w] = x[w] & y[w]; }
    break;
  case WORD_OR:
    for (uint32_t w = first; w <= last; ++w) { words[w] = x[w] | y[w]; }
    break;
  case WORD_ANDNOT:
    for (uint32_t w = first; w <= last; ++w) { words[w] = x[w] & ~y[w]; }
    break;
  }
  const uint32_t cardinality = clip_and_count(words, a->low, a->high);
  return emit_words(out, a->container->key, words, first, last, cardinality, pool);
}

/** Bitmap @p a with the values of array @p b set (@p set) or cleared, within a's range. */
static arnm_result words_with_array(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, bool set, arnm_graded_block_pool *pool
) {
  const uint16_t *y = array_of(b->container);
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  const uint32_t first = a->low >> 6;
  const uint32_t last = a->high >> 6;
  memcpy(
      words + first, words_of(a->container) + first, (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  // b is clipped to the same range, so every value lands inside [first, last]
  if (set) {
    for (uint32_t i = b->begin; i < b->end; ++i) { words[y[i] >> 6] |= 1ull << (y[i] & 63u); }
  } else {
    for (uint32_t i = b->begin; i < b->end; ++i) { words[y[i] >> 6] &= ~(1ull << (y[i] & 63u)); }
  }
  const uint32_t cardinality = clip_and_count(words, a->low, a->high);
  return emit_words(out, a->container->key, words, first, last, cardinality, pool);
}

static arnm_result or_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = array_of(a->container);
  const uint16_t *y = array_of(b->container);
  if ((a->end - a->begin) + (b->end - b->begin) > ARNM_ROARING_ARRAY_MAX) {
    // may pass 4096: straight into bits, emit_words decides the kind by the real count
    uint64_t words[ARNM_ROARING_BITMAP_WORDS];
    const uint32_t first = a->low >> 6;
    const uint32_t last = a->high >> 6;
    memset(words + first, 0, (size_t)(last - first + 1u) * sizeof(uint64_t));
    for (uint32_t i = a->begin; i < a->end; ++i) { words[x[i] >> 6] |= 1ull << (x[i] & 63u); }
    for (uint32_t j = b->begin; j < b->end; ++j) { words[y[j] >> 6] |= 1ull << (y[j] & 63u); }
    uint32_t cardinality = 0;
    for (uint32_t w = first; w <= last; ++w) { cardinality += (uint32_t)arnm_popcountll(words[w]); }
    return emit_words(out, a->container->key, words, first, last, cardinality, pool);
  }
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  uint32_t i = a->begin, j = b->begin;
  while (i < a->end && j < b->end) {
    if (x[i] < y[j]) {
      values[count++] = x[i++];
    } else if (x[i] > y[j]) {
      values[count++] = y[j++];
    } else {
      values[count++] = x[i++];
      ++j;
    }
  }
  while (i < a->end) { values[count++] = x[i++]; }
  while (j < b->end) { values[count++] = y[j++]; }
  return emit_array(out, a->container->key, values, count, pool);
}

static arnm_result andnot_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = array_of(a->container);
  const uint16_t *y = array_of(b->container);
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  uint32_t j = b->begin;
  for (uint32_t i = a->begin; i < a->end; ++i) {
    while (j < b->end && y[j] < x[i]) { ++j; }
    if (j == b->end || y[j] != x[i]) { values[count++] = x[i]; }
  }
  return emit_array(out, a->container->key, values, count, pool);
}

/** The checks every set operation starts with. */
static arnm_result check_operation(
    const arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    const arnm_graded_block_pool *pool
) {
  if (!out || !a || !b || !pool) { return ARNM_ERROR_NULL_POINTER; }
  if (out == a || out == b || out->containers || out->cardinality) {
    return ARNM_ERROR_INVALID_PARAM;
  }
  return ARNM_SUCCESS;
}

/** Whatever was built goes back, so a failed operation leaves @p out as empty as it came. */
static arnm_result finish(
    arnm_roaring_bitmap *out, arnm_result result, arnm_graded_block_pool *pool
) {
  if (ARNM_SUCCESS != result) { arnm_roaring_free(out, pool); }
  return result;
}

/*
 * Sparse inputs are small (at most ARNM_ROARING_SPARSE_MAX values), so where the result can only
 * be a part of one, it is built straight as a sparse set: each of its values in the range kept
 * or dropped by a lookup in the other set.
 */
/** Whether @p low is in @p container; @p index walks an array forward across calls. */
static inline bool container_has(
    const arnm_roaring_container *container, uint16_t low, uint32_t *index
) {
  if (ARNM_ROARING_BITMAP == container->kind) {
    return (words_of(container)[low >> 6] >> (low & 63u)) & 1u;
  }
  const uint16_t *array = array_of(container);
  while (*index < container->cardinality && array[*index] < low) { ++*index; }
  return *index < container->cardinality && array[*index] == low;
}

static arnm_result sparse_filter(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *sparse,
    const arnm_roaring_bitmap *other,
    bool keep,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  uint32_t kept[ARNM_ROARING_SPARSE_MAX];
  uint32_t count = 0;
  const uint32_t *values = sparse->values;
  const uint32_t end = upper_bound32(values, sparse->cardinality, max);
  uint32_t i = lower_bound32(values, sparse->cardinality, min);
  if (is_sparse(other)) {
    // both small and sorted: one walk through each
    uint32_t j = other->cardinality ? lower_bound32(other->values, other->cardinality, min) : 0u;
    for (; i < end; ++i) {
      while (j < other->cardinality && other->values[j] < values[i]) { ++j; }
      const bool has = j < other->cardinality && other->values[j] == values[i];
      if (has == keep) { kept[count++] = values[i]; }
    }
    return values_result(out, kept, count, pool);
  }
  // the other set's containers found once per key, not once per value
  uint32_t c = i < end ? first_container(other, key_of(values[i])) : 0u;
  while (i < end) {
    const uint16_t key = key_of(values[i]);
    while (c < other->count && other->containers[c].key < key) { ++c; }
    const arnm_roaring_container *container =
        c < other->count && other->containers[c].key == key ? &other->containers[c] : NULL;
    // this key's values, then each kept or dropped without a branch on the answer
    uint32_t key_end = i + 1u;
    while (key_end < end && key_of(values[key_end]) == key) { ++key_end; }
    const uint32_t flip = keep ? 0u : 1u;
    if (container && ARNM_ROARING_BITMAP == container->kind) {
      const uint64_t *words = words_of(container);
      for (; i < key_end; ++i) {
        const uint16_t low = low_of(values[i]);
        kept[count] = values[i];
        count += (uint32_t)((words[low >> 6] >> (low & 63u)) & 1u) ^ flip;
      }
    } else {
      uint32_t index = 0;
      for (; i < key_end; ++i) {
        const bool has = container && container_has(container, low_of(values[i]), &index);
        kept[count] = values[i];
        count += (uint32_t)has ^ flip;
      }
    }
  }
  return values_result(out, kept, count, pool);
}

arnm_result arnm_roaring_and(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  arnm_result result = check_operation(out, a, b, pool);
  if (ARNM_SUCCESS != result || min > max) { return result; }
  if (is_sparse(a) || is_sparse(b)) {
    // the smaller sparse one is read, the other asked
    const bool read_a = is_sparse(a) && (!is_sparse(b) || a->cardinality <= b->cardinality);
    return finish(
        out, sparse_filter(out, read_a ? a : b, read_a ? b : a, true, min, max, pool), pool
    );
  }
  key_source sa, sb;
  source_begin(&sa, a, min, max, false, false);
  source_begin(&sb, b, min, max, false, false);
  while (sa.current && sb.current) {
    const arnm_roaring_container *x = sa.current;
    const arnm_roaring_container *y = sb.current;
    if (x->key < y->key) {
      source_advance(&sa);
      continue;
    }
    if (x->key > y->key) {
      source_advance(&sb);
      continue;
    }
    uint16_t low, high;
    container_bounds(x->key, min, max, &low, &high);
    const clip cx = make_clip(x, low, high);
    const clip cy = make_clip(y, low, high);
    if (ARNM_ROARING_ARRAY == x->kind && ARNM_ROARING_ARRAY == y->kind) {
      result = and_arrays(out, &cx, &cy, pool);
    } else if (ARNM_ROARING_ARRAY == x->kind) {
      result = filter_array_by_words(out, &cx, &cy, true, pool);
    } else if (ARNM_ROARING_ARRAY == y->kind) {
      result = filter_array_by_words(out, &cy, &cx, true, pool);
    } else {
      result = combine_words(out, &cx, &cy, WORD_AND, pool);
    }
    if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
    source_advance(&sa);
    source_advance(&sb);
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_or(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  arnm_result result = check_operation(out, a, b, pool);
  if (ARNM_SUCCESS != result || min > max) { return result; }
  if (is_sparse(a) && is_sparse(b)) {
    // two small sorted arrays: merged, then sparse or containers by what came out
    uint32_t merged[2u * ARNM_ROARING_SPARSE_MAX];
    const arnm_roaring_bitmap *sets[2] = {a, b};
    sparse_spans spans;
    sparse_spans_of(sets, 2, min, max, &spans);
    const uint32_t count =
        spans.count ? sparse_union_page(&spans, 0, 2u * ARNM_ROARING_SPARSE_MAX, false, merged)
                    : 0u;
    return finish(out, values_result(out, merged, count, pool), pool);
  }
  key_source sa, sb;
  source_begin(&sa, a, min, max, false, false);
  source_begin(&sb, b, min, max, false, false);
  for (;;) {
    const arnm_roaring_container *x = sa.current;
    const arnm_roaring_container *y = sb.current;
    if (!x && !y) { break; }
    uint16_t low, high;
    if (!y || (x && x->key < y->key)) {
      container_bounds(x->key, min, max, &low, &high);
      const clip cx = make_clip(x, low, high);
      result = emit_clip(out, &cx, pool);
      if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
      source_advance(&sa);
      continue;
    }
    if (!x || y->key < x->key) {
      container_bounds(y->key, min, max, &low, &high);
      const clip cy = make_clip(y, low, high);
      result = emit_clip(out, &cy, pool);
      if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
      source_advance(&sb);
      continue;
    }
    container_bounds(x->key, min, max, &low, &high);
    const clip cx = make_clip(x, low, high);
    const clip cy = make_clip(y, low, high);
    if (ARNM_ROARING_ARRAY == x->kind && ARNM_ROARING_ARRAY == y->kind) {
      result = or_arrays(out, &cx, &cy, pool);
    } else if (ARNM_ROARING_ARRAY == x->kind) {
      result = words_with_array(out, &cy, &cx, true, pool);
    } else if (ARNM_ROARING_ARRAY == y->kind) {
      result = words_with_array(out, &cx, &cy, true, pool);
    } else {
      result = combine_words(out, &cx, &cy, WORD_OR, pool);
    }
    if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
    source_advance(&sa);
    source_advance(&sb);
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_andnot(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  arnm_result result = check_operation(out, a, b, pool);
  if (ARNM_SUCCESS != result || min > max) { return result; }
  if (is_sparse(a)) { return finish(out, sparse_filter(out, a, b, false, min, max, pool), pool); }
  key_source sa, sb;
  source_begin(&sa, a, min, max, false, false);
  source_begin(&sb, b, min, max, false, false);
  for (; sa.current; source_advance(&sa)) {
    const arnm_roaring_container *x = sa.current;
    while (sb.current && sb.current->key < x->key) { source_advance(&sb); }
    uint16_t low, high;
    container_bounds(x->key, min, max, &low, &high);
    const clip cx = make_clip(x, low, high);
    if (!sb.current || sb.current->key != x->key) {
      result = emit_clip(out, &cx, pool);
    } else {
      const arnm_roaring_container *y = sb.current;
      const clip cy = make_clip(y, low, high);
      if (ARNM_ROARING_ARRAY == x->kind && ARNM_ROARING_ARRAY == y->kind) {
        result = andnot_arrays(out, &cx, &cy, pool);
      } else if (ARNM_ROARING_ARRAY == x->kind) {
        result = filter_array_by_words(out, &cx, &cy, false, pool);
      } else if (ARNM_ROARING_ARRAY == y->kind) {
        result = words_with_array(out, &cx, &cy, false, pool);
      } else {
        result = combine_words(out, &cx, &cy, WORD_ANDNOT, pool);
      }
    }
    if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_copy_range(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  arnm_result result = check_operation(out, a, a, pool);
  if (ARNM_SUCCESS != result || min > max) { return result; }
  if (is_sparse(a)) {
    if (!a->cardinality) { return ARNM_SUCCESS; }
    const uint32_t begin = lower_bound32(a->values, a->cardinality, min);
    const uint32_t end = upper_bound32(a->values, a->cardinality, max);
    return finish(out, values_result(out, a->values + begin, end - begin, pool), pool);
  }
  key_source source;
  source_begin(&source, a, min, max, false, false);
  for (; source.current; source_advance(&source)) {
    uint16_t low, high;
    container_bounds(source.current->key, min, max, &low, &high);
    const clip c = make_clip(source.current, low, high);
    result = emit_clip(out, &c, pool);
    if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
  }
  return ARNM_SUCCESS;
}
