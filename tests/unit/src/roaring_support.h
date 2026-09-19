#ifndef ARNM_TESTS_ROARING_SUPPORT_H
#define ARNM_TESTS_ROARING_SUPPORT_H

/*
 * What the three roaring test binaries share: a pool and a set that clean up when a test leaves,
 * the reference as a sorted vector, the structure check every set has to pass, and the random
 * sets the comparisons are built from.
 */

#include "arnm/bitmap.h"
#include "arnm/graded_block_pool.h"
#include "arnm/result.h"
#include "arnm/roaring_bitmap.h"
#include "arnm/roaring_query.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace {

using Values = std::vector<uint32_t>;

struct Pool {
  explicit Pool(uint8_t max_log2 = 0, uint8_t min_log2 = 0) {
    arnm_graded_block_pool_options options{};
    options.min_block_log2 = min_log2;
    options.max_block_log2 = max_log2;
    EXPECT_EQ(arnm_graded_block_pool_init(&pool, &options, nullptr), ARNM_SUCCESS);
  }
  ~Pool() {
    arnm_graded_block_pool_release(&pool, nullptr);
  }
  arnm_graded_block_pool pool{};
};

/** A set that gives its blocks back when the test leaves. */
struct Set {
  explicit Set(arnm_graded_block_pool *pool_) : pool(pool_) {
    arnm_roaring_init(&set);
  }
  ~Set() {
    arnm_roaring_free(&set, pool);
  }
  Set(const Set &) = delete;
  Set &operator=(const Set &) = delete;
  arnm_roaring_bitmap set;
  arnm_graded_block_pool *pool;
};

uint64_t BlockBytes(const arnm_roaring_bitmap &set) {
  if (!set.containers) { return 0; }
  uint64_t bytes = uint64_t{1} << set.directory_log2;
  if (!set.count) { return bytes; } // sparse: the values' block and nothing else
  for (uint32_t i = 0; i < set.count; ++i) { bytes += uint64_t{1} << set.containers[i].block_log2; }
  return bytes;
}

Values ToValues(const arnm_roaring_bitmap &set) {
  Values values(set.cardinality);
  if (!values.empty()) {
    EXPECT_EQ(arnm_roaring_page(&set, 0, set.cardinality, false, values.data()), set.cardinality);
  }
  return values;
}

/** Structure and content: what every set has to satisfy after every call. */
void ExpectValid(const arnm_roaring_bitmap &set, const Values &reference) {
  ASSERT_TRUE(std::is_sorted(reference.begin(), reference.end()));
  ASSERT_EQ(set.cardinality, reference.size());
  // a sparse set never passes the limit; a small one may still have containers when it is the
  // result of an operation on containers
  if (!set.count) { ASSERT_LE(set.cardinality, ARNM_ROARING_SPARSE_MAX); }
  if (!set.count) {
    if (!set.cardinality) {
      ASSERT_EQ(set.values, nullptr);
      return;
    }
    ASSERT_NE(set.values, nullptr);
    ASSERT_LE(set.cardinality * sizeof(uint32_t), size_t{1} << set.directory_log2);
    for (uint32_t k = 1; k < set.cardinality; ++k) { ASSERT_LT(set.values[k - 1], set.values[k]); }
  }
  uint64_t total = set.count ? 0u : set.cardinality;
  for (uint32_t i = 0; i < set.count; ++i) {
    const arnm_roaring_container &c = set.containers[i];
    if (i) { ASSERT_LT(set.containers[i - 1].key, c.key); }
    ASSERT_GE(c.cardinality, 1u);
    // the kind follows the size, for built sets and results alike
    ASSERT_EQ(
        c.kind, c.cardinality > ARNM_ROARING_ARRAY_MAX ? ARNM_ROARING_BITMAP : ARNM_ROARING_ARRAY
    ) << "container "
      << i << " of " << c.cardinality;
    if (ARNM_ROARING_ARRAY == c.kind) {
      ASSERT_LE(c.cardinality * sizeof(uint16_t), size_t{1} << c.block_log2);
      const auto *array = reinterpret_cast<const uint16_t *>(c.data);
      for (uint32_t k = 1; k < c.cardinality; ++k) { ASSERT_LT(array[k - 1], array[k]); }
    } else {
      ASSERT_EQ(c.block_log2, 13u);
      const auto *words = reinterpret_cast<const uint64_t *>(c.data);
      uint32_t bits = 0;
      for (uint32_t w = 0; w < ARNM_ROARING_BITMAP_WORDS; ++w) {
        bits += static_cast<uint32_t>(arnm_popcountll(words[w]));
      }
      ASSERT_EQ(bits, c.cardinality);
    }
    total += c.cardinality;
  }
  ASSERT_EQ(total, set.cardinality);
  if (!reference.empty()) {
    ASSERT_EQ(set.maximum, reference.back());
    const Values values = ToValues(set);
    ASSERT_TRUE(std::equal(values.begin(), values.end(), reference.begin(), reference.end()));
  } else {
    ASSERT_EQ(set.count, 0u);
  }
}

void Build(arnm_roaring_bitmap *set, const Values &values, arnm_graded_block_pool *pool) {
  for (uint32_t value : values) { ASSERT_EQ(arnm_roaring_add(set, value, pool), ARNM_SUCCESS); }
  // built by adding, the form follows the size exactly
  ASSERT_EQ(set->count == 0u, set->cardinality <= ARNM_ROARING_SPARSE_MAX) << set->cardinality;
}

Values InRange(const Values &values, uint32_t min, uint32_t max) {
  if (min > max) { return {}; }
  return {
      std::lower_bound(values.begin(), values.end(), min),
      std::upper_bound(values.begin(), values.end(), max)
  };
}

size_t CountInRange(const Values &values, uint32_t min, uint32_t max) {
  if (min > max) { return 0; }
  return static_cast<size_t>(
      std::upper_bound(values.begin(), values.end(), max) -
      std::lower_bound(values.begin(), values.end(), min)
  );
}

bool Holds(const Values &values, uint32_t value) {
  return std::binary_search(values.begin(), values.end(), value);
}

/**
 * Random values spread over @p keys consecutive keys from @p first_key, each key drawn sparse,
 * dense or full, so every pairing of container kinds turns up.
 */
Values RandomValues(std::mt19937 &random, uint32_t first_key, uint32_t keys) {
  Values values;
  auto insert = [&values](uint32_t value) { values.push_back(value); };
  for (uint32_t k = 0; k < keys; ++k) {
    const uint32_t base = (first_key + k) << 16;
    switch (random() % 5) {
    case 0:
      break; // this key stays empty
    case 1:  // sparse: an array
      for (int i = 0; i < 50; ++i) { insert(base | (random() & 0xffffu)); }
      break;
    case 2: // just around the switch from array to bitmap
      for (uint32_t i = 0; i < 4096u + (random() % 3); ++i) { insert(base | (i * 13u)); }
      break;
    case 3: // dense: a bitmap
      for (int i = 0; i < 20000; ++i) { insert(base | (random() & 0xffffu)); }
      break;
    default: // every value of the key
      for (uint32_t i = 0; i < 65536u; ++i) { insert(base | i); }
      break;
    }
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

/**
 * Few values spread over @p keys keys -- a sparse set, or one just past the sparse limit, whose
 * containers then hold a handful of values each.
 */
Values RandomSparseValues(std::mt19937 &random, uint32_t first_key, uint32_t keys) {
  Values values;
  const uint32_t count = 1u + static_cast<uint32_t>(random() % (ARNM_ROARING_SPARSE_MAX + 80u));
  for (uint32_t i = 0; i < count; ++i) {
    // the edges of a key now and then: the first and the last low part it can hold
    const uint32_t pick = random() % 8u;
    const uint32_t low = 0 == pick ? 0u : 1 == pick ? 0xffffu : (random() & 0xffffu);
    values.push_back(((first_key + static_cast<uint32_t>(random() % keys)) << 16) | low);
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

/**
 * A set that shares values with @p base: a random part of it, cut down to at most @p keep_max
 * values, together with @p extra. Two independent random sets hardly ever hold the same value, and
 * an intersection of nothing tests little.
 */
Values Correlated(std::mt19937 &random, const Values &base, const Values &extra, size_t keep_max) {
  Values values;
  for (uint32_t value : base) {
    if (random() % 2 == 0) { values.push_back(value); }
  }
  while (values.size() > keep_max) {
    values.erase(values.begin() + static_cast<long>(random() % values.size()));
  }
  values.insert(values.end(), extra.begin(), extra.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

/** Dense or sparse, one in three sparse. */
Values RandomAnyValues(std::mt19937 &random, uint32_t first_key, uint32_t keys) {
  return random() % 3 == 0 ? RandomSparseValues(random, first_key, keys)
                           : RandomValues(random, first_key, keys);
}

/** A query that only unites: the sets in @c any, nothing to intersect or subtract. */
inline arnm_roaring_query AnyQuery(
    const arnm_roaring_bitmap *const *sets, uint32_t count, uint32_t min, uint32_t max
) {
  arnm_roaring_query query{};
  query.any = sets;
  query.any_count = count;
  query.min = min;
  query.max = max;
  return query;
}

/** The count of such a union. */
inline arnm_result UnionCardinality(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    uint64_t *out
) {
  const arnm_roaring_query query = AnyQuery(sets, count, min, max);
  return arnm_roaring_query_cardinality(&query, out);
}

/** A page of such a union. */
inline arnm_result UnionPage(
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
  const arnm_roaring_query query = AnyQuery(sets, count, min, max);
  return arnm_roaring_query_page(&query, skip, size, descending, out, written);
}

} // namespace

#endif // ARNM_TESTS_ROARING_SUPPORT_H
