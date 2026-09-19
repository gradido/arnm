#include "arnm/arena.h"
#include "arnm/graded_block_pool.h"
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
 * The graded block pool: its own calls, its own chain. Blocks come from the chain the pool opens
 * for itself, so the chain's measure says what the pool took and when -- which is what reuse
 * means. The source named at init only carries the bookkeeping; a borrowed arena as that source
 * shows how little.
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

/** A pool on the stack, released when the test leaves -- the init/release pair as a scope. */
struct Pool {
  explicit Pool(
      uint8_t min_log2 = 0,
      uint8_t max_log2 = 0,
      uint8_t arena_capacity = 0,
      arnm *source_ = nullptr
  )
      : source(source_) {
    arnm_graded_block_pool_options options{};
    options.min_block_log2 = min_log2;
    options.max_block_log2 = max_log2;
    options.alloc_arena_capacity = arena_capacity;
    EXPECT_EQ(arnm_graded_block_pool_init(&pool, &options, source), ARNM_SUCCESS);
  }
  ~Pool() {
    arnm_graded_block_pool_release(&pool, source);
  }
  uint8_t *Alloc(uint32_t size) {
    uint8_t *block = nullptr;
    EXPECT_EQ(arnm_graded_block_pool_alloc(&pool, &block, size), ARNM_SUCCESS) << size;
    return block;
  }
  void Free(uint8_t *block, uint32_t size) {
    EXPECT_EQ(arnm_graded_block_pool_free(&pool, block, size), ARNM_SUCCESS) << size;
  }
  arnm_multi_arena_stats Chain() const {
    arnm_multi_arena_stats stats{};
    EXPECT_EQ(arnm_multi_arena_measure(pool.source, &stats), ARNM_SUCCESS);
    return stats;
  }
  /** Bytes cut into blocks: the chain hands out whole arenas, the current one only in part. */
  uint64_t Carved() const {
    return Chain().used - arnm_arena_remaining(&pool.current);
  }
  arnm_graded_block_pool pool{};
  arnm *source;
};

} // namespace

// ---------------------------------------------------------------------------
// setting up
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, OptionsTakeDefaultsAndRefuseWhatCannotBe) {
  arnm_graded_block_pool_options options{};
  ASSERT_EQ(arnm_graded_block_pool_options_validate(&options), ARNM_SUCCESS);
  EXPECT_EQ(options.min_block_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2);
  EXPECT_EQ(options.max_block_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2);
  EXPECT_EQ(options.alloc_arena_capacity, ARNM_GRADED_BLOCK_POOL_DEFAULT_ALLOC_ARENA_CAPACITY);

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

TEST(GradedBlockPool, InitReadsNothingAndLeavesThePoolAloneOnFailure) {
  arnm_graded_block_pool pool;
  std::memset(&pool, 0xab, sizeof(pool));
  arnm_graded_block_pool garbage;
  std::memset(&garbage, 0xab, sizeof(garbage));
  arnm_graded_block_pool_options options{};

  // refused: the struct is exactly as it was
  options.min_block_log2 = 2;
  EXPECT_EQ(arnm_graded_block_pool_init(&pool, &options, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(std::memcmp(&pool, &garbage, sizeof(pool)), 0);
  EXPECT_EQ(arnm_graded_block_pool_init(nullptr, &options, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_block_pool_init(&pool, nullptr, nullptr), ARNM_ERROR_NULL_POINTER);

  // a source without room for the chain's descriptor
  Arena tiny(16);
  options.min_block_log2 = 0;
  EXPECT_EQ(arnm_graded_block_pool_init(&pool, &options, &tiny.arena), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(std::memcmp(&pool, &garbage, sizeof(pool)), 0);

  // garbage in, a working pool out
  ASSERT_EQ(arnm_graded_block_pool_init(&pool, &options, nullptr), ARNM_SUCCESS);
  EXPECT_NE(pool.source, nullptr);
  EXPECT_EQ(pool.lent_bytes, 0u);
  EXPECT_EQ(pool.cached_bytes, 0u);
  EXPECT_EQ(pool.min_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MIN_LOG2);
  EXPECT_EQ(pool.max_log2, ARNM_GRADED_BLOCK_POOL_DEFAULT_MAX_LOG2);
  for (uint8_t *head : pool.free_head) { EXPECT_EQ(head, nullptr); }
  uint8_t *block = nullptr;
  EXPECT_EQ(arnm_graded_block_pool_alloc(&pool, &block, 100), ARNM_SUCCESS);
  arnm_graded_block_pool_release(&pool, nullptr);
}

TEST(GradedBlockPool, TheSourceCarriesOnlyTheBookkeeping) {
  Arena source(4096);
  Pool pool(4, 12, 0, &source.arena);
  EXPECT_LT(source.Remaining(), 4096u);

  // no arena until the first block; then one, from the host
  EXPECT_EQ(pool.Chain().arena_count, 0u);
  pool.Alloc(64);
  const uint32_t after_first = source.Remaining();
  EXPECT_EQ(pool.Chain().arena_count, 1u);

  // everything else the first arena holds costs the source nothing more
  for (int i = 0; i < 100; ++i) { pool.Alloc(64); }
  EXPECT_EQ(pool.Chain().arena_count, 1u);
  EXPECT_EQ(source.Remaining(), after_first);
}

TEST(GradedBlockPool, AnArenaHoldsTheNamedNumberOfLargestBlocks) {
  Pool pool(4, 12, 2); // arenas of 2 * 4 KiB
  pool.Alloc(4096);
  pool.Alloc(4096);
  EXPECT_EQ(pool.Chain().arena_count, 1u);
  EXPECT_EQ(pool.Chain().reserved, 8192u);
  pool.Alloc(4096);
  EXPECT_EQ(pool.Chain().arena_count, 2u);
  EXPECT_EQ(pool.Chain().reserved, 16384u);
}

// ---------------------------------------------------------------------------
// grades and reuse
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, RequestsRoundUpToTheirGrade) {
  Pool pool(4, 12);
  const struct {
    uint32_t request;
    uint32_t block;
  } cases[] = {{1, 16}, {16, 16}, {17, 32}, {20, 32}, {100, 128}, {129, 256}, {4096, 4096}};
  uint64_t lent = 0;
  for (const auto &c : cases) {
    uint8_t *block = pool.Alloc(c.request);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % 8u, 0u) << c.request;
    lent += c.block;
    EXPECT_EQ(pool.pool.lent_bytes, lent) << c.request;
    // the whole grade is the caller's to write
    std::memset(block, 0x5a, c.block);
  }
  EXPECT_EQ(pool.Carved(), lent);
}

TEST(GradedBlockPool, AFreedBlockServesTheNextRequestOfItsGrade) {
  Pool pool(4, 12);
  uint8_t *a = pool.Alloc(40);
  uint8_t *b = pool.Alloc(64);

  pool.Free(a, 40);
  EXPECT_EQ(pool.pool.cached_bytes, 64u);
  EXPECT_EQ(pool.pool.lent_bytes, 64u);

  // another grade does not take it
  EXPECT_NE(pool.Alloc(128), a);
  // any size of the grade does, and the chain gives nothing new
  const uint64_t used = pool.Carved();
  EXPECT_EQ(pool.Alloc(33), a);
  EXPECT_EQ(pool.Carved(), used);
  EXPECT_EQ(pool.pool.cached_bytes, 0u);

  // last in, first out
  pool.Free(a, 64);
  pool.Free(b, 64);
  EXPECT_EQ(pool.Alloc(64), b);
  EXPECT_EQ(pool.Alloc(64), a);
  EXPECT_EQ(pool.Carved(), used);
}

TEST(GradedBlockPool, ChurnOfMixedSizesStopsTakingFromTheChain) {
  Pool pool(4, 12);
  std::mt19937 random(7);
  std::uniform_int_distribution<uint32_t> size_of(1, 4096);
  struct Held {
    uint8_t *block;
    uint32_t size;
  };
  std::vector<Held> held(256);
  for (auto &h : held) {
    h.size = size_of(random);
    h.block = pool.Alloc(h.size);
  }
  auto churn = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      Held &h = held[random() % held.size()];
      pool.Free(h.block, h.size);
      h.size = size_of(random);
      h.block = pool.Alloc(h.size);
      h.block[0] = 1;
      h.block[h.size - 1] = 2;
    }
  };
  // warm up until every grade has come near its peak, then the chain has to stand nearly still
  churn(20000);
  const uint64_t used = pool.Carved();
  churn(20000);
  EXPECT_LE(pool.Carved(), used + 64u * 1024u);

  // the counters add up to what the chain handed out, and nothing is handed out twice
  EXPECT_EQ(pool.pool.lent_bytes + pool.pool.cached_bytes, pool.Carved());
  std::set<uint8_t *> distinct;
  for (const auto &h : held) { EXPECT_TRUE(distinct.insert(h.block).second); }
}

TEST(GradedBlockPool, WhatAnArenaCannotHoldAnyMoreGoesOntoTheLists) {
  Pool pool(4, 12, 1); // arenas of one 4 KiB block
  uint8_t *small = pool.Alloc(16);
  // 4080 bytes left, too few for 4096: cut into 2048 + 1024 + ... + 16, then a fresh arena
  pool.Alloc(4096);
  EXPECT_EQ(pool.Chain().arena_count, 2u);
  EXPECT_EQ(pool.pool.cached_bytes, 4080u);
  for (uint8_t log2 = 4; log2 <= 11; ++log2) {
    EXPECT_NE(pool.pool.free_head[log2 - 4], nullptr) << int(log2);
  }
  EXPECT_EQ(pool.pool.free_head[12 - 4], nullptr);
  // they serve the next requests of their grades, all in the first arena, right after @p small
  EXPECT_EQ(pool.Alloc(2048), small + 16);
  EXPECT_EQ(pool.Alloc(16), small + 4080);
  EXPECT_EQ(pool.Chain().arena_count, 2u);
  EXPECT_EQ(pool.pool.lent_bytes + pool.pool.cached_bytes, pool.Carved());
}

TEST(GradedBlockPool, ByExponentIsTheSameListAsBySize) {
  Pool pool(5, 12); // smallest grade 32 bytes
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_graded_block_pool_alloc_log2(&pool.pool, &block, 6), ARNM_SUCCESS);
  EXPECT_EQ(pool.pool.lent_bytes, 64u);
  // given back by exponent, taken again by size: one list
  EXPECT_EQ(arnm_graded_block_pool_free_log2(&pool.pool, block, 6), ARNM_SUCCESS);
  EXPECT_EQ(pool.Alloc(50), block);
  pool.Free(block, 64);
  // below the smallest grade: a block of the smallest, on the smallest list
  uint8_t *tiny = nullptr;
  ASSERT_EQ(arnm_graded_block_pool_alloc_log2(&pool.pool, &tiny, 3), ARNM_SUCCESS);
  EXPECT_EQ(pool.pool.lent_bytes, 32u);
  EXPECT_EQ(arnm_graded_block_pool_free_log2(&pool.pool, tiny, 3), ARNM_SUCCESS);
  EXPECT_EQ(pool.Alloc(32), tiny);
  pool.Free(tiny, 32);

  uint8_t *const sentinel = reinterpret_cast<uint8_t *>(uintptr_t{0x1000});
  block = sentinel;
  EXPECT_EQ(
      arnm_graded_block_pool_alloc_log2(&pool.pool, &block, 13), ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  EXPECT_EQ(block, sentinel);
  EXPECT_EQ(
      arnm_graded_block_pool_free_log2(&pool.pool, tiny, 13), ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  EXPECT_EQ(arnm_graded_block_pool_free_log2(&pool.pool, tiny, 5), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(arnm_graded_block_pool_free_log2(&pool.pool, nullptr, 5), ARNM_SUCCESS);

  // a released pool keeps its grades but nothing on the lists, so the fresh path answers
  arnm_graded_block_pool released{};
  arnm_graded_block_pool_options options{};
  ASSERT_EQ(arnm_graded_block_pool_init(&released, &options, nullptr), ARNM_SUCCESS);
  arnm_graded_block_pool_release(&released, nullptr);
  EXPECT_EQ(arnm_graded_block_pool_alloc_log2(&released, &block, 4), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(block, sentinel);
}

// ---------------------------------------------------------------------------
// refusals
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, WhatNoGradeHoldsIsRefusedAndChangesNothing) {
  Pool pool(4, 12);
  uint8_t *const sentinel = reinterpret_cast<uint8_t *>(uintptr_t{0x1000});
  uint8_t *block = sentinel;
  EXPECT_EQ(
      arnm_graded_block_pool_alloc(&pool.pool, &block, 4097), ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  EXPECT_EQ(arnm_graded_block_pool_alloc(&pool.pool, &block, 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(
      arnm_graded_block_pool_alloc(&pool.pool, &block, UINT32_MAX), ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(block, sentinel);
  EXPECT_EQ(arnm_graded_block_pool_alloc(nullptr, &block, 16), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_block_pool_alloc(&pool.pool, nullptr, 16), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(pool.pool.lent_bytes, 0u);
  EXPECT_EQ(pool.Chain().arena_count, 0u);

  // above 2^31 the grade would need a 33rd bit; still refused, not wrapped around
  Pool widest(3, 31);
  EXPECT_EQ(
      arnm_graded_block_pool_alloc(&widest.pool, &block, 0x80000001u),
      ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  EXPECT_EQ(block, sentinel);
}

TEST(GradedBlockPool, MisuseTheCountersCanSeeIsRefused) {
  Pool pool(4, 12);
  uint8_t *block = pool.Alloc(64);

  // more coming back than is out
  EXPECT_EQ(arnm_graded_block_pool_free(&pool.pool, block, 128), ARNM_ERROR_INVALID_STATE);
  pool.Free(block, 64);
  EXPECT_EQ(arnm_graded_block_pool_free(&pool.pool, block, 64), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(pool.pool.cached_bytes, 64u);

  EXPECT_EQ(arnm_graded_block_pool_free(&pool.pool, block, 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_graded_block_pool_free(&pool.pool, block, 4097), ARNM_ERROR_RESOURCE_SIZE_EXCEED);
  EXPECT_EQ(arnm_graded_block_pool_free(nullptr, block, 64), ARNM_ERROR_NULL_POINTER);
  // nothing handed back is nothing to do, as free(NULL) is
  EXPECT_EQ(arnm_graded_block_pool_free(&pool.pool, nullptr, 64), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_block_pool_free(nullptr, nullptr, 64), ARNM_SUCCESS);
}

TEST(GradedBlockPool, AFailingChainLeavesTheBufferAlone) {
  // one arena of the largest grade, more than the host is allowed to give
  if (!ArnmTestAllocationMustFail(uint64_t{1} << 31)) {
    GTEST_SKIP() << "no address space cap, a 2 GiB malloc cannot be promised to fail";
  }
  Pool pool(4, 31, 1);
  uint8_t *const sentinel = reinterpret_cast<uint8_t *>(uintptr_t{0x1000});
  uint8_t *block = sentinel;
  EXPECT_EQ(
      arnm_graded_block_pool_alloc(&pool.pool, &block, 0x80000000u), ARNM_ERROR_OUT_OF_MEMORY
  );
  EXPECT_EQ(block, sentinel);
  EXPECT_EQ(pool.pool.lent_bytes, 0u);
}

// ---------------------------------------------------------------------------
// realloc
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, ReallocMovesEvenWithinItsGrade) {
  Pool pool(4, 12);
  uint8_t *block = pool.Alloc(20);
  std::memcpy(block, "0123456789abcdefghi", 20);
  uint8_t *const old = block;

  ASSERT_EQ(arnm_graded_block_pool_realloc(&pool.pool, &block, 20, 30), ARNM_SUCCESS);
  EXPECT_NE(block, old);
  EXPECT_EQ(std::memcmp(block, "0123456789abcdefghi", 20), 0);
  EXPECT_EQ(pool.pool.lent_bytes, 32u);
  EXPECT_EQ(pool.pool.cached_bytes, 32u);
  // the old block went onto its list
  EXPECT_EQ(pool.Alloc(32), old);
}

TEST(GradedBlockPool, ReallocCarriesTheContentsAcrossGrades) {
  Pool pool(4, 12);
  uint8_t *block = pool.Alloc(16);
  for (uint8_t i = 0; i < 16; ++i) { block[i] = i; }

  ASSERT_EQ(arnm_graded_block_pool_realloc(&pool.pool, &block, 16, 1000), ARNM_SUCCESS);
  for (uint8_t i = 0; i < 16; ++i) { EXPECT_EQ(block[i], i); }
  EXPECT_EQ(pool.pool.lent_bytes, 1024u);
  EXPECT_EQ(pool.pool.cached_bytes, 16u);

  // shrinking copies only what the new size holds
  for (uint32_t i = 0; i < 1000; ++i) { block[i] = uint8_t(i); }
  ASSERT_EQ(arnm_graded_block_pool_realloc(&pool.pool, &block, 1000, 40), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 40; ++i) { EXPECT_EQ(block[i], uint8_t(i)); }
  EXPECT_EQ(pool.pool.lent_bytes, 64u);
  EXPECT_EQ(pool.pool.cached_bytes, 16u + 1024u);
}

TEST(GradedBlockPool, ReallocFromNothingAllocates) {
  Pool pool(4, 12);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_graded_block_pool_realloc(&pool.pool, &block, 0, 100), ARNM_SUCCESS);
  EXPECT_NE(block, nullptr);
  EXPECT_EQ(pool.pool.lent_bytes, 128u);
  EXPECT_EQ(pool.pool.cached_bytes, 0u);
}

TEST(GradedBlockPool, ReallocRefusalLeavesTheBlockWhereItWas) {
  Pool pool(4, 12);
  uint8_t *block = pool.Alloc(64);
  uint8_t *const old = block;
  EXPECT_EQ(
      arnm_graded_block_pool_realloc(&pool.pool, &block, 64, 4097), ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  // a size of 0 is refused, not a free
  EXPECT_EQ(arnm_graded_block_pool_realloc(&pool.pool, &block, 64, 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_graded_block_pool_realloc(&pool.pool, nullptr, 64, 128), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(block, old);
  EXPECT_EQ(pool.pool.lent_bytes, 64u);
  EXPECT_EQ(pool.pool.cached_bytes, 0u);
}

TEST(GradedBlockPool, ReallocOfABlockThePoolCannotTakeBackStillMoves) {
  Pool pool(4, 12);
  uint8_t *block = pool.Alloc(16);
  uint8_t *const old = block;
  // told a grade larger than anything out: the move happens, the old block is not taken back
  EXPECT_EQ(
      arnm_graded_block_pool_realloc(&pool.pool, &block, 1024, 16),
      ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  EXPECT_NE(block, old);
  EXPECT_EQ(pool.pool.lent_bytes, 32u);
  EXPECT_EQ(pool.pool.cached_bytes, 0u);
}

// ---------------------------------------------------------------------------
// reset, release, destroy
// ---------------------------------------------------------------------------

TEST(GradedBlockPool, ResetKeepsTheArenasAndStartsOver) {
  Pool pool(4, 12, 2);
  uint8_t *first = pool.Alloc(64);
  for (int i = 0; i < 300; ++i) { pool.Alloc(64); }
  pool.Free(first, 64);
  const auto before = pool.Chain();
  EXPECT_GT(before.arena_count, 1u);

  arnm_graded_block_pool_reset(&pool.pool);
  EXPECT_EQ(pool.pool.lent_bytes, 0u);
  EXPECT_EQ(pool.pool.cached_bytes, 0u);
  for (uint8_t *head : pool.pool.free_head) { EXPECT_EQ(head, nullptr); }
  const auto after = pool.Chain();
  EXPECT_EQ(after.arena_count, before.arena_count);
  EXPECT_EQ(after.reserved, before.reserved);
  EXPECT_EQ(after.used, 0u);

  // the ground is used again from the front
  EXPECT_EQ(pool.Alloc(64), first);
  arnm_graded_block_pool_reset(nullptr);
}

TEST(GradedBlockPool, ReleaseLeavesNothingAndAnswersNotInitialized) {
  arnm_graded_block_pool pool;
  arnm_graded_block_pool_options options{};
  ASSERT_EQ(arnm_graded_block_pool_init(&pool, &options, nullptr), ARNM_SUCCESS);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_graded_block_pool_alloc(&pool, &block, 64), ARNM_SUCCESS);

  arnm_graded_block_pool_release(&pool, nullptr);
  EXPECT_EQ(pool.source, nullptr);
  EXPECT_EQ(pool.lent_bytes, 0u);
  EXPECT_EQ(pool.cached_bytes, 0u);
  uint8_t *after = nullptr;
  EXPECT_EQ(arnm_graded_block_pool_alloc(&pool, &after, 64), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_graded_block_pool_free(&pool, block, 64), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(after, nullptr);

  // twice is harmless, and so is NULL
  arnm_graded_block_pool_release(&pool, nullptr);
  arnm_graded_block_pool_reset(&pool);
  arnm_graded_block_pool_release(nullptr, nullptr);

  // and it can start over
  ASSERT_EQ(arnm_graded_block_pool_init(&pool, &options, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_block_pool_alloc(&pool, &after, 64), ARNM_SUCCESS);
  arnm_graded_block_pool_release(&pool, nullptr);
}

TEST(GradedBlockPool, ReleaseOverAnArenaGivesTheBookkeepingBack) {
  Arena source(4096);
  arnm_graded_block_pool pool;
  arnm_graded_block_pool_options options{};
  ASSERT_EQ(arnm_graded_block_pool_init(&pool, &options, &source.arena), ARNM_SUCCESS);
  uint8_t *block = nullptr;
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(arnm_graded_block_pool_alloc(&pool, &block, 1u << 20), ARNM_SUCCESS);
  }
  EXPECT_LT(source.Remaining(), 4096u);
  arnm_graded_block_pool_release(&pool, &source.arena);
  EXPECT_EQ(source.Remaining(), 4096u);
}

TEST(GradedBlockPool, CreateAndDestroyOverAnArena) {
  Arena source(4096);
  arnm_graded_block_pool_options options{};
  arnm_graded_block_pool *pool = arnm_graded_block_pool_create(&options, &source.arena);
  ASSERT_NE(pool, nullptr);
  uint8_t *block = nullptr;
  ASSERT_EQ(arnm_graded_block_pool_alloc(pool, &block, 100), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_block_pool_destroy(pool, &source.arena), ARNM_SUCCESS);
  EXPECT_EQ(source.Remaining(), 4096u);

  EXPECT_EQ(arnm_graded_block_pool_destroy(nullptr, nullptr), ARNM_SUCCESS);

  // no room for the struct, and room for the struct but not the chain
  Arena tiny(16);
  EXPECT_EQ(arnm_graded_block_pool_create(&options, &tiny.arena), nullptr);
  Arena small(uint32_t(sizeof(arnm_graded_block_pool)) + 8u);
  EXPECT_EQ(arnm_graded_block_pool_create(&options, &small.arena), nullptr);
  EXPECT_EQ(small.Remaining(), sizeof(arnm_graded_block_pool) + 8u);
}

TEST(GradedBlockPool, CreateAndDestroyOverTheHost) {
  arnm_graded_block_pool_options options{};
  arnm_graded_block_pool *pool = arnm_graded_block_pool_create(&options, nullptr);
  ASSERT_NE(pool, nullptr);
  uint8_t *block = nullptr;
  for (uint32_t size = 1; size <= (1u << 20); size *= 3) {
    ASSERT_EQ(arnm_graded_block_pool_alloc(pool, &block, size), ARNM_SUCCESS);
  }
  EXPECT_EQ(arnm_graded_block_pool_destroy(pool, nullptr), ARNM_SUCCESS);
}
