#include "arnm/roaring_bitmap.h"
#include "arnm/roaring_ops.h"
#include "arnm/roaring_query.h"

#include "memory_limit.h"
#include "roaring_support.h"

#include <iterator>

/*
 * and, or, andnot and copy_range against the same operations on sorted vectors: every pairing of
 * the container kinds and the sparse form, ranges that end on and beside real values, and what a
 * refused block leaves behind.
 */

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

TEST(RoaringOps, SetOperationsMatchTheReference) {
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

TEST(RoaringOps, TwoArraysCanUniteIntoABitmap) {
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

TEST(RoaringOps, SparseAndDenseMeetAtTheEdgesOfAKey) {
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
    ASSERT_EQ(UnionCardinality(sets, 2, range.first, range.second, &count), ARNM_SUCCESS);
    EXPECT_EQ(count, in_range.size());
    for (bool descending : {false, true}) {
      Values page(40);
      uint32_t written = 0;
      ASSERT_EQ(
          UnionPage(sets, 2, range.first, range.second, 3, 40, descending, page.data(), &written),
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

TEST(RoaringOps, SetOperationsRefuseAResultTheyCannotStartFrom) {
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

TEST(RoaringOps, AFailedOperationLeavesTheResultEmpty) {
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
