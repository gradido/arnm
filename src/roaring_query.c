#include "arnm/roaring_query.h"

#include "roaring_intern.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Answers over several sets, without building one: how many values a range holds, a page of
 * them, the first or the last. Every set is walked key by key through an arnm_roaring_source,
 * and each key is answered with the cheapest way its parts allow -- merged value by value while
 * there are few, in bits once there are many.
 */

// ********** reading a union without building it *******************

/** The containers of one key the union has, and the part of the range that key covers. */
typedef struct key_union {
  const arnm_roaring_container *parts[ARNM_ROARING_QUERY_MAX]; /**< The sets that hold this key. */
  uint32_t count;                                              /**< Parts in use. */
  uint16_t key;    /**< The upper 16 bits the values share. */
  uint16_t low;    /**< Lowest low part the range leaves of this key. */
  uint16_t high;   /**< Highest low part the range leaves of this key. */
  bool any_bitmap; /**< At least one part is a bitmap, so bits are the cheaper merge. */
} key_union;

/**
 * The lowest and highest low part a container can hold, as far as it is known without a scan:
 * an array's or a view's first and last value, a bitmap's top if it is the last container of its
 * set -- that is the set's maximum -- and its whole width otherwise.
 */
static inline void part_reach(
    const arnm_roaring_source *source,
    const arnm_roaring_container *c,
    uint16_t *bottom,
    uint16_t *top
) {
  if (ARNM_ROARING_ARRAY == c->kind) {
    *bottom = arnm_roaring_array_of(c)[0];
    *top = arnm_roaring_array_of(c)[c->cardinality - 1u];
  } else if (ROARING_KIND_WIDE == c->kind) {
    *bottom = arnm_roaring_low_of(arnm_roaring_wide_of(c)[0]);
    *top = arnm_roaring_low_of(arnm_roaring_wide_of(c)[c->cardinality - 1u]);
  } else {
    const arnm_roaring_bitmap *set = source->set;
    *bottom = 0;
    *top = c == &set->containers[set->count - 1u] ? arnm_roaring_low_of(set->maximum) : 0xffffu;
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
  const uint32_t *values[ARNM_ROARING_QUERY_MAX]; /**< Each set's sorted values. */
  uint32_t begin[ARNM_ROARING_QUERY_MAX];         /**< First index inside the range. */
  uint32_t end[ARNM_ROARING_QUERY_MAX];           /**< One past the last index inside it. */
  uint32_t count;                                 /**< Spans in use. */
} sparse_spans;

/** Whether every set is sparse or missing, and so the union can be read off the arrays. */
static bool all_sparse(const arnm_roaring_bitmap *const *sets, uint32_t count) {
  for (uint32_t s = 0; s < count; ++s) {
    if (sets[s] && !arnm_roaring_is_sparse(sets[s])) { return false; }
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
    // no shortcut for an unbounded end: the searches already turn back on the first compare
    // when the bound lies outside the array, and a branch here only costs the bounded case
    const uint32_t begin = arnm_roaring_lower_bound32(set->values, set->cardinality, min);
    const uint32_t end = arnm_roaring_upper_bound32(set->values, set->cardinality, max);
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
    const uint16_t key = arnm_roaring_key_of(smallest);
    const uint32_t key_last = arnm_roaring_value_of(key, 0xffffu);
    // each span's part in this key, and every span moved past it
    in_key.count = 0;
    uint32_t values_in_key = 0;
    uint16_t low = 0xffffu, high = 0;
    for (uint32_t p = 0; p < spans->count;) {
      const uint32_t *values = spans->values[p];
      const uint32_t begin = spans->begin[p];
      if (arnm_roaring_key_of(values[begin]) == key) {
        const uint32_t end =
            begin + arnm_roaring_upper_bound32(values + begin, spans->end[p] - begin, key_last);
        in_key.values[in_key.count] = values;
        in_key.begin[in_key.count] = begin;
        in_key.end[in_key.count] = end;
        in_key.count++;
        values_in_key += end - begin;
        if (arnm_roaring_low_of(values[begin]) < low) { low = arnm_roaring_low_of(values[begin]); }
        if (arnm_roaring_low_of(values[end - 1u]) > high) {
          high = arnm_roaring_low_of(values[end - 1u]);
        }
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
          const uint16_t bit = arnm_roaring_low_of(in_key.values[p][i]);
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
  // the value each span stands on, kept beside the spans: a page of twenty asks for it twice
  // per span and value, and reading it again means a pointer and an index every time
  uint32_t head[ARNM_ROARING_QUERY_MAX];
  // ascending reads each span from its front, descending from its back
  for (uint32_t p = 0; p < spans->count; ++p) {
    head[p] = descending ? spans->values[p][spans->end[p] - 1u] : spans->values[p][spans->begin[p]];
  }
  while (written < size && spans->count) {
    uint32_t pick = head[0];
    for (uint32_t p = 1; p < spans->count; ++p) {
      if (descending ? head[p] > pick : head[p] < pick) { pick = head[p]; }
    }
    if (skip) {
      --skip;
    } else {
      out[written++] = pick;
    }
    for (uint32_t p = 0; p < spans->count;) {
      if (head[p] != pick) {
        ++p;
        continue;
      }
      // every span standing on the value moves on; one that runs out is dropped
      const bool empty =
          descending ? --spans->end[p] == spans->begin[p] : ++spans->begin[p] == spans->end[p];
      if (empty) {
        --spans->count;
        spans->values[p] = spans->values[spans->count];
        spans->begin[p] = spans->begin[spans->count];
        spans->end[p] = spans->end[spans->count];
        head[p] = head[spans->count];
        continue;
      }
      head[p] =
          descending ? spans->values[p][spans->end[p] - 1u] : spans->values[p][spans->begin[p]];
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
      const uint64_t *x = arnm_roaring_words_of(c);
      for (uint32_t w = first; w <= last; ++w) { words[w] |= x[w]; }
    } else if (ROARING_KIND_WIDE == c->kind) {
      const uint32_t *wide = arnm_roaring_wide_of(c);
      const uint32_t begin =
          arnm_roaring_lower_bound32(wide, c->cardinality, arnm_roaring_value_of(u->key, u->low));
      const uint32_t end =
          arnm_roaring_upper_bound32(wide, c->cardinality, arnm_roaring_value_of(u->key, u->high));
      for (uint32_t i = begin; i < end; ++i) {
        const uint16_t low = arnm_roaring_low_of(wide[i]);
        words[low >> 6] |= 1ull << (low & 63u);
      }
    } else {
      const uint16_t *array = arnm_roaring_array_of(c);
      const uint32_t begin = arnm_roaring_lower_bound16(array, c->cardinality, u->low);
      const uint32_t end = arnm_roaring_upper_bound16(array, c->cardinality, u->high);
      for (uint32_t i = begin; i < end; ++i) { words[array[i] >> 6] |= 1ull << (array[i] & 63u); }
    }
  }
  return arnm_roaring_clip_and_count(words, u->low, u->high);
}

/**
 * Index spans of a key's arrays inside [low, high], for merging them. The merges only run below
 * ROARING_UNION_BITS_FROM array values in the range, so a wide part's slice is copied into
 * @c lows as low parts -- a few dozen values -- and merged like any array.
 */
typedef struct array_spans {
  const uint16_t *array[ARNM_ROARING_QUERY_MAX]; /**< Each part's low parts, sorted. */
  uint32_t begin[ARNM_ROARING_QUERY_MAX];        /**< First index inside the range. */
  uint32_t end[ARNM_ROARING_QUERY_MAX];          /**< One past the last index inside it. */
  uint16_t lows[ROARING_UNION_BITS_FROM];        /**< Room for a wide view's values as low parts. */
} array_spans;

static void key_union_spans(const key_union *u, array_spans *spans) {
  uint32_t used = 0;
  for (uint32_t p = 0; p < u->count; ++p) {
    const arnm_roaring_container *c = u->parts[p];
    if (ROARING_KIND_WIDE == c->kind) {
      const uint32_t *wide = arnm_roaring_wide_of(c);
      const uint32_t begin =
          arnm_roaring_lower_bound32(wide, c->cardinality, arnm_roaring_value_of(u->key, u->low));
      const uint32_t end =
          arnm_roaring_upper_bound32(wide, c->cardinality, arnm_roaring_value_of(u->key, u->high));
      spans->array[p] = spans->lows + used;
      spans->begin[p] = 0;
      spans->end[p] = end - begin;
      for (uint32_t i = begin; i < end; ++i) { spans->lows[used++] = arnm_roaring_low_of(wide[i]); }
      continue;
    }
    spans->array[p] = arnm_roaring_array_of(c);
    spans->begin[p] =
        u->low ? arnm_roaring_lower_bound16(arnm_roaring_array_of(c), c->cardinality, u->low) : 0u;
    spans->end[p] =
        0xffffu == u->high
            ? c->cardinality
            : arnm_roaring_upper_bound16(arnm_roaring_array_of(c), c->cardinality, u->high);
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
  const uint64_t *bitmaps[ARNM_ROARING_QUERY_MAX];
  key_union arrays;
  uint32_t bitmap_count = 0;
  arrays.count = 0;
  arrays.key = u->key;
  arrays.low = u->low;
  arrays.high = u->high;
  arrays.any_bitmap = false;
  for (uint32_t p = 0; p < u->count; ++p) {
    if (ARNM_ROARING_BITMAP == u->parts[p]->kind) {
      bitmaps[bitmap_count++] = arnm_roaring_words_of(u->parts[p]);
    } else { // arrays and wide views alike: key_union_spans reads both
      arrays.parts[arrays.count++] = u->parts[p];
    }
  }

  uint32_t total;
  if (1u == bitmap_count) {
    total = arnm_roaring_words_cardinality(bitmaps[0], u->low, u->high);
  } else {
    const uint32_t first = u->low >> 6;
    const uint32_t last = u->high >> 6;
    total = 0;
    for (uint32_t w = first; w <= last; ++w) {
      uint64_t word = 0;
      for (uint32_t b = 0; b < bitmap_count; ++b) { word |= bitmaps[b][w]; }
      if (w == first) { word &= arnm_roaring_first_word_mask(u->low); }
      if (w == last) { word &= arnm_roaring_last_word_mask(u->high); }
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
      total += arnm_roaring_container_range_cardinality(c, u->low, u->high);
      continue;
    }
    const uint16_t *array = arnm_roaring_array_of(c);
    const uint32_t begin = u->low ? arnm_roaring_lower_bound16(array, c->cardinality, u->low) : 0u;
    const uint32_t end = 0xffffu == u->high
                             ? c->cardinality
                             : arnm_roaring_upper_bound16(array, c->cardinality, u->high);
    total += end - begin;
  }
  return total;
}

static uint32_t key_union_cardinality(const key_union *u) {
  if (1u == u->count) {
    return arnm_roaring_container_range_cardinality(u->parts[0], u->low, u->high);
  }
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
    if (w == first) { word &= arnm_roaring_first_word_mask(low); }
    if (w == last) { word &= arnm_roaring_last_word_mask(high); }
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
      out[written++] = arnm_roaring_value_of(key, (w << 6) | bit);
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
  uint32_t word_index;   /**< bitmap: the word @c word was loaded from */
  uint64_t word;         /**< bitmap: bits of word_index not yet read, range edges masked */
  uint32_t value;        /**< the low part it stands on */
  bool live;             /**< false once the range is read out; @c value is stale then */
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
    if (c->word_index == first) { c->word &= arnm_roaring_first_word_mask(low); }
    if (c->word_index == last) { c->word &= arnm_roaring_last_word_mask(high); }
  }
}

static void cursor_begin(
    cursor *c, const arnm_roaring_container *container, uint16_t low, uint16_t high, bool descending
) {
  c->live = true;
  c->wide = NULL;
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = arnm_roaring_wide_of(container);
    c->wide = wide;
    c->array = NULL;
    c->words = NULL;
    const uint32_t begin =
        low ? arnm_roaring_lower_bound32(
                  wide, container->cardinality, arnm_roaring_value_of(container->key, low)
              )
            : 0u;
    const uint32_t end = 0xffffu == high ? container->cardinality
                                         : arnm_roaring_upper_bound32(
                                               wide, container->cardinality,
                                               arnm_roaring_value_of(container->key, high)
                                           );
    c->index = descending ? end : begin;
    c->stop = descending ? begin : end;
    c->live = c->index != c->stop;
    if (c->live) { c->value = arnm_roaring_low_of(wide[descending ? c->index - 1u : c->index]); }
    return;
  }
  if (ARNM_ROARING_ARRAY == container->kind) {
    const uint16_t *array = arnm_roaring_array_of(container);
    c->array = array;
    c->words = NULL;
    const uint32_t begin =
        low ? arnm_roaring_lower_bound16(array, container->cardinality, low) : 0u;
    const uint32_t end = 0xffffu == high
                             ? container->cardinality
                             : arnm_roaring_upper_bound16(array, container->cardinality, high);
    c->index = descending ? end : begin;
    c->stop = descending ? begin : end;
    c->live = c->index != c->stop;
    if (c->live) { c->value = array[descending ? c->index - 1u : c->index]; }
    return;
  }
  c->array = NULL;
  c->words = arnm_roaring_words_of(container);
  c->word_index = descending ? high >> 6 : low >> 6;
  c->word = c->words[c->word_index];
  if (c->word_index == (uint32_t)(low >> 6)) { c->word &= arnm_roaring_first_word_mask(low); }
  if (c->word_index == (uint32_t)(high >> 6)) { c->word &= arnm_roaring_last_word_mask(high); }
  cursor_seek_word(c, low, high, descending);
}

static inline void cursor_next(cursor *c, uint16_t low, uint16_t high, bool descending) {
  if (c->wide) {
    c->index = descending ? c->index - 1u : c->index + 1u;
    c->live = c->index != c->stop;
    if (c->live) { c->value = arnm_roaring_low_of(c->wide[descending ? c->index - 1u : c->index]); }
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
  cursor cursors[ARNM_ROARING_QUERY_MAX];
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
      out[written++] = arnm_roaring_value_of(u->key, pick);
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
        out[written++] = arnm_roaring_value_of(u->key, c->value);
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
    const uint32_t *wide = arnm_roaring_wide_of(c);
    const uint32_t begin = u->low ? arnm_roaring_lower_bound32(
                                        wide, c->cardinality, arnm_roaring_value_of(u->key, u->low)
                                    )
                                  : 0u;
    const uint32_t end = 0xffffu == u->high
                             ? c->cardinality
                             : arnm_roaring_upper_bound32(
                                   wide, c->cardinality, arnm_roaring_value_of(u->key, u->high)
                               );
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
    const uint16_t *array = arnm_roaring_array_of(c);
    const uint32_t begin = u->low ? arnm_roaring_lower_bound16(array, c->cardinality, u->low) : 0u;
    const uint32_t end = 0xffffu == u->high
                             ? c->cardinality
                             : arnm_roaring_upper_bound16(array, c->cardinality, u->high);
    const uint32_t available = end - begin;
    if (*skip >= available) {
      *skip -= available;
      return 0;
    }
    const uint32_t left = available - *skip;
    const uint32_t take = left < size ? left : size;
    for (uint32_t i = 0; i < take; ++i) {
      out[i] = arnm_roaring_value_of(
          u->key, array[descending ? end - 1u - *skip - i : begin + *skip + i]
      );
    }
    *skip = 0;
    return take;
  }
  if (1u == u->count) {
    return words_emit(
        arnm_roaring_words_of(u->parts[0]), u->low, u->high, u->key, skip, size, descending, out
    );
  }
  if ((uint64_t)(*skip + (uint64_t)size) * u->count <= ROARING_UNION_MERGE_UP_TO) {
    return merged_cursors_emit(u, skip, size, descending, out);
  }
  uint64_t words[ARNM_ROARING_BITMAP_WORDS];
  (void)key_union_words(u, words);
  return words_emit(words, u->low, u->high, u->key, skip, size, descending, out);
}

// ********** several sets at once *******************

/** The parts of one key, sorted into the query's three lists, and the range that key covers. */
typedef struct query_key {
  const arnm_roaring_container *all[ARNM_ROARING_QUERY_MAX];  /**< A value must be in each. */
  const arnm_roaring_container *any[ARNM_ROARING_QUERY_MAX];  /**< A value must be in one. */
  const arnm_roaring_container *none[ARNM_ROARING_QUERY_MAX]; /**< A value must be in none. */
  uint32_t all_count;                                         /**< Containers in @c all. */
  uint32_t any_count;                                         /**< Containers in @c any. */
  uint32_t none_count;                                        /**< Containers in @c none. */
  uint16_t key;  /**< The upper 16 bits the values share. */
  uint16_t low;  /**< Lowest low part the range leaves of this key. */
  uint16_t high; /**< Highest low part the range leaves of this key. */
} query_key;

/** Where each set of the query stands while its keys are walked, in one direction. */
typedef struct query_walk {
  arnm_roaring_source all[ARNM_ROARING_QUERY_MAX];  /**< The sets a value must be in. */
  arnm_roaring_source any[ARNM_ROARING_QUERY_MAX];  /**< The sets a value may be in. */
  arnm_roaring_source none[ARNM_ROARING_QUERY_MAX]; /**< The sets a value must stay out of. */
  uint32_t all_count;                               /**< Sources in @c all. */
  uint32_t any_count;                               /**< Sources in @c any. */
  uint32_t none_count;                              /**< Sources in @c none. */
  uint32_t min;                                     /**< First value of the range. */
  uint32_t max;                                     /**< Last value of the range. */
  bool descending;                                  /**< Walking from the largest key down. */
  bool narrow;   /**< a page narrows each key to what its parts reach; a count keeps the bounds */
  bool have_key; /**< a key was returned before, so @c key says where the walk stands */
  uint16_t key;  /**< the key last returned, whose sources move on next */
} query_walk;

static void query_walk_begin(
    query_walk *walk, const arnm_roaring_query *query, bool descending, bool narrow
) {
  walk->all_count = query->all_count;
  walk->any_count = query->any_count;
  walk->none_count = query->none_count;
  walk->min = query->min;
  walk->max = query->max;
  walk->descending = descending;
  walk->narrow = narrow;
  walk->have_key = false;
  walk->key = 0;
  for (uint32_t i = 0; i < query->all_count; ++i) {
    arnm_roaring_source_begin(
        &walk->all[i], query->all[i], query->min, query->max, descending, NULL
    );
  }
  for (uint32_t i = 0; i < query->any_count; ++i) {
    arnm_roaring_source_begin(
        &walk->any[i], query->any[i], query->min, query->max, descending, NULL
    );
  }
  for (uint32_t i = 0; i < query->none_count; ++i) {
    arnm_roaring_source_begin(
        &walk->none[i], query->none[i], query->min, query->max, descending, NULL
    );
  }
}

/** Is @p key ahead of @p other in walking order? */
static inline bool key_before(uint16_t key, uint16_t other, bool descending) {
  return descending ? key > other : key < other;
}

/** Moves @p source forward until its key is at or past @p key. */
static void source_seek(arnm_roaring_source *source, uint16_t key, bool descending) {
  while (source->current && key_before(source->current->key, key, descending)) {
    arnm_roaring_source_advance(source);
  }
}

/**
 * Gathers the parts of the next key that can hold a match; false once the range is done. A key
 * is skipped where a set of @c all does not have it, or @c any has none of it, or the parts do
 * not reach into the range.
 */
static bool query_walk_next(query_walk *walk, query_key *out) {
  for (;;) {
    if (walk->have_key) {
      // the sources that gave to the key before move on only now, so its views lasted
      for (uint32_t i = 0; i < walk->all_count; ++i) {
        if (walk->all[i].current && walk->all[i].current->key == walk->key) {
          arnm_roaring_source_advance(&walk->all[i]);
        }
      }
      for (uint32_t i = 0; i < walk->any_count; ++i) {
        if (walk->any[i].current && walk->any[i].current->key == walk->key) {
          arnm_roaring_source_advance(&walk->any[i]);
        }
      }
      walk->have_key = false;
    }

    uint16_t key = 0;
    if (walk->all_count) {
      // the next key every set of all has: the one furthest along, everything else pulled up
      if (!walk->all[0].current) { return false; }
      key = walk->all[0].current->key;
      for (bool aligned = false; !aligned;) {
        aligned = true;
        for (uint32_t i = 0; i < walk->all_count; ++i) {
          source_seek(&walk->all[i], key, walk->descending);
          if (!walk->all[i].current) { return false; }
          if (walk->all[i].current->key != key) {
            key = walk->all[i].current->key;
            aligned = false;
          }
        }
      }
    } else {
      if (!walk->any_count) { return false; }
      bool found = false;
      for (uint32_t i = 0; i < walk->any_count; ++i) {
        const arnm_roaring_container *c = walk->any[i].current;
        if (!c) { continue; }
        if (!found || key_before(c->key, key, walk->descending)) {
          key = c->key;
          found = true;
        }
      }
      if (!found) { return false; }
    }

    out->all_count = 0;
    out->any_count = 0;
    out->none_count = 0;
    uint16_t bottom = 0;
    uint16_t top = 0xffffu;
    for (uint32_t i = 0; i < walk->all_count; ++i) {
      const arnm_roaring_container *c = walk->all[i].current;
      out->all[out->all_count++] = c;
      if (!walk->narrow) { continue; }
      uint16_t part_bottom, part_top;
      part_reach(&walk->all[i], c, &part_bottom, &part_top);
      // every match is in every part of all: the narrowest reach of them bounds the key
      if (part_bottom > bottom) { bottom = part_bottom; }
      if (part_top < top) { top = part_top; }
    }
    uint16_t any_bottom = 0xffffu;
    uint16_t any_top = 0;
    for (uint32_t i = 0; i < walk->any_count; ++i) {
      arnm_roaring_source *source = &walk->any[i];
      source_seek(source, key, walk->descending);
      if (!source->current || source->current->key != key) { continue; }
      out->any[out->any_count++] = source->current;
      if (!walk->narrow) { continue; }
      uint16_t part_bottom, part_top;
      part_reach(source, source->current, &part_bottom, &part_top);
      // a match is in at least one part of any: the widest reach of them bounds the key
      if (part_bottom < any_bottom) { any_bottom = part_bottom; }
      if (part_top > any_top) { any_top = part_top; }
    }
    for (uint32_t i = 0; i < walk->none_count; ++i) {
      arnm_roaring_source *source = &walk->none[i];
      source_seek(source, key, walk->descending);
      if (source->current && source->current->key == key) {
        out->none[out->none_count++] = source->current;
      }
    }

    walk->key = key;
    walk->have_key = true;
    if (walk->any_count && !out->any_count) { continue; } // any names a condition nothing meets
    out->key = key;
    arnm_roaring_container_bounds(key, walk->min, walk->max, &out->low, &out->high);
    if (walk->narrow) {
      if (walk->any_count) {
        if (any_bottom > bottom) { bottom = any_bottom; }
        if (any_top < top) { top = any_top; }
      }
      if (bottom > out->low) { out->low = bottom; }
      if (top < out->high) { out->high = top; }
    }
    if (out->low <= out->high) { return true; }
  }
}

/**
 * A part made ready to be asked about single values: which kind it is and where its values are,
 * read once per key instead of once per value. The lookups run per value of the smallest part,
 * so what a value costs here is what the whole key costs.
 */
typedef struct part_probe {
  const uint64_t *words; /**< a bitmap's words, NULL otherwise */
  const uint16_t *array; /**< an array's values, NULL otherwise */
  const uint32_t *wide;  /**< a wide view's values, NULL otherwise */
  uint32_t count;        /**< values in the array or the wide view */
  uint16_t key;          /**< the key, to build a wide view's 32 bit values back */
} part_probe;

static inline part_probe probe_of(const arnm_roaring_container *container) {
  part_probe probe = {NULL, NULL, NULL, container->cardinality, container->key};
  if (ARNM_ROARING_BITMAP == container->kind) {
    probe.words = arnm_roaring_words_of(container);
  } else if (ROARING_KIND_WIDE == container->kind) {
    probe.wide = arnm_roaring_wide_of(container);
  } else {
    probe.array = arnm_roaring_array_of(container);
  }
  return probe;
}

/** Whether @p probe holds @p low; a search, so the order of the questions does not matter. */
static inline bool probe_has(const part_probe *probe, uint16_t low) {
  if (probe->words) { return (probe->words[low >> 6] >> (low & 63u)) & 1u; }
  if (probe->wide) {
    const uint32_t value = arnm_roaring_value_of(probe->key, low);
    const uint32_t position = arnm_roaring_lower_bound32(probe->wide, probe->count, value);
    return position < probe->count && probe->wide[position] == value;
  }
  const uint32_t position = arnm_roaring_lower_bound16(probe->array, probe->count, low);
  return position < probe->count && probe->array[position] == low;
}

/** The parts a value has to be checked against once the driver has it. */
typedef struct key_probes {
  part_probe all[ARNM_ROARING_QUERY_MAX];  /**< The parts of @c all but the driver. */
  part_probe any[ARNM_ROARING_QUERY_MAX];  /**< The parts of @c any. */
  part_probe none[ARNM_ROARING_QUERY_MAX]; /**< The parts of @c none. */
  uint32_t all_count;                      /**< Probes in @c all. */
  uint32_t any_count;                      /**< Probes in @c any. */
  uint32_t none_count;                     /**< Probes in @c none. */
} key_probes;

static void key_probes_of(const query_key *key, uint32_t driver, key_probes *probes) {
  probes->all_count = 0;
  probes->any_count = 0;
  probes->none_count = 0;
  for (uint32_t i = 0; i < key->all_count; ++i) {
    if (i != driver) { probes->all[probes->all_count++] = probe_of(key->all[i]); }
  }
  for (uint32_t i = 0; i < key->any_count; ++i) {
    probes->any[probes->any_count++] = probe_of(key->any[i]);
  }
  for (uint32_t i = 0; i < key->none_count; ++i) {
    probes->none[probes->none_count++] = probe_of(key->none[i]);
  }
}

/** Whether @p low is a match once the driver has it: in the rest of all, in any, in no none. */
static inline bool probes_match(const key_probes *probes, uint16_t low) {
  for (uint32_t i = 0; i < probes->all_count; ++i) {
    if (!probe_has(&probes->all[i], low)) { return false; }
  }
  if (probes->any_count) {
    bool found = false;
    for (uint32_t i = 0; i < probes->any_count && !found; ++i) {
      found = probe_has(&probes->any[i], low);
    }
    if (!found) { return false; }
  }
  for (uint32_t i = 0; i < probes->none_count; ++i) {
    if (probe_has(&probes->none[i], low)) { return false; }
  }
  return true;
}

/** The part of @c all with the fewest values: the one a key is read from, the others asked. */
/**
 * The values of the part a key is read from, as the sorted slice it already is: the entries of an
 * array or of a sparse set's wide view that lie inside the range. A bitmap never reads its values
 * this way -- with one, a key is answered in bits.
 */
typedef struct driver_slice {
  const uint16_t *array; /**< an array's low parts, NULL for a wide view */
  const uint32_t *wide;  /**< a wide view's values, NULL for an array */
  uint32_t begin;        /**< first entry inside the range */
  uint32_t end;          /**< one past the last entry inside the range */
} driver_slice;

static inline uint16_t driver_at(const driver_slice *slice, uint32_t index) {
  return slice->array ? slice->array[index] : arnm_roaring_low_of(slice->wide[index]);
}

static void driver_slice_of(
    const arnm_roaring_container *container, uint16_t low, uint16_t high, driver_slice *slice
) {
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = arnm_roaring_wide_of(container);
    slice->array = NULL;
    slice->wide = wide;
    slice->begin =
        low ? arnm_roaring_lower_bound32(
                  wide, container->cardinality, arnm_roaring_value_of(container->key, low)
              )
            : 0u;
    slice->end = 0xffffu == high
                     ? container->cardinality
                     : arnm_roaring_upper_bound32(
                           wide, container->cardinality, arnm_roaring_value_of(container->key, high)
                       );
    return;
  }
  const uint16_t *array = arnm_roaring_array_of(container);
  slice->array = array;
  slice->wide = NULL;
  slice->begin = low ? arnm_roaring_lower_bound16(array, container->cardinality, low) : 0u;
  slice->end = 0xffffu == high ? container->cardinality
                               : arnm_roaring_upper_bound16(array, container->cardinality, high);
}

/**
 * Values of @p slice the probes match.
 *
 * The shape the index asks for most -- one set against one type's bitmap -- gets the loop it
 * deserves: a bit test per value, the same one @ref arnm_roaring_and() runs. Asking @ref
 * probes_match() instead costs a call for every value of the driver, several times the test.
 */
static uint64_t probes_count_slice(const key_probes *probes, const driver_slice *slice) {
  uint64_t count = 0;
  if (1u == probes->all_count && probes->all[0].words && !probes->any_count &&
      !probes->none_count) {
    const uint64_t *words = probes->all[0].words;
    for (uint32_t i = slice->begin; i < slice->end; ++i) {
      const uint16_t low = driver_at(slice, i);
      count += (words[low >> 6] >> (low & 63u)) & 1u;
    }
    return count;
  }
  for (uint32_t i = slice->begin; i < slice->end; ++i) {
    count += probes_match(probes, driver_at(slice, i));
  }
  return count;
}

/** One page out of @p slice: the matches after @p skip, from the near end or the far one. */
static uint32_t probes_page_slice(
    const key_probes *probes,
    const driver_slice *slice,
    uint16_t key,
    uint32_t *skip,
    uint32_t size,
    bool descending,
    uint32_t *out
) {
  const uint32_t width = slice->end - slice->begin;
  uint32_t written = 0;
  for (uint32_t n = 0; n < width && written < size; ++n) {
    const uint32_t index = descending ? slice->end - 1u - n : slice->begin + n;
    const uint16_t low = driver_at(slice, index);
    if (!probes_match(probes, low)) { continue; }
    if (*skip) {
      --*skip;
      continue;
    }
    out[written++] = arnm_roaring_value_of(key, low);
  }
  return written;
}

static uint32_t key_driver(const query_key *key) {
  uint32_t smallest = 0;
  for (uint32_t i = 1; i < key->all_count; ++i) {
    if (key->all[i]->cardinality < key->all[smallest]->cardinality) { smallest = i; }
  }
  return smallest;
}

/** Sets the bits of @p container inside [low, high] in @p words. */
static void bits_of_part(
    uint64_t *words, const arnm_roaring_container *container, uint16_t low, uint16_t high
) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  if (ARNM_ROARING_BITMAP == container->kind) {
    const uint64_t *x = arnm_roaring_words_of(container);
    for (uint32_t w = first; w <= last; ++w) { words[w] |= x[w]; }
    return;
  }
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = arnm_roaring_wide_of(container);
    const uint32_t begin = arnm_roaring_lower_bound32(
        wide, container->cardinality, arnm_roaring_value_of(container->key, low)
    );
    const uint32_t end = arnm_roaring_upper_bound32(
        wide, container->cardinality, arnm_roaring_value_of(container->key, high)
    );
    for (uint32_t i = begin; i < end; ++i) {
      const uint16_t value = arnm_roaring_low_of(wide[i]);
      words[value >> 6] |= 1ull << (value & 63u);
    }
    return;
  }
  const uint16_t *array = arnm_roaring_array_of(container);
  const uint32_t begin = arnm_roaring_lower_bound16(array, container->cardinality, low);
  const uint32_t end = arnm_roaring_upper_bound16(array, container->cardinality, high);
  for (uint32_t i = begin; i < end; ++i) { words[array[i] >> 6] |= 1ull << (array[i] & 63u); }
}

/** Clears the bits of @p container inside [low, high] in @p words. */
static void bits_without_part(
    uint64_t *words, const arnm_roaring_container *container, uint16_t low, uint16_t high
) {
  const uint32_t first = low >> 6;
  const uint32_t last = high >> 6;
  if (ARNM_ROARING_BITMAP == container->kind) {
    const uint64_t *x = arnm_roaring_words_of(container);
    for (uint32_t w = first; w <= last; ++w) { words[w] &= ~x[w]; }
    return;
  }
  if (ROARING_KIND_WIDE == container->kind) {
    const uint32_t *wide = arnm_roaring_wide_of(container);
    const uint32_t begin = arnm_roaring_lower_bound32(
        wide, container->cardinality, arnm_roaring_value_of(container->key, low)
    );
    const uint32_t end = arnm_roaring_upper_bound32(
        wide, container->cardinality, arnm_roaring_value_of(container->key, high)
    );
    for (uint32_t i = begin; i < end; ++i) {
      const uint16_t value = arnm_roaring_low_of(wide[i]);
      words[value >> 6] &= ~(1ull << (value & 63u));
    }
    return;
  }
  const uint16_t *array = arnm_roaring_array_of(container);
  const uint32_t begin = arnm_roaring_lower_bound16(array, container->cardinality, low);
  const uint32_t end = arnm_roaring_upper_bound16(array, container->cardinality, high);
  for (uint32_t i = begin; i < end; ++i) { words[array[i] >> 6] &= ~(1ull << (array[i] & 63u)); }
}

/**
 * The matching values of one key as bits, edges masked; returns their count. The way a key of
 * large parts is answered: every part is read once, word by word, instead of value by value.
 */
static uint32_t key_bits(const query_key *key, uint64_t *words, uint64_t *scratch) {
  const uint32_t first = key->low >> 6;
  const uint32_t last = key->high >> 6;
  const size_t span = (size_t)(last - first + 1u) * sizeof(uint64_t);
  memset(words + first, 0, span);
  if (key->all_count) {
    bits_of_part(words, key->all[0], key->low, key->high);
    for (uint32_t i = 1; i < key->all_count; ++i) {
      memset(scratch + first, 0, span);
      bits_of_part(scratch, key->all[i], key->low, key->high);
      for (uint32_t w = first; w <= last; ++w) { words[w] &= scratch[w]; }
    }
    if (key->any_count) {
      memset(scratch + first, 0, span);
      for (uint32_t i = 0; i < key->any_count; ++i) {
        bits_of_part(scratch, key->any[i], key->low, key->high);
      }
      for (uint32_t w = first; w <= last; ++w) { words[w] &= scratch[w]; }
    }
  } else {
    for (uint32_t i = 0; i < key->any_count; ++i) {
      bits_of_part(words, key->any[i], key->low, key->high);
    }
  }
  for (uint32_t i = 0; i < key->none_count; ++i) {
    bits_without_part(words, key->none[i], key->low, key->high);
  }
  return arnm_roaring_clip_and_count(words, key->low, key->high);
}

/**
 * Whether the key is read value by value from its smallest part of all, rather than combined in
 * bits. An array or a view holds at most 4096 values and usually far fewer, and looking each of
 * them up in the other parts beats three passes over the key's words; only where the smallest
 * part is itself a bitmap -- more values than an array may hold -- do the words win.
 */
static inline bool key_reads_values(const query_key *key, uint32_t driver) {
  return key->all_count && ARNM_ROARING_BITMAP != key->all[driver]->kind;
}

static uint64_t query_key_cardinality(const query_key *key, uint64_t *words, uint64_t *scratch) {
  if (!key->all_count && !key->none_count) {
    // a plain union of this key's parts, with every shortcut it has
    key_union parts;
    parts.count = key->any_count;
    parts.key = key->key;
    parts.low = key->low;
    parts.high = key->high;
    parts.any_bitmap = false;
    for (uint32_t i = 0; i < key->any_count; ++i) {
      parts.parts[i] = key->any[i];
      parts.any_bitmap |= ARNM_ROARING_BITMAP == key->any[i]->kind;
    }
    return key_union_cardinality(&parts);
  }
  const uint32_t driver = key_driver(key);
  if (1u == key->all_count && !key->any_count && !key->none_count) {
    return arnm_roaring_container_range_cardinality(key->all[0], key->low, key->high);
  }
  if (key_reads_values(key, driver)) {
    key_probes probes;
    key_probes_of(key, driver, &probes);
    driver_slice slice;
    driver_slice_of(key->all[driver], key->low, key->high, &slice);
    return probes_count_slice(&probes, &slice);
  }
  return key_bits(key, words, scratch);
}

static uint32_t query_key_page(
    const query_key *key,
    uint32_t *skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint64_t *words,
    uint64_t *scratch
) {
  if (!key->all_count && !key->none_count) {
    key_union parts;
    parts.count = key->any_count;
    parts.key = key->key;
    parts.low = key->low;
    parts.high = key->high;
    parts.any_bitmap = false;
    for (uint32_t i = 0; i < key->any_count; ++i) {
      parts.parts[i] = key->any[i];
      parts.any_bitmap |= ARNM_ROARING_BITMAP == key->any[i]->kind;
    }
    if (*skip) {
      // a key the skip passes over whole is counted, never read value by value
      const uint32_t in_key = key_union_cardinality(&parts);
      if (*skip >= in_key) {
        *skip -= in_key;
        return 0;
      }
    }
    return key_union_emit(&parts, skip, size, descending, out);
  }
  const uint32_t driver = key_driver(key);
  if (key_reads_values(key, driver)) {
    key_probes probes;
    key_probes_of(key, driver, &probes);
    driver_slice slice;
    driver_slice_of(key->all[driver], key->low, key->high, &slice);
    return probes_page_slice(&probes, &slice, key->key, skip, size, descending, out);
  }
  // the bits are built once and then skipped over or read, whichever the page needs
  const uint32_t in_key = key_bits(key, words, scratch);
  if (*skip >= in_key) {
    *skip -= in_key;
    return 0;
  }
  return words_emit(words, key->low, key->high, key->key, skip, size, descending, out);
}

/** The checks both answers start with. */
static arnm_result check_query(const arnm_roaring_query *query) {
  if (!query) { return ARNM_ERROR_NULL_POINTER; }
  if (query->all_count > ARNM_ROARING_QUERY_MAX || query->any_count > ARNM_ROARING_QUERY_MAX ||
      query->none_count > ARNM_ROARING_QUERY_MAX) {
    return ARNM_ERROR_INVALID_PARAM;
  }
  if ((query->all_count && !query->all) || (query->any_count && !query->any) ||
      (query->none_count && !query->none)) {
    return ARNM_ERROR_NULL_POINTER;
  }
  return ARNM_SUCCESS;
}

/** Only sets of the sparse form, and nothing to intersect or subtract: the arrays answer alone. */
static bool query_is_sparse_union(const arnm_roaring_query *query) {
  return !query->all_count && !query->none_count && query->any_count &&
         all_sparse(query->any, query->any_count);
}

arnm_result arnm_roaring_query_cardinality(const arnm_roaring_query *query, uint64_t *out) {
  const arnm_result result = check_query(query);
  if (ARNM_SUCCESS != result) { return result; }
  if (!out) { return ARNM_ERROR_NULL_POINTER; }
  uint64_t total = 0;
  if (query->min <= query->max) {
    if (query_is_sparse_union(query)) {
      sparse_spans spans;
      sparse_spans_of(query->any, query->any_count, query->min, query->max, &spans);
      total = spans.count ? sparse_union_cardinality(&spans) : 0u;
    } else {
      uint64_t words[ARNM_ROARING_BITMAP_WORDS];
      uint64_t scratch[ARNM_ROARING_BITMAP_WORDS];
      query_walk walk;
      query_walk_begin(&walk, query, false, false);
      query_key key;
      while (query_walk_next(&walk, &key)) { total += query_key_cardinality(&key, words, scratch); }
    }
  }
  *out = total;
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_query_listing(
    const arnm_roaring_query *query,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written,
    uint64_t *cardinality
) {
  const arnm_result result = check_query(query);
  if (ARNM_SUCCESS != result) { return result; }
  if (!written || !cardinality || (size && !out)) { return ARNM_ERROR_NULL_POINTER; }
  uint64_t total = 0;
  uint32_t taken = 0;
  if (query->min <= query->max) {
    if (query_is_sparse_union(query)) {
      sparse_spans spans;
      sparse_spans_of(query->any, query->any_count, query->min, query->max, &spans);
      if (spans.count) {
        // both readers walk the spans down, so the page gets its own copy of them
        sparse_spans page_spans = spans;
        total = sparse_union_cardinality(&spans);
        if (size) { taken = sparse_union_page(&page_spans, skip, size, descending, out); }
      }
    } else {
      uint64_t words[ARNM_ROARING_BITMAP_WORDS];
      uint64_t scratch[ARNM_ROARING_BITMAP_WORDS];
      query_walk walk;
      query_walk_begin(&walk, query, descending, true);
      query_key key;
      while (query_walk_next(&walk, &key)) {
        // counted once; only the key the page starts in is read a second time, value by value
        const uint64_t in_key = query_key_cardinality(&key, words, scratch);
        total += in_key;
        if (taken < size) {
          if (skip >= in_key) {
            skip -= (uint32_t)in_key;
          } else {
            taken +=
                query_key_page(&key, &skip, size - taken, descending, out + taken, words, scratch);
          }
        }
      }
    }
  }
  *written = taken;
  *cardinality = total;
  return ARNM_SUCCESS;
}

arnm_result arnm_roaring_query_page(
    const arnm_roaring_query *query,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written
) {
  const arnm_result result = check_query(query);
  if (ARNM_SUCCESS != result) { return result; }
  if (!written || (size && !out)) { return ARNM_ERROR_NULL_POINTER; }
  uint32_t total = 0;
  if (size && query->min <= query->max) {
    if (query_is_sparse_union(query)) {
      sparse_spans spans;
      sparse_spans_of(query->any, query->any_count, query->min, query->max, &spans);
      total = spans.count ? sparse_union_page(&spans, skip, size, descending, out) : 0u;
    } else {
      uint64_t words[ARNM_ROARING_BITMAP_WORDS];
      uint64_t scratch[ARNM_ROARING_BITMAP_WORDS];
      query_walk walk;
      query_walk_begin(&walk, query, descending, true);
      query_key key;
      while (total < size && query_walk_next(&walk, &key)) {
        total += query_key_page(&key, &skip, size - total, descending, out + total, words, scratch);
      }
    }
  }
  *written = total;
  return ARNM_SUCCESS;
}
