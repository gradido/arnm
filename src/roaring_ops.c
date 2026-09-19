#include "arnm/roaring_ops.h"

#include "roaring_intern.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The operations that build a new set: and, or, andnot and copy_range. Each writes into an empty
 * set the caller provides and takes every block from the pool; a failure leaves that set empty
 * again. Where an input is sparse -- and a sparse set is small -- the result is built straight
 * as a sparse set instead of going through containers.
 */

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
    const uint16_t *array = arnm_roaring_array_of(container);
    result.begin = low ? arnm_roaring_lower_bound16(array, container->cardinality, low) : 0u;
    result.end = 0xffffu == high ? container->cardinality
                                 : arnm_roaring_upper_bound16(array, container->cardinality, high);
  }
  return result;
}

static arnm_result emit_clip(
    arnm_roaring_bitmap *out, const clip *c, arnm_graded_block_pool *pool
) {
  const arnm_roaring_container *container = c->container;
  if (ARNM_ROARING_ARRAY == container->kind) {
    return arnm_roaring_emit_array(
        out, container->key, arnm_roaring_array_of(container) + c->begin, c->end - c->begin, pool
    );
  }
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  const uint32_t first = c->low >> 6;
  const uint32_t last = c->high >> 6;
  memcpy(
      words + first, arnm_roaring_words_of(container) + first,
      (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  const uint32_t cardinality = arnm_roaring_clip_and_count(words, c->low, c->high);
  return arnm_roaring_emit_words(out, container->key, words, first, last, cardinality, pool);
}

static arnm_result and_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = arnm_roaring_array_of(a->container) + a->begin;
  const uint16_t *y = arnm_roaring_array_of(b->container) + b->begin;
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
      from += arnm_roaring_lower_bound16(y + from, ny - from, x[i]);
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
  return arnm_roaring_emit_array(out, a->container->key, values, count, pool);
}

/** Values of array @p a whose bit in bitmap @p b is set (@p keep) or clear (!@p keep). */
static arnm_result filter_array_by_words(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, bool keep, arnm_graded_block_pool *pool
) {
  const uint16_t *x = arnm_roaring_array_of(a->container);
  const uint64_t *words = arnm_roaring_words_of(b->container);
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  // a branch, not the branchless append sparse_filter uses: measured, the array's values run in
  // long streaks of kept or dropped often enough that the branch is the faster of the two here
  for (uint32_t i = a->begin; i < a->end; ++i) {
    const bool set = (words[x[i] >> 6] >> (x[i] & 63u)) & 1u;
    if (set == keep) { values[count++] = x[i]; }
  }
  return arnm_roaring_emit_array(out, a->container->key, values, count, pool);
}

typedef enum word_op { WORD_AND, WORD_OR, WORD_ANDNOT } word_op;

/* the operation is a constant at every call site, so each loop compiles without the switch */
static inline arnm_result combine_words(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, word_op op, arnm_graded_block_pool *pool
) {
  const uint64_t *x = arnm_roaring_words_of(a->container);
  const uint64_t *y = arnm_roaring_words_of(b->container);
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
  const uint32_t cardinality = arnm_roaring_clip_and_count(words, a->low, a->high);
  return arnm_roaring_emit_words(out, a->container->key, words, first, last, cardinality, pool);
}

/** Bitmap @p a with the values of array @p b set (@p set) or cleared, within a's range. */
static arnm_result words_with_array(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, bool set, arnm_graded_block_pool *pool
) {
  const uint16_t *y = arnm_roaring_array_of(b->container);
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  const uint32_t first = a->low >> 6;
  const uint32_t last = a->high >> 6;
  memcpy(
      words + first, arnm_roaring_words_of(a->container) + first,
      (size_t)(last - first + 1u) * sizeof(uint64_t)
  );
  // b is clipped to the same range, so every value lands inside [first, last]
  if (set) {
    for (uint32_t i = b->begin; i < b->end; ++i) { words[y[i] >> 6] |= 1ull << (y[i] & 63u); }
  } else {
    for (uint32_t i = b->begin; i < b->end; ++i) { words[y[i] >> 6] &= ~(1ull << (y[i] & 63u)); }
  }
  const uint32_t cardinality = arnm_roaring_clip_and_count(words, a->low, a->high);
  return arnm_roaring_emit_words(out, a->container->key, words, first, last, cardinality, pool);
}

static arnm_result or_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = arnm_roaring_array_of(a->container);
  const uint16_t *y = arnm_roaring_array_of(b->container);
  if ((a->end - a->begin) + (b->end - b->begin) > ARNM_ROARING_ARRAY_MAX) {
    // may pass 4096: straight into bits, arnm_roaring_emit_words decides the kind by the real count
    uint64_t words[ARNM_ROARING_BITMAP_WORDS];
    const uint32_t first = a->low >> 6;
    const uint32_t last = a->high >> 6;
    memset(words + first, 0, (size_t)(last - first + 1u) * sizeof(uint64_t));
    for (uint32_t i = a->begin; i < a->end; ++i) { words[x[i] >> 6] |= 1ull << (x[i] & 63u); }
    for (uint32_t j = b->begin; j < b->end; ++j) { words[y[j] >> 6] |= 1ull << (y[j] & 63u); }
    uint32_t cardinality = 0;
    for (uint32_t w = first; w <= last; ++w) { cardinality += (uint32_t)arnm_popcountll(words[w]); }
    return arnm_roaring_emit_words(out, a->container->key, words, first, last, cardinality, pool);
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
  return arnm_roaring_emit_array(out, a->container->key, values, count, pool);
}

static arnm_result andnot_arrays(
    arnm_roaring_bitmap *out, const clip *a, const clip *b, arnm_graded_block_pool *pool
) {
  const uint16_t *x = arnm_roaring_array_of(a->container);
  const uint16_t *y = arnm_roaring_array_of(b->container);
  uint16_t values[ARNM_ROARING_ARRAY_MAX];
  uint32_t count = 0;
  uint32_t j = b->begin;
  for (uint32_t i = a->begin; i < a->end; ++i) {
    while (j < b->end && y[j] < x[i]) { ++j; }
    if (j == b->end || y[j] != x[i]) { values[count++] = x[i]; }
  }
  return arnm_roaring_emit_array(out, a->container->key, values, count, pool);
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
    return (arnm_roaring_words_of(container)[low >> 6] >> (low & 63u)) & 1u;
  }
  const uint16_t *array = arnm_roaring_array_of(container);
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
  const uint32_t end = arnm_roaring_upper_bound32(values, sparse->cardinality, max);
  uint32_t i = arnm_roaring_lower_bound32(values, sparse->cardinality, min);
  if (arnm_roaring_is_sparse(other)) {
    // both small and sorted: one walk through each
    uint32_t j = other->cardinality
                     ? arnm_roaring_lower_bound32(other->values, other->cardinality, min)
                     : 0u;
    for (; i < end; ++i) {
      while (j < other->cardinality && other->values[j] < values[i]) { ++j; }
      const bool has = j < other->cardinality && other->values[j] == values[i];
      if (has == keep) { kept[count++] = values[i]; }
    }
    return arnm_roaring_values_result(out, kept, count, pool);
  }
  // the other set's containers found once per key, not once per value
  uint32_t c = i < end ? arnm_roaring_first_container(other, arnm_roaring_key_of(values[i])) : 0u;
  while (i < end) {
    const uint16_t key = arnm_roaring_key_of(values[i]);
    while (c < other->count && other->containers[c].key < key) { ++c; }
    const arnm_roaring_container *container =
        c < other->count && other->containers[c].key == key ? &other->containers[c] : NULL;
    // this key's values, then each kept or dropped without a branch on the answer
    uint32_t key_end = i + 1u;
    while (key_end < end && arnm_roaring_key_of(values[key_end]) == key) { ++key_end; }
    const uint32_t flip = keep ? 0u : 1u;
    if (container && ARNM_ROARING_BITMAP == container->kind) {
      const uint64_t *words = arnm_roaring_words_of(container);
      for (; i < key_end; ++i) {
        const uint16_t low = arnm_roaring_low_of(values[i]);
        kept[count] = values[i];
        count += (uint32_t)((words[low >> 6] >> (low & 63u)) & 1u) ^ flip;
      }
    } else {
      uint32_t index = 0;
      for (; i < key_end; ++i) {
        const bool has =
            container && container_has(container, arnm_roaring_low_of(values[i]), &index);
        kept[count] = values[i];
        count += (uint32_t)has ^ flip;
      }
    }
  }
  return arnm_roaring_values_result(out, kept, count, pool);
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
  if (arnm_roaring_is_sparse(a) || arnm_roaring_is_sparse(b)) {
    // the smaller sparse one is read, the other asked
    const bool read_a = arnm_roaring_is_sparse(a) &&
                        (!arnm_roaring_is_sparse(b) || a->cardinality <= b->cardinality);
    return finish(
        out, sparse_filter(out, read_a ? a : b, read_a ? b : a, true, min, max, pool), pool
    );
  }
  arnm_roaring_source sa, sb;
  uint16_t buffer_a[ARNM_ROARING_SPARSE_MAX], buffer_b[ARNM_ROARING_SPARSE_MAX];
  arnm_roaring_source_begin(&sa, a, min, max, false, buffer_a);
  arnm_roaring_source_begin(&sb, b, min, max, false, buffer_b);
  while (sa.current && sb.current) {
    const arnm_roaring_container *x = sa.current;
    const arnm_roaring_container *y = sb.current;
    if (x->key < y->key) {
      arnm_roaring_source_advance(&sa);
      continue;
    }
    if (x->key > y->key) {
      arnm_roaring_source_advance(&sb);
      continue;
    }
    uint16_t low, high;
    arnm_roaring_container_bounds(x->key, min, max, &low, &high);
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
    arnm_roaring_source_advance(&sa);
    arnm_roaring_source_advance(&sb);
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
  if (arnm_roaring_is_sparse(a) && arnm_roaring_is_sparse(b)) {
    // two small sorted arrays: merged in one walk, then sparse or containers by what came out
    uint32_t merged[2u * ARNM_ROARING_SPARSE_MAX];
    uint32_t count = 0;
    uint32_t i = arnm_roaring_lower_bound32(a->values, a->cardinality, min);
    uint32_t j = arnm_roaring_lower_bound32(b->values, b->cardinality, min);
    const uint32_t end_a = arnm_roaring_upper_bound32(a->values, a->cardinality, max);
    const uint32_t end_b = arnm_roaring_upper_bound32(b->values, b->cardinality, max);
    while (i < end_a && j < end_b) {
      if (a->values[i] < b->values[j]) {
        merged[count++] = a->values[i++];
      } else if (b->values[j] < a->values[i]) {
        merged[count++] = b->values[j++];
      } else {
        merged[count++] = a->values[i++];
        ++j;
      }
    }
    while (i < end_a) { merged[count++] = a->values[i++]; }
    while (j < end_b) { merged[count++] = b->values[j++]; }
    return finish(out, arnm_roaring_values_result(out, merged, count, pool), pool);
  }
  arnm_roaring_source sa, sb;
  uint16_t buffer_a[ARNM_ROARING_SPARSE_MAX], buffer_b[ARNM_ROARING_SPARSE_MAX];
  arnm_roaring_source_begin(&sa, a, min, max, false, buffer_a);
  arnm_roaring_source_begin(&sb, b, min, max, false, buffer_b);
  for (;;) {
    const arnm_roaring_container *x = sa.current;
    const arnm_roaring_container *y = sb.current;
    if (!x && !y) { break; }
    uint16_t low, high;
    if (!y || (x && x->key < y->key)) {
      arnm_roaring_container_bounds(x->key, min, max, &low, &high);
      const clip cx = make_clip(x, low, high);
      result = emit_clip(out, &cx, pool);
      if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
      arnm_roaring_source_advance(&sa);
      continue;
    }
    if (!x || y->key < x->key) {
      arnm_roaring_container_bounds(y->key, min, max, &low, &high);
      const clip cy = make_clip(y, low, high);
      result = emit_clip(out, &cy, pool);
      if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
      arnm_roaring_source_advance(&sb);
      continue;
    }
    arnm_roaring_container_bounds(x->key, min, max, &low, &high);
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
    arnm_roaring_source_advance(&sa);
    arnm_roaring_source_advance(&sb);
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
  if (arnm_roaring_is_sparse(a)) {
    return finish(out, sparse_filter(out, a, b, false, min, max, pool), pool);
  }
  arnm_roaring_source sa, sb;
  uint16_t buffer_a[ARNM_ROARING_SPARSE_MAX], buffer_b[ARNM_ROARING_SPARSE_MAX];
  arnm_roaring_source_begin(&sa, a, min, max, false, buffer_a);
  arnm_roaring_source_begin(&sb, b, min, max, false, buffer_b);
  for (; sa.current; arnm_roaring_source_advance(&sa)) {
    const arnm_roaring_container *x = sa.current;
    while (sb.current && sb.current->key < x->key) { arnm_roaring_source_advance(&sb); }
    uint16_t low, high;
    arnm_roaring_container_bounds(x->key, min, max, &low, &high);
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
  if (arnm_roaring_is_sparse(a)) {
    if (!a->cardinality) { return ARNM_SUCCESS; }
    const uint32_t begin = arnm_roaring_lower_bound32(a->values, a->cardinality, min);
    const uint32_t end = arnm_roaring_upper_bound32(a->values, a->cardinality, max);
    return finish(out, arnm_roaring_values_result(out, a->values + begin, end - begin, pool), pool);
  }
  arnm_roaring_source source;
  uint16_t buffer[ARNM_ROARING_SPARSE_MAX];
  arnm_roaring_source_begin(&source, a, min, max, false, buffer);
  for (; source.current; arnm_roaring_source_advance(&source)) {
    uint16_t low, high;
    arnm_roaring_container_bounds(source.current->key, min, max, &low, &high);
    const clip c = make_clip(source.current, low, high);
    result = emit_clip(out, &c, pool);
    if (ARNM_SUCCESS != result) { return finish(out, result, pool); }
  }
  return ARNM_SUCCESS;
}