#include "arnm/arena.h"
#include "arnm/dynamic_arena_pool.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include "memory_intern.h"
#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

// Small arenas and a shallow shelf, so every test crosses the line where the stock is full
// rather than describing it. The capacity is a multiple of 8 already, so the rounding is not
// silently doing the work.
namespace {

constexpr uint32_t kArenaCapacity = 128;
constexpr uint32_t kSpareLimit = 2;

/** What one arena costs the host: its descriptor rides in front of its buffer. */
constexpr uint64_t ArenaBlockBytes(uint32_t capacity) {
  return sizeof(arnm) + capacity;
}

/** Every pointer the pool hands out is 8 byte aligned, arenas and payload alike. */
void ExpectAligned(const void *p) {
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

/** Take @p count arenas and hand back the addresses. */
std::vector<arnm *> Take(arnm_dynamic_arena_pool *pool, uint32_t count) {
  std::vector<arnm *> taken;
  for (uint32_t i = 0; i < count; ++i) {
    arnm *arena = nullptr;
    EXPECT_EQ(arnm_dynamic_arena_pool_alloc(pool, &arena), ARNM_SUCCESS) << "arena " << i;
    if (arena) { taken.push_back(arena); }
  }
  return taken;
}

void GiveBack(arnm_dynamic_arena_pool *pool, const std::vector<arnm *> &arenas) {
  for (arnm *arena : arenas) { EXPECT_EQ(arnm_dynamic_arena_pool_free(pool, arena), ARNM_SUCCESS); }
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(DynamicArenaPool, InitSettlesTheMeasuresAndAsksTheHostForNothing) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  EXPECT_EQ(pool.arena_capacity, kArenaCapacity);
  EXPECT_EQ(pool.spare_limit, kSpareLimit);
  EXPECT_EQ(pool.acquired_count, 0u);
  EXPECT_EQ(pool.spare_count, 0u);
  EXPECT_EQ(pool.free_head, nullptr);
  // nothing was allocated, so nothing is held
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), 0u);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), 0u);

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, InitRejectsBadArguments) {
  arnm_dynamic_arena_pool pool{};
  EXPECT_EQ(
      arnm_dynamic_arena_pool_init(nullptr, kArenaCapacity, kSpareLimit), ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(arnm_dynamic_arena_pool_init(&pool, 0, kSpareLimit), ARNM_ERROR_INVALID_PARAM);
  // the largest request the library takes still needs a descriptor in front of it
  EXPECT_EQ(
      arnm_dynamic_arena_pool_init(&pool, ARNM_MAX_ALLOC_SIZE, kSpareLimit),
      ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  // the refusals left the descriptor exactly as it was
  EXPECT_EQ(pool.arena_capacity, 0u);
}

TEST(DynamicArenaPool, CapacityRoundsUpToEight) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, 100, kSpareLimit), ARNM_SUCCESS);
  EXPECT_EQ(pool.arena_capacity, 104u); // and that is what the arenas really get

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(arena)->capacity, 104u);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 104, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_alloc(&buffer, 1, arena), ARNM_ERROR_OUT_OF_MEMORY);

  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, CreateAndDestroy) {
  arnm_dynamic_arena_pool *pool =
      arnm_dynamic_arena_pool_create(kArenaCapacity, kSpareLimit, nullptr);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(pool->arena_capacity, kArenaCapacity);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(pool, &arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_free(pool, arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_dynamic_arena_pool_destroy(pool, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_destroy(nullptr, nullptr), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, CreateRefusesArgumentsInitWouldRefuse) {
  EXPECT_EQ(arnm_dynamic_arena_pool_create(0, kSpareLimit, nullptr), nullptr);
  EXPECT_EQ(arnm_dynamic_arena_pool_create(ARNM_MAX_ALLOC_SIZE, kSpareLimit, nullptr), nullptr);
}

TEST(DynamicArenaPool, DescriptorCanLiveInAnArena) {
  alignas(8) uint8_t blob[256];
  arnm store{};
  ASSERT_EQ(arnm_init_arena_borrow(&store, blob, sizeof(blob)), ARNM_SUCCESS);

  arnm_dynamic_arena_pool *pool =
      arnm_dynamic_arena_pool_create(kArenaCapacity, kSpareLimit, &store);
  ASSERT_NE(pool, nullptr);
  // the descriptor came from the arena; the arenas it lends did not
  EXPECT_GT(ARNM_INTERN(&store)->last_index, 0u);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(pool, &arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_free(pool, arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_dynamic_arena_pool_destroy(pool, &store), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&store)->last_index, 0u); // it was the tail, so it came back
  arnm_release(&store);
}

// ---------------------------------------------------------------------------
// lending, and the shelf behind it
// ---------------------------------------------------------------------------

TEST(DynamicArenaPool, AnEmptyShelfMakesTheArenaItWasAskedFor) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_SUCCESS);
  ASSERT_NE(arena, nullptr);
  ExpectAligned(arena);

  EXPECT_EQ(pool.acquired_count, 1u);
  EXPECT_EQ(pool.spare_count, 0u); // it was made, not taken from stock
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), ArenaBlockBytes(kArenaCapacity));

  // an ordinary arena, and empty
  EXPECT_TRUE(arnm_is_arena(arena));
  EXPECT_EQ(arnm_arena_remaining(arena), kArenaCapacity);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, arena), ARNM_SUCCESS);
  ExpectAligned(buffer);

  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, GrowsPastTheStockAndDrainsBackToIt) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  // a burst of five against a shelf that keeps two: every one of them is served
  std::vector<arnm *> taken = Take(&pool, 5);
  ASSERT_EQ(taken.size(), 5u);
  EXPECT_EQ(pool.acquired_count, 5u);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), 5 * ArenaBlockBytes(kArenaCapacity));

  GiveBack(&pool, taken);
  // the burst drained away; what is left is the resting size and nothing more
  EXPECT_EQ(pool.acquired_count, 0u);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), kSpareLimit);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), kSpareLimit * ArenaBlockBytes(kArenaCapacity));

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ASpareLimitOfZeroKeepsNothing) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, 0), ARNM_SUCCESS);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), 0u);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), 0u); // straight back to the host
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, AnArenaFromTheShelfArrivesEmpty) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  arnm *first = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &first), ARNM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 96, first), ARNM_SUCCESS);
  ASSERT_LT(arnm_arena_remaining(first), kArenaCapacity);
  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, first), ARNM_SUCCESS);

  arnm *second = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &second), ARNM_SUCCESS);
  EXPECT_EQ(second, first); // the shelf hands back what it last took in
  EXPECT_EQ(arnm_arena_remaining(second), kArenaCapacity);
  EXPECT_EQ(pool.spare_count, 0u);

  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, second), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, TurningOverManyTimesHoldsTheFootprintStill) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, 1), ARNM_SUCCESS);

  for (int round = 0; round < 10000; ++round) {
    arnm *arena = nullptr;
    ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_SUCCESS) << "round " << round;
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, kArenaCapacity, arena), ARNM_SUCCESS);
    ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, arena), ARNM_SUCCESS);
    // one arena served all of it, and the host was asked exactly once
    ASSERT_EQ(arnm_dynamic_arena_pool_reserved(&pool), ArenaBlockBytes(kArenaCapacity));
  }

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ArenasLentAtOnceDoNotOverlap) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  std::vector<arnm *> taken = Take(&pool, 4);
  ASSERT_EQ(taken.size(), 4u);

  // each arena is written full with its own byte, then every one of them is read back
  std::vector<uint8_t *> buffers;
  for (size_t i = 0; i < taken.size(); ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, kArenaCapacity, taken[i]), ARNM_SUCCESS);
    memset(buffer, static_cast<int>(i) + 1, kArenaCapacity);
    buffers.push_back(buffer);
  }
  for (size_t i = 0; i < buffers.size(); ++i) {
    for (uint32_t b = 0; b < kArenaCapacity; ++b) {
      ASSERT_EQ(buffers[i][b], static_cast<uint8_t>(i + 1)) << "arena " << i << " byte " << b;
    }
  }

  GiveBack(&pool, taken);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, AllocRefusesOnceTheCounterIsFull) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  // reaching UINT32_MAX arenas for real would take 32 GB, so the counter is put there instead
  pool.acquired_count = UINT32_MAX;
  arnm *arena = nullptr;
  EXPECT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(arena, nullptr); // a refusal leaves the output untouched

  pool.acquired_count = 0;
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// filling the shelf up front
// ---------------------------------------------------------------------------

TEST(DynamicArenaPool, ReserveFillsTheStockAndStopsAtIt) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  ASSERT_EQ(arnm_dynamic_arena_pool_reserve(&pool, kSpareLimit), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), kSpareLimit);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), kSpareLimit * ArenaBlockBytes(kArenaCapacity));

  // a stock already that deep is left alone, and a shallower request changes nothing
  ASSERT_EQ(arnm_dynamic_arena_pool_reserve(&pool, kSpareLimit), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_reserve(&pool, 1), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), kSpareLimit);

  // and the arenas it made are the ones handed out, with no further asking
  std::vector<arnm *> taken = Take(&pool, kSpareLimit);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), 0u);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), kSpareLimit * ArenaBlockBytes(kArenaCapacity));
  for (arnm *arena : taken) { EXPECT_EQ(arnm_arena_remaining(arena), kArenaCapacity); }

  GiveBack(&pool, taken);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ReserveRejectsWhatTheShelfCouldNotHold) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  EXPECT_EQ(arnm_dynamic_arena_pool_reserve(&pool, kSpareLimit + 1), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), 0u); // and nothing was made on the way
  EXPECT_EQ(arnm_dynamic_arena_pool_reserve(nullptr, 1), ARNM_ERROR_NULL_POINTER);

  arnm_dynamic_arena_pool empty{};
  EXPECT_EQ(arnm_dynamic_arena_pool_reserve(&empty, 1), ARNM_ERROR_NOT_INITIALIZED);

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// refusals
// ---------------------------------------------------------------------------

TEST(DynamicArenaPool, CallsOnAnEmptyPoolSayItWasNeverInitialized) {
  arnm_dynamic_arena_pool pool{};
  arnm *arena = nullptr;
  EXPECT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arena, nullptr);

  arnm stranger{};
  EXPECT_EQ(arnm_dynamic_arena_pool_free(&pool, &stranger), ARNM_ERROR_NOT_INITIALIZED);
  // release on a pool that holds nothing is not a failure
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, NullArgumentsAreRefusedEverywhere) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);
  arnm *arena = nullptr;

  EXPECT_EQ(arnm_dynamic_arena_pool_alloc(nullptr, &arena), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_dynamic_arena_pool_alloc(&pool, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_dynamic_arena_pool_free(nullptr, arena), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_dynamic_arena_pool_free(&pool, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(nullptr), 0u);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(nullptr), 0u);

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, FreeRefusesWhenNothingIsOut) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_free(&pool, arena), ARNM_SUCCESS);

  // the arena is on the shelf; returning it again would knot the free list into a ring
  arnm *spare = pool.free_head;
  EXPECT_EQ(arnm_dynamic_arena_pool_free(&pool, spare), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(arnm_dynamic_arena_pool_available(&pool), 1u); // and the shelf is untouched

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ReleaseRefusesWhileAnArenaIsOut) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);

  std::vector<arnm *> taken = Take(&pool, 3);
  ASSERT_EQ(taken.size(), 3u);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_ERROR_RESOURCE_IN_USE);
  // nothing was changed, so the arenas in hand are still good
  EXPECT_EQ(pool.acquired_count, 3u);
  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 64, taken[0]), ARNM_SUCCESS);

  GiveBack(&pool, taken);
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, DestroyRefusesWhileAnArenaIsOutAndKeepsTheDescriptor) {
  arnm_dynamic_arena_pool *pool =
      arnm_dynamic_arena_pool_create(kArenaCapacity, kSpareLimit, nullptr);
  ASSERT_NE(pool, nullptr);

  arnm *arena = nullptr;
  ASSERT_EQ(arnm_dynamic_arena_pool_alloc(pool, &arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_destroy(pool, nullptr), ARNM_ERROR_RESOURCE_IN_USE);

  // the descriptor is still ours, which is the only way the arena can come home
  ASSERT_EQ(arnm_dynamic_arena_pool_free(pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_dynamic_arena_pool_destroy(pool, nullptr), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ReleaseLeavesTheEmptyState) {
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kArenaCapacity, kSpareLimit), ARNM_SUCCESS);
  ASSERT_EQ(arnm_dynamic_arena_pool_reserve(&pool, kSpareLimit), ARNM_SUCCESS);

  ASSERT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
  EXPECT_EQ(pool.arena_capacity, 0u);
  EXPECT_EQ(pool.spare_limit, 0u);
  EXPECT_EQ(pool.free_head, nullptr);
  EXPECT_EQ(arnm_dynamic_arena_pool_reserved(&pool), 0u);

  arnm *arena = nullptr;
  EXPECT_EQ(arnm_dynamic_arena_pool_alloc(&pool, &arena), ARNM_ERROR_NOT_INITIALIZED);
  // and a second release finds nothing to do
  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(DynamicArenaPool, ReleaseGivesTheStockBackAndNotJustForgetsIt) {
  // A leak is invisible from inside the process, so it is made loud instead: a megabyte an
  // arena, four of them a round, a thousand rounds. Released properly the pool never holds
  // more than four; leaked, the rounds add up to four gigabytes and the cap in memory_limit.h
  // refuses long before the last one. Without a cap this still passes -- see the note there.
  constexpr uint32_t kBigArena = 1024 * 1024;
  constexpr uint32_t kStock = 4;

  for (int round = 0; round < 1000; ++round) {
    arnm_dynamic_arena_pool pool;
    ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kBigArena, kStock), ARNM_SUCCESS);
    ASSERT_EQ(arnm_dynamic_arena_pool_reserve(&pool, kStock), ARNM_SUCCESS) << "round " << round;
    ASSERT_EQ(arnm_dynamic_arena_pool_reserved(&pool), kStock * ArenaBlockBytes(kBigArena));
    ASSERT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
  }
}

TEST(DynamicArenaPool, AnArenaTheShelfTurnsAwayGoesToTheHost) {
  // The same measure from the other side: the shelf keeps one, so every further return has to
  // reach the host or a thousand rounds of four leave four gigabytes behind.
  constexpr uint32_t kBigArena = 1024 * 1024;
  arnm_dynamic_arena_pool pool;
  ASSERT_EQ(arnm_dynamic_arena_pool_init(&pool, kBigArena, 1), ARNM_SUCCESS);

  for (int round = 0; round < 1000; ++round) {
    std::vector<arnm *> taken = Take(&pool, 4);
    ASSERT_EQ(taken.size(), 4u) << "round " << round;
    GiveBack(&pool, taken);
    ASSERT_EQ(arnm_dynamic_arena_pool_reserved(&pool), ArenaBlockBytes(kBigArena));
  }

  EXPECT_EQ(arnm_dynamic_arena_pool_release(&pool), ARNM_SUCCESS);
}
