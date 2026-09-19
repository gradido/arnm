#include "arnm/bitmap.h"
#include "arnm/graded_block_pool.h"
#include "arnm/result.h"
#include "arnm/roaring_bitmap.h"

#include "memory_limit.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

/*
 * The roaring bitmap against a sorted vector of the same values. Every operation is compared with
 * what the reference answers, and after every change the set's own structure is checked: keys
 * ascending, each container's kind fitting its size, the stored cardinality and maximum true,
 * and the pool lending exactly the blocks the sets hold.
 */

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

} // namespace

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

TEST(RoaringBitmap, EmptyIsZeroed) {
  arnm_roaring_bitmap set;
  std::memset(&set, 0xab, sizeof(set));
  arnm_roaring_init(&set);
  arnm_roaring_bitmap zero{};
  EXPECT_EQ(std::memcmp(&set, &zero, sizeof(set)), 0);
  EXPECT_EQ(sizeof(arnm_roaring_bitmap), 24u);
  EXPECT_EQ(sizeof(arnm_roaring_container), 16u);

  uint32_t out = 7;
  EXPECT_EQ(arnm_roaring_cardinality(&set), 0u);
  EXPECT_FALSE(arnm_roaring_minimum(&set, &out));
  EXPECT_FALSE(arnm_roaring_maximum(&set, &out));
  EXPECT_FALSE(arnm_roaring_select(&set, 0, &out));
  EXPECT_FALSE(arnm_roaring_contains(&set, 0));
  EXPECT_EQ(arnm_roaring_range_cardinality(&set, 0, UINT32_MAX), 0u);
  EXPECT_EQ(arnm_roaring_page(&set, 0, 1, false, &out), 0u);
  EXPECT_EQ(out, 7u);

  // NULL reads as empty, NULL frees and inits as nothing
  EXPECT_EQ(arnm_roaring_cardinality(nullptr), 0u);
  EXPECT_FALSE(arnm_roaring_minimum(nullptr, &out));
  EXPECT_FALSE(arnm_roaring_contains(nullptr, 1));
  EXPECT_EQ(arnm_roaring_page(nullptr, 0, 1, false, &out), 0u);
  arnm_roaring_init(nullptr);
  arnm_roaring_free(nullptr, nullptr);
  arnm_roaring_free(&set, nullptr);
}

TEST(RoaringBitmap, AddIsAscendingOnly) {
  Pool pool;
  Set s(&pool.pool);
  EXPECT_EQ(arnm_roaring_add(&s.set, 10, &pool.pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_roaring_add(&s.set, 10, &pool.pool), ARNM_SUCCESS); // the largest again
  EXPECT_EQ(arnm_roaring_add(&s.set, 9, &pool.pool), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_roaring_add(&s.set, 70000, &pool.pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_roaring_add(&s.set, 69999, &pool.pool), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_roaring_add(nullptr, 1, &pool.pool), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_roaring_add(&s.set, 80000, nullptr), ARNM_ERROR_NULL_POINTER);
  ExpectValid(s.set, {10, 70000});
  EXPECT_EQ(s.set.count, 0u) << "two values are a sparse set";
}

TEST(RoaringBitmap, ASparseSetDoublesAndTurnsIntoContainersPastItsLimit) {
  Pool pool;
  Set s(&pool.pool);
  Values reference;
  // spread over many keys, the case the sparse form is for
  for (uint32_t v = 0; v < ARNM_ROARING_SPARSE_MAX; ++v) {
    const uint32_t value = v * 40000u;
    ASSERT_EQ(arnm_roaring_add(&s.set, value, &pool.pool), ARNM_SUCCESS);
    reference.push_back(value);
    ASSERT_EQ(s.set.count, 0u);
    // the block is always the smallest power of two that holds the values
    size_t block = 16u;
    while (block < (v + 1u) * sizeof(uint32_t)) { block *= 2u; }
    ASSERT_EQ(size_t{1} << s.set.directory_log2, block) << v;
    ASSERT_EQ(pool.pool.lent_bytes, block) << "an outgrown block goes back";
  }
  ASSERT_EQ(arnm_roaring_add(&s.set, ARNM_ROARING_SPARSE_MAX * 40000u, &pool.pool), ARNM_SUCCESS);
  reference.push_back(ARNM_ROARING_SPARSE_MAX * 40000u);
  EXPECT_GT(s.set.count, 1u);
  ExpectValid(s.set, reference);
  EXPECT_EQ(pool.pool.lent_bytes, BlockBytes(s.set));
}

TEST(RoaringBitmap, AnArrayContainerDoublesAndTurnsIntoABitmapPast4096) {
  Pool pool;
  Set s(&pool.pool);
  Values reference;
  for (uint32_t v = 0; v < 4096u; ++v) {
    ASSERT_EQ(arnm_roaring_add(&s.set, v * 2u, &pool.pool), ARNM_SUCCESS);
    reference.push_back(v * 2u);
    if (v < ARNM_ROARING_SPARSE_MAX) { continue; }
    ASSERT_EQ(s.set.count, 1u);
    const auto &c = s.set.containers[0];
    // from the switch on, the array's block is the smallest power of two that holds it
    size_t block = 16u;
    while (block < (v + 1u) * 2u) { block *= 2u; }
    ASSERT_EQ(size_t{1} << c.block_log2, block) << v;
  }
  EXPECT_EQ(s.set.containers[0].kind, ARNM_ROARING_ARRAY);
  ASSERT_EQ(arnm_roaring_add(&s.set, 8192u, &pool.pool), ARNM_SUCCESS);
  reference.push_back(8192u);
  EXPECT_EQ(s.set.containers[0].kind, ARNM_ROARING_BITMAP);
  ExpectValid(s.set, reference);
  // the outgrown blocks went back to the pool, only what the set holds is lent out
  EXPECT_EQ(pool.pool.lent_bytes, BlockBytes(s.set));
}

TEST(RoaringBitmap, TheWholeValueRangeFits) {
  Pool pool;
  Set s(&pool.pool);
  const Values reference = {0u, 1u, 65535u, 65536u, 0xfffeffffu, 0xffff0000u, UINT32_MAX};
  Build(&s.set, reference, &pool.pool);
  ExpectValid(s.set, reference);
  EXPECT_TRUE(arnm_roaring_contains(&s.set, UINT32_MAX));
  EXPECT_EQ(arnm_roaring_range_cardinality(&s.set, 0xffff0000u, UINT32_MAX), 2u);
  uint32_t value = 0;
  EXPECT_TRUE(arnm_roaring_minimum(&s.set, &value));
  EXPECT_EQ(value, 0u);
}

TEST(RoaringBitmap, EveryKeyTheDirectoryCanHold) {
  Pool pool;
  Set s(&pool.pool);
  Values reference;
  for (uint32_t key = 0; key < 65536u; ++key) { reference.push_back((key << 16) | (key & 0xffu)); }
  Build(&s.set, reference, &pool.pool);
  EXPECT_EQ(s.set.count, 65536u);
  EXPECT_EQ(s.set.directory_log2, 20u);
  ExpectValid(s.set, reference);
}

// ---------------------------------------------------------------------------
// reading, against the reference
// ---------------------------------------------------------------------------

TEST(RoaringBitmap, ReadsMatchTheReference) {
  Pool pool;
  std::mt19937 random(1);
  for (int round = 0; round < 6; ++round) {
    const Values reference = RandomAnyValues(random, random() % 60000u, 1u + random() % 6u);
    Set s(&pool.pool);
    Build(&s.set, reference, &pool.pool);
    ExpectValid(s.set, reference);
    if (reference.empty()) { continue; }
    const Values &values = reference;

    uint32_t out = 0;
    ASSERT_TRUE(arnm_roaring_minimum(&s.set, &out));
    EXPECT_EQ(out, values.front());
    ASSERT_TRUE(arnm_roaring_maximum(&s.set, &out));
    EXPECT_EQ(out, values.back());

    for (int probe = 0; probe < 2000; ++probe) {
      // near a value of the set half the time, anywhere in its span otherwise
      const uint32_t near = values[random() % values.size()];
      const uint32_t value =
          (probe & 1) ? near + (random() % 3u) - 1u
                      : values.front() + random() % (values.back() - values.front() + 1u);
      ASSERT_EQ(arnm_roaring_contains(&s.set, value), Holds(reference, value)) << value;

      const uint32_t rank = static_cast<uint32_t>(random() % values.size());
      ASSERT_TRUE(arnm_roaring_select(&s.set, rank, &out));
      ASSERT_EQ(out, values[rank]) << "rank " << rank;

      uint32_t min = values[random() % values.size()] - (random() % 100u);
      uint32_t max = values[random() % values.size()] + (random() % 100u);
      ASSERT_EQ(arnm_roaring_range_cardinality(&s.set, min, max), CountInRange(reference, min, max))
          << min << ".." << max;
    }
    EXPECT_FALSE(arnm_roaring_select(&s.set, static_cast<uint32_t>(values.size()), &out));
    EXPECT_EQ(arnm_roaring_range_cardinality(&s.set, 10, 9), 0u);
    EXPECT_EQ(arnm_roaring_range_cardinality(&s.set, 0, UINT32_MAX), values.size());
  }
}

TEST(RoaringBitmap, PagesMatchTheReferenceBothWays) {
  Pool pool;
  std::mt19937 random(2);
  for (int round = 0; round < 6; ++round) {
    const Values reference = RandomAnyValues(random, random() % 1000u, 1u + random() % 5u);
    Set s(&pool.pool);
    Build(&s.set, reference, &pool.pool);
    const Values &ascending = reference;
    const Values descending(reference.rbegin(), reference.rend());
    for (int probe = 0; probe < 300 && !ascending.empty(); ++probe) {
      const uint32_t skip = static_cast<uint32_t>(random() % (ascending.size() + 2u));
      const uint32_t size = 1u + static_cast<uint32_t>(random() % 5000u);
      for (bool down : {false, true}) {
        const Values &expected = down ? descending : ascending;
        Values page(size, 0xdeadbeefu);
        const uint32_t written = arnm_roaring_page(&s.set, skip, size, down, page.data());
        const size_t available = skip < expected.size() ? expected.size() - skip : 0u;
        ASSERT_EQ(written, std::min<size_t>(size, available))
            << "skip " << skip << " down " << down;
        ASSERT_TRUE(
            std::equal(
                page.begin(), page.begin() + written,
                expected.begin() + std::min<size_t>(skip, expected.size())
            )
        ) << "skip "
          << skip << " size " << size << " down " << down;
        ASSERT_EQ(page[written < size ? written : size - 1u] == 0xdeadbeefu, written < size);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// set operations, against the reference
// ---------------------------------------------------------------------------

namespace {

enum class Op { And, Or, AndNot, CopyRange };

Values Expected(Op op, const Values &a, const Values &b, uint32_t min, uint32_t max) {
  const Values ra = InRange(a, min, max);
  const Values rb = InRange(b, min, max);
  Values result;
  auto into = std::back_inserter(result);
  switch (op) {
  case Op::And:
    std::set_intersection(ra.begin(), ra.end(), rb.begin(), rb.end(), into);
    break;
  case Op::Or:
    std::set_union(ra.begin(), ra.end(), rb.begin(), rb.end(), into);
    break;
  case Op::AndNot:
    std::set_difference(ra.begin(), ra.end(), rb.begin(), rb.end(), into);
    break;
  case Op::CopyRange:
    result = ra;
    break;
  }
  return result;
}

arnm_result RunOp(
    Op op,
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
) {
  switch (op) {
  case Op::And:
    return arnm_roaring_and(out, a, b, min, max, pool);
  case Op::Or:
    return arnm_roaring_or(out, a, b, min, max, pool);
  case Op::AndNot:
    return arnm_roaring_andnot(out, a, b, min, max, pool);
  case Op::CopyRange:
    return arnm_roaring_copy_range(out, a, min, max, pool);
  }
  return ARNM_ERROR_INVALID_PARAM;
}

} // namespace

TEST(RoaringBitmap, SetOperationsMatchTheReference) {
  Pool pool;
  std::mt19937 random(3);
  for (int round = 0; round < 30; ++round) {
    // overlapping key spans, so matching keys meet in every pairing of kinds
    const uint32_t first_key = random() % 100u;
    const Values ra = RandomAnyValues(random, first_key, 1u + random() % 6u);
    Values rb = RandomAnyValues(random, first_key + random() % 3u, 1u + random() % 6u);
    if (round % 3 != 0) {
      // shares values with ra: sparse (a few hundred of them) or dense (all it takes)
      const bool sparse = round % 3 == 1;
      rb = Correlated(random, ra, sparse ? Values{} : rb, sparse ? 600u : SIZE_MAX);
    }
    Set a(&pool.pool), b(&pool.pool);
    Build(&a.set, ra, &pool.pool);
    Build(&b.set, rb, &pool.pool);
    const uint64_t lent_inputs = pool.pool.lent_bytes;

    const uint32_t span_low = first_key << 16;
    const uint32_t span_high = (first_key + 9u) << 16;
    for (int probe = 0; probe < 25; ++probe) {
      uint32_t min = span_low + random() % (span_high - span_low);
      uint32_t max = span_low + random() % (span_high - span_low);
      if (probe == 0) {
        min = 0;
        max = UINT32_MAX;
      } else if (probe % 7 == 0) {
        std::swap(min, max); // sometimes empty
      } else if (probe % 3 == 0) {
        // ends exactly on values of the inputs, where an off by one at the edge shows
        const Values &from = probe % 2 ? ra : rb;
        if (!from.empty()) {
          // on a value, or one beside it -- where a search that stops a step early shows
          min = from[random() % from.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
          max = from[random() % from.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
          if (min > max) { std::swap(min, max); }
        }
      }
      for (Op op : {Op::And, Op::Or, Op::AndNot, Op::CopyRange}) {
        Set out(&pool.pool);
        ASSERT_EQ(RunOp(op, &out.set, &a.set, &b.set, min, max, &pool.pool), ARNM_SUCCESS);
        ExpectValid(out.set, Expected(op, ra, rb, min, max));
        ASSERT_EQ(pool.pool.lent_bytes, lent_inputs + BlockBytes(out.set));
      }
      // the inputs are read, never changed
      ExpectValid(a.set, ra);
      ExpectValid(b.set, rb);
    }
  }
}

TEST(RoaringBitmap, TwoArraysCanUniteIntoABitmap) {
  // 3000 even and 3000 odd values: both arrays, their union of 6000 is not
  Pool pool;
  Set a(&pool.pool), b(&pool.pool);
  Values ra, rb;
  for (uint32_t i = 0; i < 3000u; ++i) {
    ra.push_back(i * 2u);
    rb.push_back(i * 2u + 1u);
  }
  Build(&a.set, ra, &pool.pool);
  Build(&b.set, rb, &pool.pool);
  ASSERT_EQ(a.set.containers[0].kind, ARNM_ROARING_ARRAY);
  ASSERT_EQ(b.set.containers[0].kind, ARNM_ROARING_ARRAY);
  for (uint32_t max : {UINT32_MAX, 8000u, 4000u}) {
    Set out(&pool.pool);
    ASSERT_EQ(arnm_roaring_or(&out.set, &a.set, &b.set, 0, max, &pool.pool), ARNM_SUCCESS);
    ExpectValid(out.set, Expected(Op::Or, ra, rb, 0, max));
  }
  // overlapping: more than 4096 values in, fewer out, still an array
  Set c(&pool.pool);
  Build(&c.set, InRange(ra, 0, 2000u), &pool.pool);
  Set out(&pool.pool);
  ASSERT_EQ(arnm_roaring_or(&out.set, &a.set, &c.set, 0, UINT32_MAX, &pool.pool), ARNM_SUCCESS);
  ExpectValid(out.set, ra);
}

TEST(RoaringBitmap, SparseAndDenseMeetAtTheEdgesOfAKey) {
  // dense: every value of key 3 and some of key 4; sparse: the first and last low part of the
  // keys around them, so a key's view of the sparse set has to end exactly at 0xffff
  Pool pool;
  Set dense(&pool.pool), sparse(&pool.pool);
  Values rd, rs;
  for (uint32_t low = 0; low < 65536u; ++low) { rd.push_back((3u << 16) | low); }
  for (uint32_t low = 0; low < 65536u; low += 7u) { rd.push_back((4u << 16) | low); }
  for (uint32_t key = 2; key <= 5u; ++key) {
    rs.push_back(key << 16);
    rs.push_back((key << 16) | 0xffffu);
  }
  Build(&dense.set, rd, &pool.pool);
  Build(&sparse.set, rs, &pool.pool);
  ASSERT_GT(dense.set.count, 0u);
  ASSERT_EQ(sparse.set.count, 0u);

  const std::pair<uint32_t, uint32_t> ranges[] = {
      {0u, UINT32_MAX}, {3u << 16, (4u << 16) | 0xffffu}, {(3u << 16) | 0xffffu, 4u << 16}
  };
  for (const auto &range : ranges) {
    for (Op op : {Op::And, Op::Or, Op::AndNot}) {
      for (bool swap : {false, true}) {
        Set out(&pool.pool);
        const Set &a = swap ? sparse : dense;
        const Set &b = swap ? dense : sparse;
        ASSERT_EQ(
            RunOp(op, &out.set, &a.set, &b.set, range.first, range.second, &pool.pool), ARNM_SUCCESS
        );
        ExpectValid(
            out.set, Expected(op, swap ? rs : rd, swap ? rd : rs, range.first, range.second)
        );
      }
    }
    const arnm_roaring_bitmap *sets[2] = {&dense.set, &sparse.set};
    Values all;
    std::set_union(rd.begin(), rd.end(), rs.begin(), rs.end(), std::back_inserter(all));
    const Values in_range = InRange(all, range.first, range.second);
    uint64_t count = 0;
    ASSERT_EQ(
        arnm_roaring_union_cardinality(sets, 2, range.first, range.second, &count), ARNM_SUCCESS
    );
    EXPECT_EQ(count, in_range.size());
    for (bool descending : {false, true}) {
      Values page(40);
      uint32_t written = 0;
      ASSERT_EQ(
          arnm_roaring_union_page(
              sets, 2, range.first, range.second, 3, 40, descending, page.data(), &written
          ),
          ARNM_SUCCESS
      );
      page.resize(written);
      Values expected;
      for (size_t i = 3; i < in_range.size() && expected.size() < 40u; ++i) {
        expected.push_back(descending ? in_range[in_range.size() - 1u - i] : in_range[i]);
      }
      EXPECT_EQ(page, expected) << "descending " << descending;
    }
  }
}

TEST(RoaringBitmap, RangesAtTheEdgesOfEveryContainer) {
  // every container's first and last value, and one beside each: the bounds a search that stops
  // a step early or late gets wrong, whether the container is an array or a bitmap
  Pool pool;
  std::mt19937 random(9);
  Set dense(&pool.pool), sparse(&pool.pool);
  const Values rd = RandomValues(random, 10, 6);
  Values rs = RandomSparseValues(random, 10, 6);
  while (rs.size() > ARNM_ROARING_SPARSE_MAX) { rs.pop_back(); }
  Build(&dense.set, rd, &pool.pool);
  Build(&sparse.set, rs, &pool.pool);
  ASSERT_GT(dense.set.count, 1u);
  Values all;
  std::set_union(rd.begin(), rd.end(), rs.begin(), rs.end(), std::back_inserter(all));
  const arnm_roaring_bitmap *both[2] = {&dense.set, &sparse.set};
  for (uint32_t c = 0; c < dense.set.count; ++c) {
    const Values in_key = InRange(
        rd, dense.set.containers[c].key << 16, (dense.set.containers[c].key << 16) | 0xffffu
    );
    const uint32_t first = in_key.front(), last = in_key.back();
    const std::pair<uint32_t, uint32_t> ranges[] = {
        {first - 1u, first - 1u}, {first, first},         {first + 1u, last - 1u},
        {first - 1u, last + 1u},  {last - 1u, last - 1u}, {last, last},
        {last + 1u, last + 1u},   {first, last - 1u},     {first + 1u, last}
    };
    for (const auto &range : ranges) {
      const uint32_t min = range.first, max = range.second;
      ASSERT_EQ(arnm_roaring_range_cardinality(&dense.set, min, max), CountInRange(rd, min, max))
          << "container " << c << " [" << min << ", " << max << "]";
      Set out(&pool.pool);
      ASSERT_EQ(arnm_roaring_copy_range(&out.set, &dense.set, min, max, &pool.pool), ARNM_SUCCESS);
      ExpectValid(out.set, InRange(rd, min, max));
      const Values expected = InRange(all, min, max);
      uint64_t count = 0;
      ASSERT_EQ(arnm_roaring_union_cardinality(both, 2, min, max, &count), ARNM_SUCCESS);
      ASSERT_EQ(count, expected.size()) << "[" << min << ", " << max << "]";
      for (bool descending : {false, true}) {
        Values page(8);
        uint32_t written = 0;
        ASSERT_EQ(
            arnm_roaring_union_page(both, 2, min, max, 1, 8, descending, page.data(), &written),
            ARNM_SUCCESS
        );
        page.resize(written);
        Values want;
        for (size_t i = 1; i < expected.size() && want.size() < 8u; ++i) {
          want.push_back(descending ? expected[expected.size() - 1u - i] : expected[i]);
        }
        ASSERT_EQ(page, want) << "[" << min << ", " << max << "] descending " << descending;
      }
    }
  }
}

TEST(RoaringBitmap, SetOperationsRefuseAResultTheyCannotStartFrom) {
  Pool pool;
  Set a(&pool.pool), b(&pool.pool), out(&pool.pool);
  Build(&a.set, {1, 2, 3}, &pool.pool);
  Build(&b.set, {2, 3, 4}, &pool.pool);
  for (Op op : {Op::And, Op::Or, Op::AndNot, Op::CopyRange}) {
    EXPECT_EQ(
        RunOp(op, &a.set, &a.set, &b.set, 0, UINT32_MAX, &pool.pool), ARNM_ERROR_INVALID_PARAM
    );
    EXPECT_EQ(
        RunOp(op, nullptr, &a.set, &b.set, 0, UINT32_MAX, &pool.pool), ARNM_ERROR_NULL_POINTER
    );
    EXPECT_EQ(
        RunOp(op, &out.set, nullptr, &b.set, 0, UINT32_MAX, &pool.pool), ARNM_ERROR_NULL_POINTER
    );
    EXPECT_EQ(RunOp(op, &out.set, &a.set, &b.set, 0, UINT32_MAX, nullptr), ARNM_ERROR_NULL_POINTER);
    // an empty range is an empty result, not a refusal
    EXPECT_EQ(RunOp(op, &out.set, &a.set, &b.set, 3, 2, &pool.pool), ARNM_SUCCESS);
    EXPECT_EQ(out.set.cardinality, 0u);
  }
  EXPECT_EQ(
      arnm_roaring_and(&b.set, &a.set, &a.set, 0, UINT32_MAX, &pool.pool), ARNM_ERROR_INVALID_PARAM
  ) << "a result that is not empty";
  EXPECT_EQ(arnm_roaring_or(&out.set, &a.set, &b.set, 0, UINT32_MAX, &pool.pool), ARNM_SUCCESS);
  EXPECT_EQ(
      arnm_roaring_or(&out.set, &a.set, &b.set, 0, UINT32_MAX, &pool.pool), ARNM_ERROR_INVALID_PARAM
  );
  ExpectValid(out.set, {1, 2, 3, 4});
}

// ---------------------------------------------------------------------------
// reading a union without building it
// ---------------------------------------------------------------------------

TEST(RoaringBitmap, UnionReadsMatchTheReference) {
  Pool pool;
  std::mt19937 random(5);
  for (int round = 0; round < 25; ++round) {
    const uint32_t first_key = random() % 100u;
    const uint32_t count = 1u + random() % 5u;
    std::vector<Values> references(count);
    std::vector<std::unique_ptr<Set>> sets;
    std::vector<const arnm_roaring_bitmap *> pointers;
    Values all;
    for (uint32_t i = 0; i < count; ++i) {
      sets.push_back(std::make_unique<Set>(&pool.pool));
      if (random() % 6 == 0) { // an empty set, or none at all
        pointers.push_back(random() % 2 ? &sets.back()->set : nullptr);
        continue;
      }
      references[i] = RandomAnyValues(random, first_key + random() % 3u, 1u + random() % 4u);
      Build(&sets.back()->set, references[i], &pool.pool);
      pointers.push_back(&sets.back()->set);
      all.insert(all.end(), references[i].begin(), references[i].end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());

    const uint32_t span_low = first_key << 16;
    const uint32_t span_high = (first_key + 7u) << 16;
    for (int probe = 0; probe < 60; ++probe) {
      uint32_t min = span_low + random() % (span_high - span_low);
      uint32_t max = span_low + random() % (span_high - span_low);
      if (probe == 0) {
        min = 0;
        max = UINT32_MAX;
      } else if (probe % 4 == 0 && !all.empty()) {
        // ends on values of the union, or one beside them
        min = all[random() % all.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
        max = all[random() % all.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
        if (min > max) { std::swap(min, max); }
      } else if (probe % 9 != 0 && min > max) {
        std::swap(min, max); // an empty range now and then
      }
      const Values in_range = InRange(all, min, max);
      uint64_t cardinality = 0;
      ASSERT_EQ(
          arnm_roaring_union_cardinality(pointers.data(), count, min, max, &cardinality),
          ARNM_SUCCESS
      );
      ASSERT_EQ(cardinality, in_range.size()) << "[" << min << ", " << max << "]";

      const uint32_t skip = static_cast<uint32_t>(random() % (in_range.size() + 3u));
      const uint32_t size = 1u + static_cast<uint32_t>(random() % (probe % 2 ? 20u : 9000u));
      for (bool descending : {false, true}) {
        Values page(size, 0xdeadbeefu);
        uint32_t written = 7;
        ASSERT_EQ(
            arnm_roaring_union_page(
                pointers.data(), count, min, max, skip, size, descending, page.data(), &written
            ),
            ARNM_SUCCESS
        );
        Values expected;
        for (size_t i = skip; i < in_range.size() && expected.size() < size; ++i) {
          expected.push_back(descending ? in_range[in_range.size() - 1u - i] : in_range[i]);
        }
        page.resize(written);
        ASSERT_EQ(page, expected) << "skip " << skip << " size " << size << " descending "
                                  << descending << " [" << min << ", " << max << "]";
      }
    }
  }
}

TEST(RoaringBitmap, UnionReadsRefuseWhatTheyCannotRead) {
  Pool pool;
  Set a(&pool.pool);
  Build(&a.set, {1, 2, 3}, &pool.pool);
  const arnm_roaring_bitmap *sets[ARNM_ROARING_UNION_MAX + 1u] = {};
  for (auto &set : sets) { set = &a.set; }
  uint64_t cardinality = 99;
  uint32_t page[4] = {};
  uint32_t written = 99;
  EXPECT_EQ(
      arnm_roaring_union_cardinality(
          sets, ARNM_ROARING_UNION_MAX + 1u, 0, UINT32_MAX, &cardinality
      ),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      arnm_roaring_union_cardinality(nullptr, 1, 0, UINT32_MAX, &cardinality),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_roaring_union_cardinality(sets, 1, 0, UINT32_MAX, nullptr), ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(cardinality, 99u);
  EXPECT_EQ(
      arnm_roaring_union_page(
          sets, ARNM_ROARING_UNION_MAX + 1u, 0, UINT32_MAX, 0, 4, false, page, &written
      ),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      arnm_roaring_union_page(sets, 1, 0, UINT32_MAX, 0, 4, false, nullptr, &written),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_roaring_union_page(sets, 1, 0, UINT32_MAX, 0, 4, false, page, nullptr),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(written, 99u);
  // the most sets it takes, all the same: the union is the set itself
  EXPECT_EQ(
      arnm_roaring_union_cardinality(sets, ARNM_ROARING_UNION_MAX, 0, UINT32_MAX, &cardinality),
      ARNM_SUCCESS
  );
  EXPECT_EQ(cardinality, 3u);
  EXPECT_EQ(
      arnm_roaring_union_page(
          sets, ARNM_ROARING_UNION_MAX, 0, UINT32_MAX, 1, 4, true, page, &written
      ),
      ARNM_SUCCESS
  );
  EXPECT_EQ(written, 2u);
  EXPECT_EQ(page[0], 2u);
  EXPECT_EQ(page[1], 1u);
  // no sets, no size: nothing, and a success
  EXPECT_EQ(arnm_roaring_union_cardinality(sets, 0, 0, UINT32_MAX, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
  EXPECT_EQ(
      arnm_roaring_union_page(sets, 1, 0, UINT32_MAX, 0, 0, false, nullptr, &written), ARNM_SUCCESS
  );
  EXPECT_EQ(written, 0u);
}

// ---------------------------------------------------------------------------
// a pool that says no
// ---------------------------------------------------------------------------

TEST(RoaringBitmap, AFailedAddLeavesTheSetAsItWas) {
  // largest grade 4 KiB: an array can reach 2048 values, never a bitmap
  Pool pool(12);
  Set s(&pool.pool);
  Values reference;
  for (uint32_t v = 0; v < 4096u; ++v) {
    const arnm_result result = arnm_roaring_add(&s.set, v, &pool.pool);
    if (ARNM_SUCCESS != result) {
      EXPECT_EQ(result, ARNM_ERROR_RESOURCE_SIZE_EXCEED);
      EXPECT_EQ(v, 2048u);
      break;
    }
    reference.push_back(v);
  }
  ExpectValid(s.set, reference);
  EXPECT_EQ(pool.pool.lent_bytes, BlockBytes(s.set));

  // a sparse block that cannot grow past 32 bytes: eight values, the ninth is refused
  Pool tiny(5);
  Set t(&tiny.pool);
  Values small;
  for (uint32_t i = 1; i <= 8u; ++i) {
    ASSERT_EQ(arnm_roaring_add(&t.set, i << 16, &tiny.pool), ARNM_SUCCESS);
    small.push_back(i << 16);
  }
  EXPECT_EQ(arnm_roaring_add(&t.set, 9u << 16, &tiny.pool), ARNM_ERROR_RESOURCE_SIZE_EXCEED);
  ExpectValid(t.set, small);
  EXPECT_EQ(tiny.pool.lent_bytes, BlockBytes(t.set));
}

TEST(RoaringBitmap, AFailedSwitchToContainersLeavesTheSetSparse) {
  // 1025 values in 1025 keys need a directory of 16 KiB; the pool stops at 4 KiB
  Pool pool(12);
  Set s(&pool.pool);
  Values reference;
  for (uint32_t v = 0; v < ARNM_ROARING_SPARSE_MAX; ++v) {
    ASSERT_EQ(arnm_roaring_add(&s.set, v << 16, &pool.pool), ARNM_SUCCESS);
    reference.push_back(v << 16);
  }
  const uint64_t lent = pool.pool.lent_bytes;
  EXPECT_EQ(
      arnm_roaring_add(&s.set, ARNM_ROARING_SPARSE_MAX << 16, &pool.pool),
      ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  ExpectValid(s.set, reference);
  EXPECT_EQ(pool.pool.lent_bytes, lent) << "every container built on the way went back";
}

TEST(RoaringBitmap, AFailedOperationLeavesTheResultEmpty) {
  // inputs: a first key whose union is a small array, then keys whose union needs a bitmap
  Pool pool;
  Set a(&pool.pool), b(&pool.pool);
  Values ra, rb;
  for (uint32_t i = 0; i < 10u; ++i) {
    ra.push_back(i * 2u);
    rb.push_back(i * 2u + 1u);
  }
  for (uint32_t key = 1; key < 3u; ++key) {
    for (uint32_t i = 0; i < 3000u; ++i) {
      ra.push_back((key << 16) | (i * 2u));
      rb.push_back((key << 16) | (i * 2u + 1u));
    }
  }
  Build(&a.set, ra, &pool.pool);
  Build(&b.set, rb, &pool.pool);
  const uint64_t lent = pool.pool.lent_bytes;

  // a result pool whose largest grade is 4 KiB: the first key goes in, the second cannot
  Pool small(12);
  Set out(&small.pool);
  EXPECT_EQ(
      arnm_roaring_or(&out.set, &a.set, &b.set, 0, UINT32_MAX, &small.pool),
      ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  EXPECT_EQ(out.set.cardinality, 0u);
  EXPECT_EQ(out.set.containers, nullptr);
  EXPECT_EQ(small.pool.lent_bytes, 0u);
  EXPECT_GT(small.pool.cached_bytes, 0u) << "the first key was built before the second failed";
  ExpectValid(a.set, ra);
  ExpectValid(b.set, rb);
  EXPECT_EQ(pool.pool.lent_bytes, lent);
}

TEST(RoaringBitmap, FreeGivesEveryBlockBackForTheNextSet) {
  Pool pool;
  std::mt19937 random(4);
  const Values reference = RandomValues(random, 0, 8);
  {
    Set s(&pool.pool);
    Build(&s.set, reference, &pool.pool);
    EXPECT_EQ(pool.pool.lent_bytes, BlockBytes(s.set));
  }
  EXPECT_EQ(pool.pool.lent_bytes, 0u);
  const uint64_t cached = pool.pool.cached_bytes;
  {
    // the same set again takes nothing new from the chain
    Set s(&pool.pool);
    Build(&s.set, reference, &pool.pool);
    EXPECT_EQ(pool.pool.lent_bytes + pool.pool.cached_bytes, cached);
  }
}
