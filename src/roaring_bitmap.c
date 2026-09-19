#include "arnm/roaring_bitmap.h"

#include "roaring_intern.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The set itself: how it is built, which of its two forms it is in, and how one set is read.
 * The layout, with containers: a directory of 16 byte container entries, sorted by key, in one
 * pool block that doubles; each entry points at its own pool block with the values. Sparse: one
 * pool block of sorted uint32_t values that doubles, and nothing else. `count` tells the two
 * apart -- a set with containers has at least one -- so a set being built in either form is
 * never mistaken for the other, whatever its cardinality says on the way.
 *
 * Normalised, a set is sparse exactly while it holds at most ARNM_ROARING_SPARSE_MAX values:
 * add turns it into containers past that. An operation in arnm/roaring_ops.h builds a sparse
 * result only up to that many as well, so the buffers the fast paths keep on the stack always
 * fit one; the other way round is not promised.
 */

/** Word and bit of the @p rank-th set bit (0 based) of a bitmap container. */
static void arnm_roaring_words_select(
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
  if (arnm_roaring_is_sparse(set)) {
    if (set->values && pool) {
      arnm_roaring_block_free(pool, (uint8_t *)set->values, set->directory_log2);
    }
  } else if (pool) {
    for (uint32_t i = 0; i < set->count; ++i) {
      arnm_roaring_block_free(pool, set->containers[i].data, set->containers[i].block_log2);
    }
    arnm_roaring_block_free(pool, (uint8_t *)set->containers, set->directory_log2);
  }
  memset(set, 0, sizeof(*set));
}

/** Append @p container to the directory, doubling it when full. The set is unchanged on failure. */
arnm_result arnm_roaring_push_container(
    arnm_roaring_bitmap *set, const arnm_roaring_container *container, arnm_graded_block_pool *pool
) {
  const uint32_t capacity = set->containers ? ((uint32_t)1u << set->directory_log2) /
                                                  (uint32_t)sizeof(arnm_roaring_container)
                                            : 0u;
  if (set->count == capacity) {
    const uint8_t log2 = set->containers ? (uint8_t)(set->directory_log2 + 1u)
                                         : (uint8_t)ROARING_DIRECTORY_LOG2_FIRST;
    uint8_t *block = NULL;
    const arnm_result result = arnm_roaring_block_alloc(pool, log2, &block);
    if (ARNM_SUCCESS != result) { return result; }
    if (set->containers) {
      memcpy(block, set->containers, (size_t)set->count * sizeof(arnm_roaring_container));
      arnm_roaring_block_free(pool, (uint8_t *)set->containers, set->directory_log2);
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
  const arnm_result result = arnm_roaring_block_alloc(pool, ROARING_BITMAP_LOG2, &block);
  if (ARNM_SUCCESS != result) { return result; }
  memset(block, 0, ROARING_BITMAP_BYTES);
  uint64_t *words = (uint64_t *)(void *)block;
  const uint16_t *array = arnm_roaring_array_of(last);
  for (uint32_t i = 0; i < last->cardinality; ++i) {
    words[array[i] >> 6] |= 1ull << (array[i] & 63u);
  }
  words[low >> 6] |= 1ull << (low & 63u);
  arnm_roaring_block_free(pool, last->data, last->block_log2);
  last->data = block;
  last->block_log2 = ROARING_BITMAP_LOG2;
  last->kind = ARNM_ROARING_BITMAP;
  return ARNM_SUCCESS;
}

/** A full array below 4096 values moves into a block twice the size. */
static arnm_result array_grow(arnm_roaring_container *last, arnm_graded_block_pool *pool) {
  uint8_t *block = NULL;
  const uint8_t log2 = (uint8_t)(last->block_log2 + 1u);
  const arnm_result result = arnm_roaring_block_alloc(pool, log2, &block);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(block, last->data, (size_t)last->cardinality * sizeof(uint16_t));
  arnm_roaring_block_free(pool, last->data, last->block_log2);
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
    const arnm_result result = arnm_roaring_block_alloc(pool, log2, &block);
    if (ARNM_SUCCESS != result) { return result; }
    if (set->values) {
      memcpy(block, set->values, (size_t)set->cardinality * sizeof(uint32_t));
      arnm_roaring_block_free(pool, (uint8_t *)set->values, set->directory_log2);
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
  if (arnm_roaring_is_sparse(set)) {
    if (set->cardinality < ARNM_ROARING_SPARSE_MAX) { return sparse_add(set, value, pool); }
    // one past the limit: containers from here on, the value added to them below
    const arnm_result result = sparse_to_containers(set, pool);
    if (ARNM_SUCCESS != result) { return result; }
  }
  const uint16_t key = arnm_roaring_key_of(value);
  const uint16_t low = arnm_roaring_low_of(value);

  if (set->count && set->containers[set->count - 1u].key == key) {
    arnm_roaring_container *last = &set->containers[set->count - 1u];
    if (ARNM_ROARING_BITMAP == last->kind) {
      arnm_roaring_words_of(last)[low >> 6] |= 1ull << (low & 63u);
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
        arnm_roaring_array_of(last)[last->cardinality] = low;
      }
    }
    last->cardinality++;
  } else {
    arnm_roaring_container container = {0};
    arnm_result result = arnm_roaring_block_alloc(pool, ROARING_ARRAY_LOG2_FIRST, &container.data);
    if (ARNM_SUCCESS != result) { return result; }
    container.key = key;
    container.kind = ARNM_ROARING_ARRAY;
    container.block_log2 = ROARING_ARRAY_LOG2_FIRST;
    container.cardinality = 1;
    arnm_roaring_array_of(&container)[0] = low;
    result = arnm_roaring_push_container(set, &container, pool);
    if (ARNM_SUCCESS != result) {
      arnm_roaring_block_free(pool, container.data, container.block_log2);
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

arnm_result arnm_roaring_emit_array(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint16_t *values,
    uint32_t count,
    arnm_graded_block_pool *pool
);

/** Words valid in [first, last]; everything outside is taken as zero. */
arnm_result arnm_roaring_emit_words(
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
    return arnm_roaring_emit_array(out, key, values, count, pool);
  }
  if (out->cardinality > UINT32_MAX - cardinality) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  arnm_roaring_container container = {0};
  arnm_result result = arnm_roaring_block_alloc(pool, ROARING_BITMAP_LOG2, &container.data);
  if (ARNM_SUCCESS != result) { return result; }
  memset(container.data, 0, ROARING_BITMAP_BYTES);
  memcpy(
      arnm_roaring_words_of(&container) + first, words + first,
      (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  container.key = key;
  container.kind = ARNM_ROARING_BITMAP;
  container.block_log2 = ROARING_BITMAP_LOG2;
  container.cardinality = cardinality;
  result = arnm_roaring_push_container(out, &container, pool);
  if (ARNM_SUCCESS != result) {
    arnm_roaring_block_free(pool, container.data, container.block_log2);
    return result;
  }
  uint32_t top = last;
  while (!words[top]) { --top; }
  out->cardinality += cardinality;
  out->maximum = arnm_roaring_value_of(key, (top << 6) | (uint32_t)(63 - arnm_clzll(words[top])));
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_emit_array(
    arnm_roaring_bitmap *out,
    uint16_t key,
    const uint16_t *values,
    uint32_t count,
    arnm_graded_block_pool *pool
) {
  if (!count) { return ARNM_SUCCESS; }
  if (out->cardinality > UINT32_MAX - count) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  arnm_roaring_container container = {0};
  const uint8_t log2 = arnm_roaring_log2_for_bytes(count * (uint32_t)sizeof(uint16_t));
  arnm_result result = arnm_roaring_block_alloc(pool, log2, &container.data);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(container.data, values, (size_t)count * sizeof(uint16_t));
  container.key = key;
  container.kind = ARNM_ROARING_ARRAY;
  container.block_log2 = log2;
  container.cardinality = count;
  result = arnm_roaring_push_container(out, &container, pool);
  if (ARNM_SUCCESS != result) {
    arnm_roaring_block_free(pool, container.data, container.block_log2);
    return result;
  }
  out->cardinality += count;
  out->maximum = arnm_roaring_value_of(key, values[count - 1u]);
  return ARNM_SUCCESS;
}

// ********** changing form *******************

/**
 * Appends @p values, sorted, to @p out as containers, one array per key: the form a sparse set
 * or a sparse result takes once it is too large. At most two sparse sets' worth of values, so
 * never 4096 in one key.
 */
arnm_result arnm_roaring_values_to_containers(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
) {
  uint16_t lows[2u * ARNM_ROARING_SPARSE_MAX];
  uint32_t i = 0;
  while (i < count) {
    const uint16_t key = arnm_roaring_key_of(values[i]);
    uint32_t n = 0;
    while (i < count && arnm_roaring_key_of(values[i]) == key) {
      lows[n++] = arnm_roaring_low_of(values[i++]);
    }
    const arnm_result result = arnm_roaring_emit_array(out, key, lows, n, pool);
    if (ARNM_SUCCESS != result) { return result; }
  }
  return ARNM_SUCCESS;
}

/** A sparse set becomes containers. Unchanged on failure. */
static arnm_result sparse_to_containers(arnm_roaring_bitmap *set, arnm_graded_block_pool *pool) {
  arnm_roaring_bitmap built;
  memset(&built, 0, sizeof(built));
  const arnm_result result =
      arnm_roaring_values_to_containers(&built, set->values, set->cardinality, pool);
  if (ARNM_SUCCESS != result) {
    arnm_roaring_free(&built, pool);
    return result;
  }
  arnm_roaring_block_free(pool, (uint8_t *)set->values, set->directory_log2);
  *set = built;
  return ARNM_SUCCESS;
}

/** @p out, empty, as the sparse or container set of @p values, sorted. */
arnm_result arnm_roaring_values_result(
    arnm_roaring_bitmap *out, const uint32_t *values, uint32_t count, arnm_graded_block_pool *pool
) {
  if (!count) { return ARNM_SUCCESS; }
  if (count > ARNM_ROARING_SPARSE_MAX) {
    return arnm_roaring_values_to_containers(out, values, count, pool);
  }
  const uint8_t log2 = arnm_roaring_log2_for_bytes(count * (uint32_t)sizeof(uint32_t));
  uint8_t *block = NULL;
  const arnm_result result = arnm_roaring_block_alloc(pool, log2, &block);
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
  if (arnm_roaring_is_sparse(set)) {
    *out = set->values[0];
    return true;
  }
  const arnm_roaring_container *first = &set->containers[0];
  if (ARNM_ROARING_ARRAY == first->kind) {
    *out = arnm_roaring_value_of(first->key, arnm_roaring_array_of(first)[0]);
    return true;
  }
  const uint64_t *words = arnm_roaring_words_of(first);
  uint32_t w = 0;
  while (!words[w]) { ++w; }
  *out = arnm_roaring_value_of(first->key, (w << 6) | (uint32_t)arnm_ctzll(words[w]));
  return true;
}

bool arnm_roaring_maximum(const arnm_roaring_bitmap *set, uint32_t *out) {
  if (!set || !set->cardinality) { return false; }
  *out = set->maximum;
  return true;
}

bool arnm_roaring_contains(const arnm_roaring_bitmap *set, uint32_t value) {
  if (!set) { return false; }
  if (arnm_roaring_is_sparse(set)) {
    const uint32_t position = arnm_roaring_lower_bound32(set->values, set->cardinality, value);
    return position < set->cardinality && set->values[position] == value;
  }
  const uint16_t key = arnm_roaring_key_of(value);
  const uint16_t low = arnm_roaring_low_of(value);
  const uint32_t index = arnm_roaring_first_container(set, key);
  if (index == set->count || set->containers[index].key != key) { return false; }
  const arnm_roaring_container *container = &set->containers[index];
  if (ARNM_ROARING_BITMAP == container->kind) {
    return (arnm_roaring_words_of(container)[low >> 6] >> (low & 63u)) & 1u;
  }
  const uint32_t position =
      arnm_roaring_lower_bound16(arnm_roaring_array_of(container), container->cardinality, low);
  return position < container->cardinality && arnm_roaring_array_of(container)[position] == low;
}

uint32_t arnm_roaring_container_range_cardinality(
    const arnm_roaring_container *container, uint16_t low, uint16_t high
) {
  if (0u == low && 0xffffu == high) { return container->cardinality; }
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = arnm_roaring_wide_of(container);
    return arnm_roaring_upper_bound32(
               wide, container->cardinality, arnm_roaring_value_of(container->key, high)
           ) -
           arnm_roaring_lower_bound32(
               wide, container->cardinality, arnm_roaring_value_of(container->key, low)
           );
  }
  if (ARNM_ROARING_ARRAY == container->kind) {
    const uint16_t *array = arnm_roaring_array_of(container);
    return arnm_roaring_upper_bound16(array, container->cardinality, high) -
           arnm_roaring_lower_bound16(array, container->cardinality, low);
  }
  return arnm_roaring_words_cardinality(arnm_roaring_words_of(container), low, high);
}

uint32_t arnm_roaring_range_cardinality(
    const arnm_roaring_bitmap *set, uint32_t min, uint32_t max
) {
  if (!set || min > max) { return 0u; }
  if (arnm_roaring_is_sparse(set)) {
    return arnm_roaring_upper_bound32(set->values, set->cardinality, max) -
           arnm_roaring_lower_bound32(set->values, set->cardinality, min);
  }
  const uint16_t max_key = arnm_roaring_key_of(max);
  uint32_t total = 0;
  for (uint32_t i = arnm_roaring_first_container(set, arnm_roaring_key_of(min));
       i < set->count && set->containers[i].key <= max_key; ++i) {
    uint16_t low, high;
    arnm_roaring_container_bounds(set->containers[i].key, min, max, &low, &high);
    total += arnm_roaring_container_range_cardinality(&set->containers[i], low, high);
  }
  return total;
}

bool arnm_roaring_select(const arnm_roaring_bitmap *set, uint32_t rank, uint32_t *out) {
  if (!set || rank >= set->cardinality) { return false; }
  if (arnm_roaring_is_sparse(set)) {
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
      *out = arnm_roaring_value_of(container->key, arnm_roaring_array_of(container)[rank]);
    } else {
      uint32_t w, bit;
      arnm_roaring_words_select(arnm_roaring_words_of(container), rank, &w, &bit);
      *out = arnm_roaring_value_of(container->key, (w << 6) | bit);
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
      const uint16_t *array = arnm_roaring_array_of(container);
      for (uint32_t i = rank; i < container->cardinality && written < size; ++i) {
        out[written++] = arnm_roaring_value_of(container->key, array[i]);
      }
      continue;
    }
    const uint64_t *words = arnm_roaring_words_of(container);
    uint32_t w, bit;
    arnm_roaring_words_select(words, rank, &w, &bit);
    uint64_t word = words[w] & (~0ull << bit);
    for (;;) {
      while (word && written < size) {
        out[written++] =
            arnm_roaring_value_of(container->key, (w << 6) | (uint32_t)arnm_ctzll(word));
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
      const uint16_t *array = arnm_roaring_array_of(container);
      for (uint32_t i = rank + 1u; i > 0u && written < size; --i) {
        out[written++] = arnm_roaring_value_of(container->key, array[i - 1u]);
      }
    } else {
      const uint64_t *words = arnm_roaring_words_of(container);
      uint32_t w, bit;
      arnm_roaring_words_select(words, rank, &w, &bit);
      uint64_t word = words[w] & (~0ull >> (63u - bit));
      for (;;) {
        while (word && written < size) {
          const uint32_t top = 63u - (uint32_t)arnm_clzll(word);
          out[written++] = arnm_roaring_value_of(container->key, (w << 6) | top);
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
  if (arnm_roaring_is_sparse(set)) {
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

// ********** a set read key by key *******************

void arnm_roaring_source_load(arnm_roaring_source *source) {
  const arnm_roaring_bitmap *set = source->set;
  source->current = NULL;
  if (!set) { return; }
  if (!arnm_roaring_is_sparse(set)) {
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
    key = arnm_roaring_key_of(values[source->position - 1u]);
    if (key < source->min_key) { return; }
    end = source->position;
    begin = arnm_roaring_lower_bound32(values, end, (uint32_t)key << 16);
    source->next = begin;
  } else {
    if (source->position >= set->cardinality) { return; }
    key = arnm_roaring_key_of(values[source->position]);
    if (key > source->max_key) { return; }
    begin = source->position;
    end = begin + arnm_roaring_upper_bound32(
                      values + begin, set->cardinality - begin, arnm_roaring_value_of(key, 0xffffu)
                  );
    source->next = end;
  }
  source->view.cardinality = end - begin;
  source->view.key = key;
  source->view.block_log2 = 0;
  if (!source->buffer) {
    source->view.data = (uint8_t *)(uintptr_t)(values + begin);
    source->view.kind = ROARING_KIND_WIDE;
  } else {
    for (uint32_t i = begin; i < end; ++i) {
      source->buffer[i - begin] = arnm_roaring_low_of(values[i]);
    }
    source->view.data = (uint8_t *)source->buffer;
    source->view.kind = ARNM_ROARING_ARRAY;
  }
  source->current = &source->view;
}

void arnm_roaring_source_begin(
    arnm_roaring_source *source,
    const arnm_roaring_bitmap *set,
    uint32_t min,
    uint32_t max,
    bool descending,
    uint16_t *buffer
) {
  source->set = set;
  source->buffer = buffer;
  source->min_key = arnm_roaring_key_of(min);
  source->max_key = arnm_roaring_key_of(max);
  source->descending = descending;
  source->position = 0;
  if (set) {
    if (!arnm_roaring_is_sparse(set)) {
      source->position = descending ? arnm_roaring_container_after(set, source->max_key)
                                    : arnm_roaring_first_container(set, source->min_key);
    } else {
      source->position =
          descending
              ? arnm_roaring_upper_bound32(
                    set->values, set->cardinality, arnm_roaring_value_of(source->max_key, 0xffffu)
                )
              : arnm_roaring_lower_bound32(
                    set->values, set->cardinality, arnm_roaring_value_of(source->min_key, 0u)
                );
    }
  }
  arnm_roaring_source_load(source);
}

void arnm_roaring_source_advance(arnm_roaring_source *source) {
  if (!source->current) { return; }
  if (arnm_roaring_is_sparse(source->set)) {
    source->position = source->next;
  } else {
    source->position = source->descending ? source->position - 1u : source->position + 1u;
  }
  arnm_roaring_source_load(source);
}

/** Where each set stands while the union is walked key by key, in one direction. */
