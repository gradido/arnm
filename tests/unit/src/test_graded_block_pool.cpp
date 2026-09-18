#include "arnm/arena.h"
#include "arnm/bucket_vector.h"
#include "arnm/graded_block_pool.h"
#include "arnm/key_map.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <vector>

/*
 * The graded block pool as a handle: created over a source, then driven only through the calls
 * every allocator answers. Most tests put it over a borrowed arena, because an arena's remaining
 * bytes say exactly what the pool took from its source and when -- which is what reuse means.
 */

namespace {

/** A borrowed arena over a vector of words, so the block is 8 byte aligned. */
struct Arena {
  explicit Arena(uint32_t bytes) : blob(bytes / 8u) {
    EXPECT_EQ(
        arnm_init_arena_borrow(&arena, reinterpret_cast<uint8_t *>(blob.data()), bytes),
        ARNM_SUCCESS
    );
  }
  uint32_t Remaining() const {
    return arnm_arena_remaining(&arena);
  }
  std::vector<uint64_t> blob;
  arnm arena{};
};

arnm *MakePool(arnm *source, uint8_t min_log2 = 0, uint8_t max_log2 = 0) {
  arnm_graded_block_pool_options options{};
  options.min_block_log2 = min_log2;
  options.max_block_log2 = max_log2;
  return arnm_graded_block_pool_create(&options, source);
}

arnm_graded_block_pool_stats Stats(const arnm *pool) {
  arnm_graded_block_pool_stats stats{};
  EXPECT_EQ(arnm_graded_block_pool_measure(pool, &stats), ARNM_SUCCESS);
  return stats;
}

const uint32_t kPoolBytes = [] {
  // what create takes from the source: the handle and the state behind it, rounded to 8
  Arena probe(4096);
  const uint32_t before = probe.Remaining();
  arnm *pool = MakePool(&probe.arena);
  const uint32_t taken = before - probe.Remaining();
  arnm_destroy(pool, &probe.arena);
  return taken;
}();

} // namespace

// ---------------------------------------------------------------------------
// creation
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, OptionsTakeDefaultsAndRefuseWhatCannotBe) {
  arnm_graded_block_pool_options options{};
  ASSERT_EQ(arnm_graded_block_pool_options_validate(&options), ARNM_SUCCESS);
  EXPECT_EQ(options.min_block_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2);
  EXPECT_EQ(options.max_block_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2);

  EXPECT_EQ(arnm_graded_block_pool_options_validate(nullptr), ARNM_ERROR_NULL_POINTER);
  for (auto bad : {std::pair<uint8_t, uint8_t>{2, 10}, {3, 32}, {12, 11}}) {
    options.min_block_log2 = bad.first;
    options.max_block_log2 = bad.second;
    EXPECT_EQ(arnm_graded_block_pool_options_validate(&options), ARNM_ERROR_INVALID_PARAM)
        << int(bad.first) << ".." << int(bad.second);
    EXPECT_EQ(arnm_graded_block_pool_create(&options, nullptr), nullptr);
  }
  options.min_block_log2 = 3;
  options.max_block_log2 = 31;
  EXPECT_EQ(arnm_graded_block_pool_options_validate(&options), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_block_pool_create(nullptr, nullptr), nullptr);
}

TEST(GradedBlockPool, CreateTakesOnlyItsOwnBytesAndIsNoArena) {
  Arena source(4096);
  arnm *pool = MakePool(&source.arena, 5, 12);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(source.Remaining(), 4096u - kPoolBytes);
  EXPECT_TRUE(arnm_is_graded_block_pool(pool));
  EXPECT_FALSE(arnm_is_arena(pool));
  EXPECT_FALSE(arnm_is_multi_arena(pool));
  EXPECT_EQ(arnm_arena_remaining(pool), 0u);
  EXPECT_FALSE(arnm_is_graded_block_pool(&source.arena));
  EXPECT_FALSE(arnm_is_graded_block_pool(nullptr));

  const auto stats = Stats(pool);
  EXPECT_EQ(stats.lent_bytes, 0u);
  EXPECT_EQ(stats.cached_bytes, 0u);
  EXPECT_EQ(stats.oversized_bytes, 0u);
  EXPECT_EQ(stats.min_block_log2, 5u);
  EXPECT_EQ(stats.max_block_log2, 12u);

  arnm_graded_block_pool_stats untouched{};
  untouched.lent_bytes = 99;
  EXPECT_EQ(arnm_graded_block_pool_measure(&source.arena, &untouched), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(arnm_graded_block_pool_measure(nullptr, &untouched), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_block_pool_measure(pool, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(untouched.lent_bytes, 99u);

  // a source with no room for the pool itself
  Arena tiny(16);
  EXPECT_EQ(MakePool(&tiny.arena), nullptr);
  EXPECT_EQ(arnm_destroy(pool, &source.arena), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// grades and reuse
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, RequestsRoundUpToTheirGrade) {
  Arena source(1u << 20);
  arnm *pool = MakePool(&source.arena, 4, 12);
  const struct {
    uint32_t request;
    uint32_t block;
  } cases[] = {{1, 16},    {8, 16},    {16, 16},   {17, 32},     {24, 32},    {33, 64},
               {100, 128}, {128, 128}, {129, 256}, {1000, 1024}, {4096, 4096}};
  uint64_t lent = 0;
  std::vector<std::pair<uint8_t *, uint32_t>> blocks;
  for (const auto &c : cases) {
    const uint32_t before = source.Remaining();
    uint8_t *block = nullptr;
    ASSERT_EQ(arnm_alloc(&block, c.request, pool), ARNM_SUCCESS) << c.request;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % 8u, 0u);
    EXPECT_EQ(before - source.Remaining(), c.block) << "request " << c.request;
    memset(block, 0xa5, c.block); // the whole block is the caller's
    lent += c.block;
    EXPECT_EQ(Stats(pool).lent_bytes, lent);
    blocks.push_back({block, c.request});
  }
  for (const auto &[block, request] : blocks) {
    ASSERT_EQ(arnm_free(block, request, pool), ARNM_SUCCESS);
  }
  EXPECT_EQ(Stats(pool).lent_bytes, 0u);
  EXPECT_EQ(Stats(pool).cached_bytes, lent);
  arnm_destroy(pool, &source.arena);
}

TEST(GradedBlockPool, AFreedBlockServesTheNextRequestOfItsGrade) {
  Arena source(1u << 16);
  arnm *pool = MakePool(&source.arena);
  uint8_t *first = nullptr, *second = nullptr, *other = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 40, pool), ARNM_SUCCESS);  // 64 byte grade
  ASSERT_EQ(arnm_alloc(&second, 60, pool), ARNM_SUCCESS); // 64 byte grade
  ASSERT_EQ(arnm_alloc(&other, 20, pool), ARNM_SUCCESS);  // 32 byte grade
  const uint32_t remaining = source.Remaining();

  ASSERT_EQ(arnm_free(first, 40, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(second, 60, pool), ARNM_SUCCESS);
  uint8_t *again = nullptr;
  ASSERT_EQ(arnm_alloc(&again, 33, pool), ARNM_SUCCESS);
  EXPECT_EQ(again, second) << "last freed, first served";
  ASSERT_EQ(arnm_alloc(&again, 64, pool), ARNM_SUCCESS);
  EXPECT_EQ(again, first);
  EXPECT_EQ(source.Remaining(), remaining) << "both came from the free list, not the source";

  // a different grade does not take them
  ASSERT_EQ(arnm_free(again, 64, pool), ARNM_SUCCESS);
  uint8_t *small = nullptr;
  ASSERT_EQ(arnm_alloc(&small, 20, pool), ARNM_SUCCESS);
  EXPECT_NE(small, again);
  EXPECT_EQ(source.Remaining(), remaining - 32u);
  arnm_destroy(pool, &source.arena);
}

TEST(GradedBlockPool, ChurnOfMixedSizesStopsTakingFromTheSource) {
  // a working set of 200 blocks of random sizes, replaced one at a time for many rounds: once the
  // free lists hold the peak of each grade, the source is not asked again
  Arena source(1u << 22);
  arnm *pool = MakePool(&source.arena, 4, 16);
  std::mt19937_64 rng(7);
  struct Held {
    uint8_t *block;
    uint32_t size;
    uint8_t fill;
  };
  std::vector<Held> held;
  auto take = [&] {
    Held h{nullptr, 1u + static_cast<uint32_t>(rng() % 3000u), static_cast<uint8_t>(rng())};
    ASSERT_EQ(arnm_alloc(&h.block, h.size, pool), ARNM_SUCCESS);
    memset(h.block, h.fill, h.size);
    held.push_back(h);
  };
  for (int i = 0; i < 200; ++i) { take(); }
  uint32_t remaining_after_warmup = 0;
  for (int round = 0; round < 20000; ++round) {
    const size_t victim = rng() % held.size();
    for (uint32_t b = 0; b < held[victim].size; ++b) {
      ASSERT_EQ(held[victim].block[b], held[victim].fill) << "a block was handed out twice";
    }
    ASSERT_EQ(arnm_free(held[victim].block, held[victim].size, pool), ARNM_SUCCESS);
    held.erase(held.begin() + static_cast<long>(victim));
    take();
    if (round == 10000) { remaining_after_warmup = source.Remaining(); }
  }
  // the last half asked for nothing new that the first half had not already cached, give or take
  // a grade whose peak rose once more
  EXPECT_LE(remaining_after_warmup - source.Remaining(), 4u * 4096u);
  const auto stats = Stats(pool);
  uint64_t lent = 0;
  for (const auto &h : held) {
    uint32_t block = 16;
    while (block < ((h.size + 7u) & ~7u)) { block <<= 1; }
    lent += block;
  }
  EXPECT_EQ(stats.lent_bytes, lent);
  EXPECT_EQ((1u << 22) - kPoolBytes - source.Remaining(), stats.lent_bytes + stats.cached_bytes);
  arnm_destroy(pool, &source.arena);
}

// ---------------------------------------------------------------------------
// past the largest grade
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, OversizedGoesStraightToTheSource) {
  Arena source(1u << 16);
  arnm *pool = MakePool(&source.arena, 4, 10); // largest grade 1024
  const uint32_t before = source.Remaining();
  uint8_t *big = nullptr;
  ASSERT_EQ(arnm_alloc(&big, 1025, pool), ARNM_SUCCESS);
  EXPECT_EQ(before - source.Remaining(), 1032u) << "rounded to 8, not to a grade";
  EXPECT_EQ(Stats(pool).oversized_bytes, 1032u);
  EXPECT_EQ(Stats(pool).lent_bytes, 0u);

  // at the tail of the arena, so the source takes it back
  ASSERT_EQ(arnm_free(big, 1025, pool), ARNM_SUCCESS);
  EXPECT_EQ(source.Remaining(), before);
  EXPECT_EQ(Stats(pool).oversized_bytes, 0u);
  EXPECT_EQ(Stats(pool).cached_bytes, 0u) << "never cached";

  // buried: the source's warning comes through
  ASSERT_EQ(arnm_alloc(&big, 2000, pool), ARNM_SUCCESS);
  uint8_t *after = nullptr;
  ASSERT_EQ(arnm_alloc(&after, 16, pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_free(big, 2000, pool), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(Stats(pool).oversized_bytes, 0u);
  arnm_destroy(pool, &source.arena);
}

// ---------------------------------------------------------------------------
// realloc
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, ReallocStaysInItsGradeAndMovesAcrossOne) {
  Arena source(1u << 16);
  arnm *pool = MakePool(&source.arena, 4, 10);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_realloc(&block, 0, 40, pool), ARNM_SUCCESS) << "NULL is an allocation";
  for (uint8_t i = 0; i < 40; ++i) { block[i] = i; }
  const uint8_t *first = block;
  const uint32_t remaining = source.Remaining();

  ASSERT_EQ(arnm_realloc(&block, 40, 64, pool), ARNM_SUCCESS);
  EXPECT_EQ(block, first) << "33..64 is one grade";
  ASSERT_EQ(arnm_realloc(&block, 64, 33, pool), ARNM_SUCCESS);
  EXPECT_EQ(block, first);
  EXPECT_EQ(source.Remaining(), remaining);

  ASSERT_EQ(arnm_realloc(&block, 33, 300, pool), ARNM_SUCCESS);
  EXPECT_NE(block, first);
  for (uint8_t i = 0; i < 33; ++i) { ASSERT_EQ(block[i], i); }
  EXPECT_EQ(Stats(pool).lent_bytes, 512u);
  EXPECT_EQ(Stats(pool).cached_bytes, 64u) << "the old block went to its free list";

  // into oversized and back, contents carried both ways
  for (uint32_t i = 0; i < 300; ++i) { block[i] = static_cast<uint8_t>(i * 7u); }
  ASSERT_EQ(arnm_realloc(&block, 300, 5000, pool), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 300; ++i) { ASSERT_EQ(block[i], static_cast<uint8_t>(i * 7u)); }
  EXPECT_EQ(Stats(pool).oversized_bytes, 5000u);
  EXPECT_EQ(Stats(pool).lent_bytes, 0u);
  ASSERT_EQ(arnm_realloc(&block, 5000, 6000, pool), ARNM_SUCCESS) << "the arena's tail grows";
  EXPECT_EQ(Stats(pool).oversized_bytes, 6000u);
  const arnm_result back = arnm_realloc(&block, 6000, 100, pool);
  EXPECT_TRUE(back == ARNM_SUCCESS || back == ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  for (uint32_t i = 0; i < 100; ++i) { ASSERT_EQ(block[i], static_cast<uint8_t>(i * 7u)); }
  EXPECT_EQ(Stats(pool).oversized_bytes, 0u);
  EXPECT_EQ(Stats(pool).lent_bytes, 128u);

  ASSERT_EQ(arnm_realloc(&block, 100, 0, pool), ARNM_SUCCESS) << "0 is a free";
  EXPECT_EQ(block, nullptr);
  EXPECT_EQ(Stats(pool).lent_bytes, 0u);
  arnm_destroy(pool, &source.arena);
}

TEST(GradedBlockPool, RefusalLeavesEverythingAsItWas) {
  Arena source(kPoolBytes + 64u);
  arnm *pool = MakePool(&source.arena, 4, 10);
  ASSERT_NE(pool, nullptr);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_alloc(&block, 50, pool), ARNM_SUCCESS); // the arena's last 64 bytes
  memset(block, 0x3c, 50);

  uint8_t *untouched = reinterpret_cast<uint8_t *>(0x1000);
  EXPECT_EQ(arnm_alloc(&untouched, 8, pool), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(untouched, reinterpret_cast<uint8_t *>(0x1000));
  EXPECT_EQ(arnm_alloc(&untouched, 4000, pool), ARNM_ERROR_OUT_OF_MEMORY) << "oversized too";

  uint8_t *kept = block;
  EXPECT_EQ(arnm_realloc(&kept, 50, 200, pool), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(kept, block);
  for (int i = 0; i < 50; ++i) { ASSERT_EQ(kept[i], 0x3c); }
  const auto stats = Stats(pool);
  EXPECT_EQ(stats.lent_bytes, 64u);
  EXPECT_EQ(stats.cached_bytes, 0u);
  EXPECT_EQ(stats.oversized_bytes, 0u);

  // what was freed can still be had
  ASSERT_EQ(arnm_free(block, 50, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&block, 64, pool), ARNM_SUCCESS);
  arnm_destroy(pool, &source.arena);
}

TEST(GradedBlockPool, MisuseTheCountersCanSeeIsRefused) {
  Arena source(1u << 14);
  arnm *pool = MakePool(&source.arena, 4, 10);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_alloc(&block, 16, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(block, 16, pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_free(block, 16, pool), ARNM_ERROR_INVALID_STATE) << "plain double free";
  EXPECT_EQ(Stats(pool).cached_bytes, 16u);

  ASSERT_EQ(arnm_alloc(&block, 16, pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_free(block, 1000, pool), ARNM_ERROR_INVALID_STATE) << "a larger grade than out";
  EXPECT_EQ(arnm_free(block, 4000, pool), ARNM_ERROR_INVALID_STATE) << "oversized never out";
  EXPECT_EQ(arnm_free(block, 0, pool), ARNM_ERROR_INVALID_PARAM);
  uint8_t *moving = block;
  EXPECT_EQ(arnm_realloc(&moving, 0, 64, pool), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_realloc(&moving, 4000, 64, pool), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(moving, block);
  EXPECT_EQ(arnm_free(nullptr, 16, pool), ARNM_SUCCESS) << "nothing back is nothing to do";
  EXPECT_EQ(Stats(pool).lent_bytes, 16u);

  uint8_t *out = nullptr;
  EXPECT_EQ(arnm_alloc(&out, 0, pool), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_alloc(nullptr, 8, pool), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_alloc(&out, UINT32_MAX, pool), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  arnm_destroy(pool, &source.arena);
}

// ---------------------------------------------------------------------------
// reset, release, destroy
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, ResetOverAnArenaForgetsTheListsAndLeavesTheSourceAlone) {
  Arena source(1u << 14);
  arnm *pool = MakePool(&source.arena);
  uint8_t *a = nullptr, *b = nullptr;
  ASSERT_EQ(arnm_alloc(&a, 100, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(a, 100, pool), ARNM_SUCCESS);
  const uint32_t remaining = source.Remaining();

  arnm_reset(pool);
  EXPECT_EQ(source.Remaining(), remaining) << "the source is borrowed";
  auto stats = Stats(pool);
  EXPECT_EQ(stats.lent_bytes + stats.cached_bytes + stats.oversized_bytes, 0u);
  ASSERT_EQ(arnm_alloc(&b, 100, pool), ARNM_SUCCESS);
  EXPECT_NE(b, a) << "the cached block was forgotten";
  EXPECT_EQ(source.Remaining(), remaining - 128u);
  arnm_destroy(pool, &source.arena);
}

TEST(GradedBlockPool, ResetOverTheHostGivesTheBlocksBackInstead) {
  // Forgetting a malloc'd block loses it, so over the host a reset is a release: the cached
  // blocks are freed (ASan reports them otherwise) and a block still out can still come back.
  // The same for a zeroed handle, which is the host too.
  arnm zeroed{};
  for (arnm *source : {static_cast<arnm *>(nullptr), &zeroed}) {
    SCOPED_TRACE(source ? "zeroed handle" : "NULL");
    arnm *pool = MakePool(source, 4, 12);
    ASSERT_NE(pool, nullptr);
    uint8_t *kept = nullptr, *cached = nullptr, *big = nullptr;
    ASSERT_EQ(arnm_alloc(&kept, 40, pool), ARNM_SUCCESS);
    ASSERT_EQ(arnm_alloc(&cached, 300, pool), ARNM_SUCCESS);
    ASSERT_EQ(arnm_alloc(&big, 10000, pool), ARNM_SUCCESS);
    ASSERT_EQ(arnm_free(cached, 300, pool), ARNM_SUCCESS);

    arnm_reset(pool);
    auto stats = Stats(pool);
    EXPECT_EQ(stats.cached_bytes, 0u);
    EXPECT_EQ(stats.lent_bytes, 64u) << "the block still out is still counted";
    EXPECT_EQ(stats.oversized_bytes, 10000u);
    EXPECT_EQ(arnm_free(kept, 40, pool), ARNM_SUCCESS) << "and the pool still takes it back";
    EXPECT_EQ(arnm_free(big, 10000, pool), ARNM_SUCCESS);
    EXPECT_EQ(arnm_destroy(pool, source), ARNM_SUCCESS);
  }
}

TEST(GradedBlockPool, ResetOverAnotherPoolGivesTheBlocksBackToIt) {
  // the outer pool would count a block the inner one forgot as lent for good
  arnm *outer = MakePool(nullptr, 4, 16);
  ASSERT_NE(outer, nullptr);
  const uint64_t outer_lent_empty = Stats(outer).lent_bytes; // the inner pool's own bytes, later
  arnm *inner = MakePool(outer, 5, 10);
  ASSERT_NE(inner, nullptr);
  const uint64_t outer_lent_with_inner = Stats(outer).lent_bytes;
  EXPECT_GT(outer_lent_with_inner, outer_lent_empty);

  uint8_t *a = nullptr, *b = nullptr;
  ASSERT_EQ(arnm_alloc(&a, 100, inner), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&b, 500, inner), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(b, 500, inner), ARNM_SUCCESS);
  EXPECT_EQ(Stats(outer).lent_bytes, outer_lent_with_inner + 128u + 512u);

  arnm_reset(inner);
  EXPECT_EQ(Stats(inner).cached_bytes, 0u);
  EXPECT_EQ(Stats(inner).lent_bytes, 128u);
  EXPECT_EQ(Stats(outer).lent_bytes, outer_lent_with_inner + 128u) << "the 512 went back";
  EXPECT_EQ(Stats(outer).cached_bytes, 512u);

  ASSERT_EQ(arnm_free(a, 100, inner), ARNM_SUCCESS);
  EXPECT_EQ(arnm_destroy(inner, outer), ARNM_SUCCESS);
  EXPECT_EQ(Stats(outer).lent_bytes, outer_lent_empty);
  EXPECT_EQ(arnm_destroy(outer, nullptr), ARNM_SUCCESS);
}

TEST(GradedBlockPool, ReleaseGivesCachedBlocksBackAndKeepsWorking) {
  // on the host, so a block release forgets is a leak ASan reports
  arnm *pool = MakePool(nullptr, 4, 12);
  ASSERT_NE(pool, nullptr);
  std::vector<std::pair<uint8_t *, uint32_t>> blocks;
  for (uint32_t size : {10u, 50u, 50u, 300u, 3000u, 9000u}) {
    uint8_t *block = nullptr;
    ASSERT_EQ(arnm_alloc(&block, size, pool), ARNM_SUCCESS);
    blocks.push_back({block, size});
  }
  // free all but one, so release has cached and lent blocks to tell apart
  for (size_t i = 1; i < blocks.size(); ++i) {
    ASSERT_EQ(arnm_free(blocks[i].first, blocks[i].second, pool), ARNM_SUCCESS);
  }
  EXPECT_GT(Stats(pool).cached_bytes, 0u);

  arnm_release(pool);
  auto stats = Stats(pool);
  EXPECT_EQ(stats.cached_bytes, 0u);
  EXPECT_EQ(stats.lent_bytes, 16u) << "the block still out is the caller's";
  EXPECT_TRUE(arnm_is_graded_block_pool(pool));

  uint8_t *again = nullptr;
  ASSERT_EQ(arnm_alloc(&again, 50, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(again, 50, pool), ARNM_SUCCESS);
  ASSERT_EQ(arnm_free(blocks[0].first, blocks[0].second, pool), ARNM_SUCCESS);
  EXPECT_EQ(arnm_destroy(pool, nullptr), ARNM_SUCCESS);
}

TEST(GradedBlockPool, WorksOverAChainAndOverAnotherPool) {
  arnm_multi_arena_options chain_options{};
  chain_options.arena_capacity = 4096;
  arnm *chain = arnm_create_multi_arena(&chain_options, nullptr);
  ASSERT_NE(chain, nullptr);
  arnm *outer = MakePool(chain, 4, 14);
  ASSERT_NE(outer, nullptr);
  arnm *inner = MakePool(outer, 5, 8);
  ASSERT_NE(inner, nullptr);

  std::vector<std::pair<uint8_t *, uint32_t>> blocks;
  for (uint32_t i = 1; i < 400; ++i) {
    uint8_t *block = nullptr;
    const uint32_t size = (i * 37u) % 1500u + 1u;
    ASSERT_EQ(arnm_alloc(&block, size, inner), ARNM_SUCCESS);
    memset(block, static_cast<int>(i), size);
    blocks.push_back({block, size});
  }
  std::set<uint8_t *> distinct;
  for (const auto &[block, size] : blocks) { distinct.insert(block); }
  EXPECT_EQ(distinct.size(), blocks.size());
  for (const auto &[block, size] : blocks) {
    ASSERT_EQ(arnm_free(block, size, inner), ARNM_SUCCESS);
  }

  EXPECT_EQ(arnm_destroy(inner, outer), ARNM_SUCCESS) << "a graded block always goes back";
  // the outer pool's own bytes sit buried in one of the chain's arenas
  EXPECT_EQ(arnm_destroy(outer, chain), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  arnm_destroy(chain, nullptr);
}

// ---------------------------------------------------------------------------
// containers over a pool
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, ContainersOverAPoolReuseWhatTheyOutgrow) {
  // The same key map filled twice behind the same arena size: directly, every superseded table
  // stays in the arena; through a pool, a table outgrown is reused by the next growth of a
  // smaller grade -- here the key vector's index array and the next map's first tables.
  const uint32_t capacity = 8u * 1024u * 1024u;
  auto fill = [](arnm *memory) {
    for (int round = 0; round < 4; ++round) {
      arnm_key_map map;
      EXPECT_EQ(arnm_key_map_init(&map, 8, 10, memory), ARNM_SUCCESS);
      for (uint64_t k = 0; k < 20000; ++k) {
        uint32_t id = 0;
        EXPECT_EQ(
            arnm_key_map_get_or_insert(&map, reinterpret_cast<const uint8_t *>(&k), &id, nullptr),
            ARNM_SUCCESS
        );
        EXPECT_EQ(id, k);
      }
      for (uint64_t k = 0; k < 20000; k += 101) {
        uint32_t id = 0;
        EXPECT_TRUE(arnm_key_map_find(&map, reinterpret_cast<const uint8_t *>(&k), &id));
        EXPECT_EQ(id, k);
      }
      arnm_key_map_free(&map);
    }
  };

  Arena direct(capacity);
  fill(&direct.arena);
  const uint32_t direct_used = capacity - direct.Remaining();

  Arena under_pool(capacity);
  arnm *pool = MakePool(&under_pool.arena, 4, 20);
  fill(pool);
  const uint32_t pool_used = capacity - under_pool.Remaining();
  EXPECT_LT(pool_used, direct_used / 2u)
      << "direct " << direct_used << " bytes, through the pool " << pool_used;
  EXPECT_EQ(Stats(pool).lent_bytes, 0u) << "every map gave everything back";

  arnm_bvec vec;
  ASSERT_EQ(arnm_bvec_init(&vec, 6, 1, sizeof(uint64_t), pool), ARNM_SUCCESS);
  for (uint64_t i = 0; i < 50000; ++i) { ASSERT_EQ(arnm_bvec_push_ptr(&vec, &i), ARNM_SUCCESS); }
  for (uint64_t i = 0; i < 50000; i += 997) {
    ASSERT_EQ(*static_cast<uint64_t *>(arnm_bvec_get(&vec, static_cast<uint32_t>(i))), i);
  }
  arnm_bvec_free(&vec);
  EXPECT_EQ(Stats(pool).lent_bytes, 0u);
  arnm_destroy(pool, &under_pool.arena);
}
