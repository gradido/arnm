#include "arnm/roaring_bitmap.h"

#include "memory_limit.h"
#include "roaring_support.h"

/*
 * The set itself: building it in both forms, the switch between them, and reading one set --
 * contains, minimum, maximum, range counts, select and pages -- against a sorted vector.
 */

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
