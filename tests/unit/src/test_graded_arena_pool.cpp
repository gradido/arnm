#include "arnm/arena.h"
#include "arnm/bitmap.h"
#include "arnm/dynamic_arena_pool.h"
#include "arnm/graded_arena_pool.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include "memory_intern.h"
#include "memory_limit.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

// Four rungs an octave apart, the shape a real ladder tends to have. Every size is a multiple
// of 8 already, so the rounding is not silently doing the work.
namespace {

const uint32_t kSizes[] = {128, 256, 512, 1024};
constexpr uint16_t kGradeCount = 4;
constexpr uint32_t kSparePerGrade = 2;

/** What one arena of @p capacity costs the host: its descriptor rides in front of its buffer. */
constexpr uint64_t ArenaBlockBytes(uint32_t capacity) {
  return sizeof(arnm) + capacity;
}

/** What the ladder itself costs, before a single arena is made. */
constexpr uint64_t LadderBytes(uint16_t grades) {
  return sizeof(arnm_dynamic_arena_pool) * grades;
}

arnm *TakeOne(arnm_graded_arena_pool *pool, uint32_t size) {
  arnm *arena = nullptr;
  EXPECT_EQ(arnm_graded_arena_pool_alloc(pool, size, &arena), ARNM_SUCCESS) << "size " << size;
  return arena;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(GradedArenaPool, InitOpensOneGradePerSize) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  EXPECT_EQ(arnm_graded_arena_pool_grade_count(&pool), kGradeCount);
  for (uint16_t i = 0; i < kGradeCount; ++i) {
    const arnm_dynamic_arena_pool *grade = arnm_graded_arena_pool_grade_at(&pool, i);
    ASSERT_NE(grade, nullptr) << "grade " << i;
    EXPECT_EQ(grade->arena_capacity, kSizes[i]);
    EXPECT_EQ(grade->spare_limit, kSparePerGrade);
    EXPECT_EQ(grade->spare_count, 0u); // no arena is made before one is asked for
  }
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, kGradeCount), nullptr);
  // only the ladder itself is held so far
  EXPECT_EQ(arnm_graded_arena_pool_reserved(&pool), LadderBytes(kGradeCount));

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, InitRejectsBadArguments) {
  arnm_graded_arena_pool pool{};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(nullptr, kSizes, kGradeCount, kSparePerGrade),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, nullptr, kGradeCount, kSparePerGrade),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, kSizes, 0, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  const uint32_t with_zero[] = {128, 0, 512};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, with_zero, 3, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  const uint32_t descending[] = {512, 256};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, descending, 2, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  const uint32_t repeated[] = {128, 128};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, repeated, 2, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  // ascending, in range, and still not a ladder: 250 is on no rung and is not rounded onto one
  const uint32_t not_a_power_of_two[] = {128, 250, 512};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, not_a_power_of_two, 3, kSparePerGrade),
      ARNM_ERROR_INVALID_PARAM
  );

  const uint32_t too_large[] = {128, ARNM_MAX_ALLOC_SIZE};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, too_large, 2, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  // below the smallest arena there is, and above the largest power of two a uint32_t names
  const uint32_t too_small[] = {4, 128};
  EXPECT_EQ(
      arnm_graded_arena_pool_init(&pool, too_small, 2, kSparePerGrade), ARNM_ERROR_INVALID_PARAM
  );

  // there are only 29 powers of two in range, so a longer list cannot ascend strictly
  uint32_t every_rung[ARNM_GRADED_MAX_GRADE_COUNT + 1];
  for (uint32_t i = 0; i < ARNM_GRADED_MAX_GRADE_COUNT + 1; ++i) { every_rung[i] = 8u << i; }
  EXPECT_EQ(
      arnm_graded_arena_pool_init(
          &pool, every_rung, ARNM_GRADED_MAX_GRADE_COUNT + 1, kSparePerGrade
      ),
      ARNM_ERROR_INVALID_PARAM
  );

  // every refusal happened before the host was asked, so the descriptor is as it was
  EXPECT_EQ(pool.grades, nullptr);
  EXPECT_EQ(pool.grade_count, 0u);
}

TEST(GradedArenaPool, NothingIsRoundedOntoARung) {
  // 100 would be 128 if this rounded like the rest of arnm. It refuses instead, so a ladder is
  // never quietly holding a size nobody wrote down.
  const uint32_t sizes[] = {100, 200};
  arnm_graded_arena_pool pool{};
  EXPECT_EQ(arnm_graded_arena_pool_init(&pool, sizes, 2, kSparePerGrade), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(pool.grades, nullptr);
}

TEST(GradedArenaPool, TheMaskNamesExactlyTheSizesTheLadderHas) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  // bit e set means a grade of 1 << e bytes: 128, 256, 512, 1024 are exponents 7 to 10
  EXPECT_EQ(pool.grade_bits, (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10));
  EXPECT_EQ(arnm_popcount(pool.grade_bits), pool.grade_count);
  // and the bits stand where the array does: counting below a bit gives that grade's slot
  for (uint16_t i = 0; i < kGradeCount; ++i) {
    const uint32_t exponent = (uint32_t)arnm_ctz(kSizes[i]);
    EXPECT_EQ(arnm_popcount(pool.grade_bits & ((1u << exponent) - 1u)), i) << "grade " << i;
  }

  ASSERT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
  EXPECT_EQ(pool.grade_bits, 0u);
}

TEST(GradedArenaPool, TheLadderNeedNotBeARun) {
  // gaps cost nothing: an absent grade is an unset bit, not an empty slot
  const uint32_t sparse[] = {8, 64, 4096};
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, sparse, 3, kSparePerGrade), ARNM_SUCCESS);

  EXPECT_EQ(pool.grade_bits, (1u << 3) | (1u << 6) | (1u << 12));
  EXPECT_EQ(arnm_graded_arena_pool_grade_count(&pool), 3u);

  // a request landing in a gap climbs to the next rung that exists, not to the one that would
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 1), 8u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 8), 8u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 9), 64u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 16), 64u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 64), 64u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 65), 4096u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 4096), 4096u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 4097), 0u);

  // and the arena a gap request gets really is the rung above it
  arnm *arena = TakeOne(&pool, 9);
  ASSERT_NE(arena, nullptr);
  EXPECT_EQ(arnm_arena_remaining(arena), 64u);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 1)->acquired_count, 1u);
  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 1)->spare_count, 1u);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, TheEndsOfTheRangeAreLadderSizesLikeAnyOther) {
  // 8 B is the smallest arena there is and 2 GiB the largest power of two a uint32_t names.
  // Nothing is allocated here -- init opens grades, it does not fill them.
  const uint32_t ends[] = {ARNM_GRADED_MIN_SIZE, ARNM_GRADED_MAX_SIZE};
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, ends, 2, kSparePerGrade), ARNM_SUCCESS);

  EXPECT_EQ(pool.grade_bits, (1u << 3) | (1u << 31));
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 9), ARNM_GRADED_MAX_SIZE);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, ARNM_GRADED_MAX_SIZE), ARNM_GRADED_MAX_SIZE);
  // one byte past the top rung, and the largest request the library takes at all
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, ARNM_GRADED_MAX_SIZE + 1u), 0u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, ARNM_MAX_ALLOC_SIZE), 0u);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, CreateAndDestroy) {
  arnm_graded_arena_pool *pool =
      arnm_graded_arena_pool_create(kSizes, kGradeCount, kSparePerGrade, nullptr);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_grade_count(pool), kGradeCount);

  arnm *arena = TakeOne(pool, 300);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(arnm_graded_arena_pool_free(pool, arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_graded_arena_pool_destroy(pool, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_destroy(nullptr, nullptr), ARNM_SUCCESS);
}

TEST(GradedArenaPool, CreateRefusesArgumentsInitWouldRefuse) {
  const uint32_t descending[] = {512, 256};
  EXPECT_EQ(arnm_graded_arena_pool_create(kSizes, 0, kSparePerGrade, nullptr), nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_create(nullptr, kGradeCount, kSparePerGrade, nullptr), nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_create(descending, 2, kSparePerGrade, nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// climbing to the right rung
// ---------------------------------------------------------------------------

TEST(GradedArenaPool, ARequestIsServedByTheNextGradeUp) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  struct Case {
    uint32_t requested;
    uint32_t served;
  };
  const Case cases[] = {{1, 128},   {128, 128}, {129, 256},  {256, 256},
                        {300, 512}, {512, 512}, {513, 1024}, {1024, 1024}};

  for (const Case &c : cases) {
    // the same answer whether it is asked for or taken
    EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, c.requested), c.served)
        << "requested " << c.requested;
    arnm *arena = TakeOne(&pool, c.requested);
    ASSERT_NE(arena, nullptr);
    EXPECT_EQ(arnm_arena_remaining(arena), c.served) << "requested " << c.requested;
    // and it really holds what it promised
    uint8_t *buffer = nullptr;
    EXPECT_EQ(arnm_alloc(&buffer, c.requested, arena), ARNM_SUCCESS);
    EXPECT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  }

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, PastTheLastRungIsRefusedAsASizeAndNotAsSupply) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  arnm *arena = nullptr;
  EXPECT_EQ(arnm_graded_arena_pool_alloc(&pool, 1025, &arena), ARNM_ERROR_RESOURCE_SIZE_EXCEED);
  EXPECT_EQ(arena, nullptr); // a refusal leaves the output untouched
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 1025), 0u);
  EXPECT_EQ(
      arnm_graded_arena_pool_alloc(&pool, ARNM_MAX_ALLOC_SIZE, &arena),
      ARNM_ERROR_RESOURCE_SIZE_EXCEED
  );
  // a size of 0 is the caller's mistake, not the ladder's ceiling
  EXPECT_EQ(arnm_graded_arena_pool_alloc(&pool, 0, &arena), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 0), 0u);
  // nothing was made on the way
  EXPECT_EQ(arnm_graded_arena_pool_reserved(&pool), LadderBytes(kGradeCount));

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, OnlyTheGradeThatServedGrows) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  arnm *arena = TakeOne(&pool, 300); // the 512 rung
  ASSERT_NE(arena, nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 2)->acquired_count, 1u);
  for (uint16_t i = 0; i < kGradeCount; ++i) {
    if (i == 2) { continue; }
    EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, i)->acquired_count, 0u) << "grade " << i;
    EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, i)->spare_count, 0u) << "grade " << i;
  }
  EXPECT_EQ(
      arnm_graded_arena_pool_reserved(&pool), LadderBytes(kGradeCount) + ArenaBlockBytes(512)
  );

  // and it comes home to the same rung, found from its own capacity
  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 2)->acquired_count, 0u);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 2)->spare_count, 1u);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, EachGradeKeepsItsOwnStock) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  // three from one rung against a shelf that keeps two
  std::vector<arnm *> taken;
  for (int i = 0; i < 3; ++i) { taken.push_back(TakeOne(&pool, 200)); }
  for (arnm *arena : taken) { ASSERT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS); }

  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 1)->spare_count, kSparePerGrade);
  EXPECT_EQ(
      arnm_graded_arena_pool_reserved(&pool),
      LadderBytes(kGradeCount) + kSparePerGrade * ArenaBlockBytes(256)
  );

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, AGradeCanBeStockedUpFront) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  arnm_dynamic_arena_pool *smallest = arnm_graded_arena_pool_grade_at(&pool, 0);
  ASSERT_NE(smallest, nullptr);
  ASSERT_EQ(arnm_dynamic_arena_pool_reserve(smallest, kSparePerGrade), ARNM_SUCCESS);

  const uint64_t reserved = arnm_graded_arena_pool_reserved(&pool);
  EXPECT_EQ(reserved, LadderBytes(kGradeCount) + kSparePerGrade * ArenaBlockBytes(128));

  // the first callers are served from that stock and the host is not asked again
  arnm *first = TakeOne(&pool, 8);
  arnm *second = TakeOne(&pool, 8);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(arnm_graded_arena_pool_reserved(&pool), reserved);

  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, first), ARNM_SUCCESS);
  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, second), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// refusals
// ---------------------------------------------------------------------------

TEST(GradedArenaPool, FreeRefusesWhatNoGradeCouldHaveLent) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  // a capacity that is not a power of two at all
  alignas(8) uint8_t blob[200];
  arnm stranger{};
  ASSERT_EQ(arnm_init_arena_borrow(&stranger, blob, sizeof(blob)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &stranger), ARNM_ERROR_INVALID_PARAM);

  // and one that is a power of two, but a rung this ladder does not have. The mask has to be
  // consulted and not just the exponent -- 2048 is a perfectly good size, only not here.
  alignas(8) uint8_t off_ladder[2048];
  arnm above_the_top{};
  ASSERT_EQ(arnm_init_arena_borrow(&above_the_top, off_ladder, sizeof(off_ladder)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &above_the_top), ARNM_ERROR_INVALID_PARAM);

  alignas(8) uint8_t below_the_bottom[64];
  arnm under{};
  ASSERT_EQ(
      arnm_init_arena_borrow(&under, below_the_bottom, sizeof(below_the_bottom)), ARNM_SUCCESS
  );
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &under), ARNM_ERROR_INVALID_PARAM);

  // a handle that is no single arena at all
  arnm host_mode{};
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &host_mode), ARNM_ERROR_INVALID_PARAM);
  arnm_multi_arena_options options{};
  arnm *chain = arnm_create_multi_arena(&options, nullptr);
  ASSERT_NE(chain, nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, chain), ARNM_ERROR_INVALID_PARAM);
  arnm_destroy(chain, nullptr);

  // and one whose capacity is a rung, returned to a grade that has nothing out
  alignas(8) uint8_t sized_like_a_grade[128];
  arnm impostor{};
  ASSERT_EQ(
      arnm_init_arena_borrow(&impostor, sized_like_a_grade, sizeof(sized_like_a_grade)),
      ARNM_SUCCESS
  );
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &impostor), ARNM_ERROR_INVALID_STATE);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, CallsOnAnEmptyPoolSayItWasNeverInitialized) {
  arnm_graded_arena_pool pool{};
  arnm *arena = nullptr;
  EXPECT_EQ(arnm_graded_arena_pool_alloc(&pool, 128, &arena), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arena, nullptr);

  arnm stranger{};
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, &stranger), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_graded_arena_pool_grade_count(&pool), 0u);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(&pool, 128), 0u);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 0), nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, NullArgumentsAreRefusedEverywhere) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);
  arnm *arena = nullptr;

  EXPECT_EQ(arnm_graded_arena_pool_alloc(nullptr, 128, &arena), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_arena_pool_alloc(&pool, 128, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_arena_pool_free(nullptr, arena), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_arena_pool_free(&pool, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_arena_pool_release(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_graded_arena_pool_grade_count(nullptr), 0u);
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(nullptr, 0), nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_capacity_for(nullptr, 128), 0u);
  EXPECT_EQ(arnm_graded_arena_pool_reserved(nullptr), 0u);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, ReleaseLeavesEveryGradeStandingWhileOneIsInUse) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);

  // stock on one rung, an arena out on another: the ladder must come down all at once or not
  ASSERT_EQ(
      arnm_dynamic_arena_pool_reserve(arnm_graded_arena_pool_grade_at(&pool, 0), 1), ARNM_SUCCESS
  );
  arnm *arena = TakeOne(&pool, 1000);
  ASSERT_NE(arena, nullptr);

  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_ERROR_RESOURCE_IN_USE);
  // nothing was changed: the stock is still there and the arena in hand still works
  EXPECT_EQ(arnm_graded_arena_pool_grade_at(&pool, 0)->spare_count, 1u);
  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 1000, arena), ARNM_SUCCESS);

  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, DestroyRefusesWhileAnArenaIsOutAndKeepsTheDescriptor) {
  arnm_graded_arena_pool *pool =
      arnm_graded_arena_pool_create(kSizes, kGradeCount, kSparePerGrade, nullptr);
  ASSERT_NE(pool, nullptr);

  arnm *arena = TakeOne(pool, 700);
  ASSERT_NE(arena, nullptr);
  EXPECT_EQ(arnm_graded_arena_pool_destroy(pool, nullptr), ARNM_ERROR_RESOURCE_IN_USE);

  // the descriptor is still ours, which is the only way the arena can come home
  ASSERT_EQ(arnm_graded_arena_pool_free(pool, arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_graded_arena_pool_destroy(pool, nullptr), ARNM_SUCCESS);
}

TEST(GradedArenaPool, ReleaseLeavesTheEmptyState) {
  arnm_graded_arena_pool pool;
  ASSERT_EQ(arnm_graded_arena_pool_init(&pool, kSizes, kGradeCount, kSparePerGrade), ARNM_SUCCESS);
  arnm *arena = TakeOne(&pool, 128);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(arnm_graded_arena_pool_free(&pool, arena), ARNM_SUCCESS);

  ASSERT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
  EXPECT_EQ(pool.grades, nullptr);
  EXPECT_EQ(pool.grade_count, 0u);
  EXPECT_EQ(arnm_graded_arena_pool_reserved(&pool), 0u);

  arnm *again = nullptr;
  EXPECT_EQ(arnm_graded_arena_pool_alloc(&pool, 128, &again), ARNM_ERROR_NOT_INITIALIZED);
  // and a second release finds nothing to do
  EXPECT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
}

TEST(GradedArenaPool, ReleaseGivesTheWholeLadderBackAndNotJustForgetsIt) {
  // A leak is invisible from inside the process, so it is made loud instead: four rungs of a
  // megabyte, one arena stocked on each, a thousand rounds. Released properly nothing carries
  // over; leaked, the rounds add up to four gigabytes and the cap in memory_limit.h refuses
  // long before the last one. Without a cap this still passes -- see the note there.
  const uint32_t big_sizes[] = {1024 * 1024, 2 * 1024 * 1024};

  for (int round = 0; round < 1000; ++round) {
    arnm_graded_arena_pool pool;
    ASSERT_EQ(arnm_graded_arena_pool_init(&pool, big_sizes, 2, 1), ARNM_SUCCESS);
    for (uint16_t grade = 0; grade < 2; ++grade) {
      ASSERT_EQ(
          arnm_dynamic_arena_pool_reserve(arnm_graded_arena_pool_grade_at(&pool, grade), 1),
          ARNM_SUCCESS
      ) << "round "
        << round << " grade " << grade;
    }
    ASSERT_EQ(arnm_graded_arena_pool_release(&pool), ARNM_SUCCESS);
  }
}
