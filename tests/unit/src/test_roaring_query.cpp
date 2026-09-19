#include "arnm/roaring_bitmap.h"
#include "arnm/roaring_ops.h"
#include "arnm/roaring_query.h"

#include "memory_limit.h"
#include "roaring_support.h"

#include <iterator>
#include <memory>

/*
 * Answers over several sets against the same answers over sorted vectors: counts and pages of a
 * union, of an intersection, of a difference, and of the three together.
 */

TEST(RoaringQuery, RangesAtTheEdgesOfEveryContainer) {
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
      ASSERT_EQ(UnionCardinality(both, 2, min, max, &count), ARNM_SUCCESS);
      ASSERT_EQ(count, expected.size()) << "[" << min << ", " << max << "]";
      for (bool descending : {false, true}) {
        Values page(8);
        uint32_t written = 0;
        ASSERT_EQ(
            UnionPage(both, 2, min, max, 1, 8, descending, page.data(), &written), ARNM_SUCCESS
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

TEST(RoaringQuery, UnionsMatchTheReference) {
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
      ASSERT_EQ(UnionCardinality(pointers.data(), count, min, max, &cardinality), ARNM_SUCCESS);
      ASSERT_EQ(cardinality, in_range.size()) << "[" << min << ", " << max << "]";

      const uint32_t skip = static_cast<uint32_t>(random() % (in_range.size() + 3u));
      const uint32_t size = 1u + static_cast<uint32_t>(random() % (probe % 2 ? 20u : 9000u));
      for (bool descending : {false, true}) {
        Values page(size, 0xdeadbeefu);
        uint32_t written = 7;
        ASSERT_EQ(
            UnionPage(
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

TEST(RoaringQuery, RefusesWhatItCannotRead) {
  Pool pool;
  Set a(&pool.pool);
  Build(&a.set, {1, 2, 3}, &pool.pool);
  const arnm_roaring_bitmap *sets[ARNM_ROARING_QUERY_MAX + 1u] = {};
  for (auto &set : sets) { set = &a.set; }
  uint64_t cardinality = 99;
  uint32_t page[4] = {};
  uint32_t written = 99;
  EXPECT_EQ(
      UnionCardinality(sets, ARNM_ROARING_QUERY_MAX + 1u, 0, UINT32_MAX, &cardinality),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(UnionCardinality(nullptr, 1, 0, UINT32_MAX, &cardinality), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(UnionCardinality(sets, 1, 0, UINT32_MAX, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(cardinality, 99u);
  EXPECT_EQ(
      UnionPage(sets, ARNM_ROARING_QUERY_MAX + 1u, 0, UINT32_MAX, 0, 4, false, page, &written),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      UnionPage(sets, 1, 0, UINT32_MAX, 0, 4, false, nullptr, &written), ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(UnionPage(sets, 1, 0, UINT32_MAX, 0, 4, false, page, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(written, 99u);
  // the most sets it takes, all the same: the union is the set itself
  EXPECT_EQ(
      UnionCardinality(sets, ARNM_ROARING_QUERY_MAX, 0, UINT32_MAX, &cardinality), ARNM_SUCCESS
  );
  EXPECT_EQ(cardinality, 3u);
  EXPECT_EQ(
      UnionPage(sets, ARNM_ROARING_QUERY_MAX, 0, UINT32_MAX, 1, 4, true, page, &written),
      ARNM_SUCCESS
  );
  EXPECT_EQ(written, 2u);
  EXPECT_EQ(page[0], 2u);
  EXPECT_EQ(page[1], 1u);
  // no sets, no size: nothing, and a success
  EXPECT_EQ(UnionCardinality(sets, 0, 0, UINT32_MAX, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
  EXPECT_EQ(UnionPage(sets, 1, 0, UINT32_MAX, 0, 0, false, nullptr, &written), ARNM_SUCCESS);
  EXPECT_EQ(written, 0u);
}

// ---------------------------------------------------------------------------
// a pool that says no
// ---------------------------------------------------------------------------

namespace {

/** The values the query matches, worked out on the references. */
Values Matching(
    const std::vector<Values> &all,
    const std::vector<Values> &any,
    const std::vector<Values> &none,
    uint32_t min,
    uint32_t max
) {
  Values result;
  if (min > max || (all.empty() && any.empty())) { return result; }
  // every candidate comes from one of the lists that say what has to be there
  const Values &first = all.empty() ? any.front() : all.front();
  for (uint32_t value : first) {
    if (value < min || value > max) { continue; }
    bool ok = true;
    for (const Values &set : all) { ok = ok && Holds(set, value); }
    if (ok && !any.empty()) {
      bool found = false;
      for (const Values &set : any) { found = found || Holds(set, value); }
      ok = found;
    }
    for (const Values &set : none) { ok = ok && !Holds(set, value); }
    if (ok) { result.push_back(value); }
  }
  if (all.empty()) {
    // the union: the other sets bring candidates of their own
    for (size_t i = 1; i < any.size(); ++i) {
      for (uint32_t value : any[i]) {
        if (value < min || value > max || Holds(any.front(), value)) { continue; }
        bool ok = true;
        for (const Values &set : none) { ok = ok && !Holds(set, value); }
        if (ok) { result.push_back(value); }
      }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
  }
  return result;
}

} // namespace

TEST(RoaringQuery, EveryShapeMatchesTheReference) {
  Pool pool;
  std::mt19937 random(13);
  for (int round = 0; round < 25; ++round) {
    const uint32_t first_key = random() % 50u;
    // a handful of sets that share values, dense and sparse mixed
    std::vector<Values> references;
    std::vector<std::unique_ptr<Set>> sets;
    std::vector<const arnm_roaring_bitmap *> pointers;
    const uint32_t count = 2u + random() % 4u;
    for (uint32_t i = 0; i < count; ++i) {
      Values values = RandomAnyValues(random, first_key + random() % 2u, 1u + random() % 3u);
      if (i && random() % 2) { values = Correlated(random, references.front(), values, SIZE_MAX); }
      references.push_back(values);
      sets.push_back(std::make_unique<Set>(&pool.pool));
      Build(&sets.back()->set, values, &pool.pool);
      pointers.push_back(&sets.back()->set);
    }

    for (int probe = 0; probe < 12; ++probe) {
      // split the sets over the three lists, sometimes leaving one empty
      std::vector<const arnm_roaring_bitmap *> all, any, none;
      std::vector<Values> all_ref, any_ref, none_ref;
      const uint32_t shape = static_cast<uint32_t>(probe) % 4u;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t where = shape == 0   ? 0u            // all only
                               : shape == 1 ? 1u            // any only
                               : shape == 2 ? (i ? 2u : 0u) // all minus none
                                            : (i % 3u);     // all three
        if (where == 0) {
          all.push_back(pointers[i]);
          all_ref.push_back(references[i]);
        } else if (where == 1) {
          any.push_back(pointers[i]);
          any_ref.push_back(references[i]);
        } else {
          none.push_back(pointers[i]);
          none_ref.push_back(references[i]);
        }
      }
      if (all.empty() && any.empty()) { continue; }

      uint32_t min = 0, max = UINT32_MAX;
      if (probe % 3) {
        const Values &from = all_ref.empty() ? any_ref.front() : all_ref.front();
        if (!from.empty()) {
          min = from[random() % from.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
          max = from[random() % from.size()] + static_cast<uint32_t>(random() % 3u) - 1u;
          if (min > max) { std::swap(min, max); }
        }
      }
      const Values expected = Matching(all_ref, any_ref, none_ref, min, max);

      arnm_roaring_query query{};
      query.all = all.data();
      query.all_count = static_cast<uint32_t>(all.size());
      query.any = any.data();
      query.any_count = static_cast<uint32_t>(any.size());
      query.none = none.data();
      query.none_count = static_cast<uint32_t>(none.size());
      query.min = min;
      query.max = max;

      uint64_t cardinality = 0;
      ASSERT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
      ASSERT_EQ(cardinality, expected.size())
          << "all " << all.size() << " any " << any.size() << " none " << none.size() << " [" << min
          << ", " << max << "]";

      const uint32_t skip = static_cast<uint32_t>(random() % (expected.size() + 2u));
      const uint32_t size = 1u + static_cast<uint32_t>(random() % (probe % 2 ? 20u : 4000u));
      for (bool descending : {false, true}) {
        Values page(size, 0xdeadbeefu);
        uint32_t written = 0;
        ASSERT_EQ(
            arnm_roaring_query_page(&query, skip, size, descending, page.data(), &written),
            ARNM_SUCCESS
        );
        page.resize(written);
        Values want;
        for (size_t i = skip; i < expected.size() && want.size() < size; ++i) {
          want.push_back(descending ? expected[expected.size() - 1u - i] : expected[i]);
        }
        ASSERT_EQ(page, want) << "all " << all.size() << " any " << any.size() << " none "
                              << none.size() << " skip " << skip << " descending " << descending;

        // the same answers asked in one walk
        Values listed(size, 0xdeadbeefu);
        uint32_t listed_written = 0;
        uint64_t listed_cardinality = 0;
        ASSERT_EQ(
            arnm_roaring_query_listing(
                &query, skip, size, descending, listed.data(), &listed_written, &listed_cardinality
            ),
            ARNM_SUCCESS
        );
        listed.resize(listed_written);
        EXPECT_EQ(listed_cardinality, cardinality);
        EXPECT_EQ(listed, page);
      }
    }
  }
}

/**
 * The shape an index asks for: one address's few numbers against a type's dense bitmap, with more
 * types united beside it and one community taken out. The small set drives the key, so the dense
 * ones are asked value by value -- the path that has its own loop.
 */
TEST(RoaringQuery, ASmallSetAgainstDenseOnesMatchesTheReference) {
  Pool pool;
  std::mt19937 random(97);
  for (int round = 0; round < 8; ++round) {
    Values address = RandomSparseValues(random, 0, 3);
    Values type = RandomValues(random, 0, 3);
    Values other_type = RandomValues(random, 0, 3);
    Values community = Correlated(random, address, {}, SIZE_MAX);
    if (address.empty() || type.empty()) { continue; }

    Set address_set(&pool.pool), type_set(&pool.pool), other_set(&pool.pool),
        community_set(&pool.pool);
    Build(&address_set.set, address, &pool.pool);
    Build(&type_set.set, type, &pool.pool);
    Build(&other_set.set, other_type, &pool.pool);
    Build(&community_set.set, community, &pool.pool);

    const arnm_roaring_bitmap *all[2] = {&address_set.set, &type_set.set};
    const arnm_roaring_bitmap *any[1] = {&other_set.set};
    const arnm_roaring_bitmap *none[1] = {&community_set.set};

    for (int probe = 0; probe < 6; ++probe) {
      arnm_roaring_query query{};
      query.all = all;
      query.all_count = 2;
      // with any, without it, and with a community taken out on top
      query.any = probe % 2 ? any : nullptr;
      query.any_count = probe % 2 ? 1u : 0u;
      query.none = probe % 3 ? none : nullptr;
      query.none_count = probe % 3 ? 1u : 0u;
      query.min = probe % 4 ? address[random() % address.size()] : 0u;
      query.max = probe % 4 ? address[random() % address.size()] : UINT32_MAX;
      if (query.min > query.max) { std::swap(query.min, query.max); }

      std::vector<Values> all_ref{address, type};
      std::vector<Values> any_ref;
      std::vector<Values> none_ref;
      if (query.any_count) { any_ref.push_back(other_type); }
      if (query.none_count) { none_ref.push_back(community); }
      const Values expected = Matching(all_ref, any_ref, none_ref, query.min, query.max);

      uint64_t cardinality = 0;
      ASSERT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
      ASSERT_EQ(cardinality, expected.size()) << "round " << round << " probe " << probe;

      const uint32_t skip = static_cast<uint32_t>(random() % (expected.size() + 2u));
      const uint32_t size = 20;
      for (bool descending : {false, true}) {
        Values page(size, 0xdeadbeefu);
        uint32_t written = 0;
        ASSERT_EQ(
            arnm_roaring_query_page(&query, skip, size, descending, page.data(), &written),
            ARNM_SUCCESS
        );
        page.resize(written);
        Values want;
        for (size_t i = skip; i < expected.size() && want.size() < size; ++i) {
          want.push_back(descending ? expected[expected.size() - 1u - i] : expected[i]);
        }
        ASSERT_EQ(page, want) << "round " << round << " probe " << probe << " descending "
                              << descending;

        Values listed(size, 0xdeadbeefu);
        uint32_t listed_written = 0;
        uint64_t listed_cardinality = 0;
        ASSERT_EQ(
            arnm_roaring_query_listing(
                &query, skip, size, descending, listed.data(), &listed_written, &listed_cardinality
            ),
            ARNM_SUCCESS
        );
        listed.resize(listed_written);
        EXPECT_EQ(listed_cardinality, cardinality);
        EXPECT_EQ(listed, page);
      }
    }
  }
}

/**
 * The union of an address's three sparse sets, counted and paged in one walk -- what a listing
 * over an index asks for most. The count reads the spans down, so the page needs its own.
 */
TEST(RoaringQuery, AListingOverSparseSetsCountsAndPages) {
  Pool pool;
  std::mt19937 random(2029);
  for (int round = 0; round < 10; ++round) {
    std::vector<Values> references;
    std::vector<std::unique_ptr<Set>> sets;
    std::vector<const arnm_roaring_bitmap *> pointers;
    for (int i = 0; i < 3; ++i) {
      // sparse, and sharing values with the first: an address is in all three lists at once
      Values values = RandomSparseValues(random, 0, 1u + random() % 3u);
      if (i && random() % 2) { values = Correlated(random, references.front(), values, SIZE_MAX); }
      while (values.size() > ARNM_ROARING_SPARSE_MAX) { values.pop_back(); }
      references.push_back(values);
      sets.push_back(std::make_unique<Set>(&pool.pool));
      Build(&sets.back()->set, values, &pool.pool);
      ASSERT_EQ(sets.back()->set.count, 0u) << "the test needs sparse sets";
      pointers.push_back(&sets.back()->set);
    }
    const Values all_values = Matching({}, references, {}, 0, UINT32_MAX);
    if (all_values.empty()) { continue; }

    for (int probe = 0; probe < 8; ++probe) {
      uint32_t min = 0, max = UINT32_MAX;
      if (probe % 2) {
        min = all_values[random() % all_values.size()];
        max = all_values[random() % all_values.size()];
        if (min > max) { std::swap(min, max); }
      }
      const arnm_roaring_query query = AnyQuery(pointers.data(), 3, min, max);
      const Values expected = Matching({}, references, {}, min, max);

      const uint32_t skip = static_cast<uint32_t>(random() % (expected.size() + 2u));
      const uint32_t size = 1u + static_cast<uint32_t>(random() % 25u);
      for (bool descending : {false, true}) {
        Values listed(size, 0xdeadbeefu);
        uint32_t written = 0;
        uint64_t cardinality = 0;
        ASSERT_EQ(
            arnm_roaring_query_listing(
                &query, skip, size, descending, listed.data(), &written, &cardinality
            ),
            ARNM_SUCCESS
        );
        listed.resize(written);
        EXPECT_EQ(cardinality, expected.size()) << "round " << round << " probe " << probe;
        Values want;
        for (size_t i = skip; i < expected.size() && want.size() < size; ++i) {
          want.push_back(descending ? expected[expected.size() - 1u - i] : expected[i]);
        }
        EXPECT_EQ(listed, want) << "round " << round << " probe " << probe << " descending "
                                << descending;
      }
    }
  }
}

/** A listing without room for a page is a count, and what it refuses it refuses whole. */
TEST(RoaringQuery, AListingWithoutAPageIsACount) {
  Pool pool;
  Set a(&pool.pool), b(&pool.pool);
  Build(&a.set, {1, 2, 3, 70000, 70001}, &pool.pool);
  Build(&b.set, {2, 3, 4, 70001}, &pool.pool);
  const arnm_roaring_bitmap *both[2] = {&a.set, &b.set};
  arnm_roaring_query query{};
  query.all = both;
  query.all_count = 2;
  query.max = UINT32_MAX;

  uint32_t written = 7;
  uint64_t cardinality = 7;
  ASSERT_EQ(
      arnm_roaring_query_listing(&query, 0, 0, false, nullptr, &written, &cardinality), ARNM_SUCCESS
  );
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(cardinality, 3u); // 2, 3, 70001

  // a page that starts past everything still answers the count
  uint32_t page[4] = {};
  ASSERT_EQ(
      arnm_roaring_query_listing(&query, 99, 4, true, page, &written, &cardinality), ARNM_SUCCESS
  );
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(cardinality, 3u);

  written = 7;
  cardinality = 7;
  EXPECT_EQ(
      arnm_roaring_query_listing(&query, 0, 4, false, page, &written, nullptr),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_roaring_query_listing(&query, 0, 4, false, nullptr, &written, &cardinality),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_roaring_query_listing(nullptr, 0, 4, false, page, &written, &cardinality),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(written, 7u);
  EXPECT_EQ(cardinality, 7u);
}

TEST(RoaringQuery, AQueryWithNothingToMatchAnswersNothing) {
  Pool pool;
  Set a(&pool.pool);
  Build(&a.set, {1, 2, 3}, &pool.pool);
  const arnm_roaring_bitmap *sets[1] = {&a.set};
  arnm_roaring_query query{};
  query.none = sets;
  query.none_count = 1;
  query.max = UINT32_MAX;
  uint64_t cardinality = 7;
  uint32_t page[4] = {};
  uint32_t written = 7;
  // none alone says only what must not be there, so nothing matches
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
  EXPECT_EQ(arnm_roaring_query_page(&query, 0, 4, false, page, &written), ARNM_SUCCESS);
  EXPECT_EQ(written, 0u);

  // a NULL set in all matches nothing, in any it brings nothing, in none it takes nothing
  const arnm_roaring_bitmap *with_null[2] = {&a.set, nullptr};
  query = arnm_roaring_query{};
  query.all = with_null;
  query.all_count = 2;
  query.max = UINT32_MAX;
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
  query = arnm_roaring_query{};
  query.any = with_null;
  query.any_count = 2;
  query.max = UINT32_MAX;
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 3u);
  query = arnm_roaring_query{};
  query.all = sets;
  query.all_count = 1;
  query.none = with_null + 1;
  query.none_count = 1;
  query.max = UINT32_MAX;
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 3u);

  // an empty range, and a query that names no set at all
  query.min = 3;
  query.max = 2;
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
  query = arnm_roaring_query{};
  query.max = UINT32_MAX;
  EXPECT_EQ(arnm_roaring_query_cardinality(&query, &cardinality), ARNM_SUCCESS);
  EXPECT_EQ(cardinality, 0u);
}
