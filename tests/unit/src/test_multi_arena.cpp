#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"

#include "memory_intern.h"
#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

// Small arenas on purpose: every test crosses an arena boundary. The capacity stays well above
// the full threshold, or an arena would count as full the moment it opens and each allocation
// would get one of its own -- which init refuses outright.
//
// The 0 threaded through every init call is the full threshold, left at
// ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING (128); the tests that mean to tune it say so.
namespace {

constexpr uint32_t kArenaCapacity = 1024;

/** Every pointer the chain hands out is 8 byte aligned, in every arena. */
void ExpectAligned(const uint8_t *p) {
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

/**
 * A chain with the capacity and threshold a test names, or nullptr when create refuses.
 *
 * The chain is a heap handle now rather than a struct the caller owns, so every test takes it
 * from here and gives it back with arnm_destroy().
 */
arnm *MakeChain(uint32_t capacity, uint32_t full_remaining, arnm *allocator) {
  arnm_multi_arena_options options{};
  options.arena_capacity = capacity;
  options.full_remaining = full_remaining;
  return arnm_create_multi_arena(&options, allocator);
}

/** The common case: this file's capacity, the host's memory. */
arnm *MakeChain() {
  return MakeChain(kArenaCapacity, 0, nullptr);
}

/** Figures of a chain, or a zeroed set when the call fails -- keeps the tests to one line. */
arnm_multi_arena_stats Measure(const arnm *m) {
  arnm_multi_arena_stats stats{};
  EXPECT_EQ(arnm_multi_arena_measure(m, &stats), ARNM_SUCCESS);
  return stats;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle and empty states
// ---------------------------------------------------------------------------

TEST(MultiArena, EmptyAfterInit) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);
  const arnm_multi_arena_stats stats = Measure(m);
  EXPECT_EQ(stats.reserved, 0u);
  EXPECT_EQ(stats.used, 0u);
  EXPECT_EQ(stats.arena_count, 0u);
  EXPECT_EQ(stats.open_count, 0u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, ZeroInitializedIsNotAChain) {
  // The chain used to be a struct the caller owned, and a zeroed one was already a usable chain.
  // A handle is opaque now and a zeroed one is the default allocator -- it hands out the host's
  // memory, holds nothing, and is not a chain at all. Worth stating: the old reading would
  // silently turn "I forgot to create it" into malloc traffic under a chain's name.
  arnm m{};
  EXPECT_FALSE(arnm_is_multi_arena(&m));
  EXPECT_FALSE(arnm_is_arena(&m));
  EXPECT_EQ(arnm_multi_arena_arena_count(&m), 0u);

  arnm_multi_arena_stats stats{};
  EXPECT_EQ(arnm_multi_arena_measure(&m, &stats), ARNM_ERROR_INVALID_STATE);

  // it still allocates, straight from the host, and the block is the host's to free
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &m), ARNM_SUCCESS);
  ASSERT_NE(buffer, nullptr);
  ExpectAligned(buffer);
  EXPECT_EQ(arnm_multi_arena_arena_count(&m), 0u);
  EXPECT_EQ(arnm_free(buffer, 32, &m), ARNM_SUCCESS);
}

TEST(MultiArena, CreateRejectsBadArguments) {
  // The options are checked by a function of their own now, so a caller can find out why a set
  // was refused instead of only that create answered NULL. Both routes are checked against each
  // other here: whatever _validate refuses, _create must refuse as well.
  auto validate = [](uint32_t capacity, uint32_t full_remaining) {
    arnm_multi_arena_options options{};
    options.arena_capacity = capacity;
    options.full_remaining = full_remaining;
    return arnm_multi_arena_options_validate(&options);
  };

  EXPECT_EQ(arnm_multi_arena_options_validate(nullptr), ARNM_ERROR_NULL_POINTER);

  // rounding the capacity up to 8 would wrap uint32_t
  EXPECT_EQ(validate(UINT32_MAX, 0), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(MakeChain(UINT32_MAX, 0, nullptr), nullptr);

  // a threshold that reaches the capacity would write every arena off at birth
  EXPECT_EQ(validate(kArenaCapacity, kArenaCapacity), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(validate(kArenaCapacity, kArenaCapacity + 1), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(MakeChain(kArenaCapacity, kArenaCapacity, nullptr), nullptr);
  EXPECT_EQ(MakeChain(kArenaCapacity, kArenaCapacity + 1, nullptr), nullptr);

  // the capacity it is measured against is the effective one, so a 0 there means the default
  EXPECT_EQ(MakeChain(0, ARNM_MULTI_ARENA_DEFAULT_CAPACITY, nullptr), nullptr);
  arnm *defaulted = MakeChain(0, ARNM_MULTI_ARENA_DEFAULT_CAPACITY - 8, nullptr);
  ASSERT_NE(defaulted, nullptr);
  EXPECT_EQ(arnm_destroy(defaulted, nullptr), ARNM_SUCCESS);

  // the bucket exponent of the descriptor vector has its own ceiling
  arnm_multi_arena_options too_wide{};
  too_wide.arena_capacity = kArenaCapacity;
  too_wide.bucket_size_log2 = 16;
  EXPECT_EQ(arnm_multi_arena_options_validate(&too_wide), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  // one below the capacity is allowed: a fresh arena still has room the threshold does not claim
  arnm *m = MakeChain(kArenaCapacity, kArenaCapacity - 8, nullptr);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(arnm_destroy(m, nullptr), ARNM_SUCCESS);
}

TEST(MultiArena, CreateTakesTheDescriptorFromWhereverTheHostSays) {
  // One allocator with both jobs now: it hands out the chain handle and feeds the descriptor
  // vector inside it. Pointed at host storage here, so nothing about this chain reaches the
  // heap except the arenas it opens for payload.
  alignas(8) uint8_t store_blob[4096];
  arnm store{};
  ASSERT_EQ(arnm_init_arena_borrow(&store, store_blob, sizeof(store_blob)), ARNM_SUCCESS);

  arnm *m = MakeChain(kArenaCapacity, 0, &store);
  ASSERT_NE(m, nullptr);
  EXPECT_GE(reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(store_blob));
  EXPECT_LT(
      reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(store_blob) + sizeof(store_blob)
  );
  // handle and chain body come out of one block, so one allocation covers both
  const uint32_t after_create = ARNM_INTERN(&store)->last_index;
  EXPECT_EQ(after_create, ARNM_ALIGN8(sizeof(arnm) + sizeof(arnm_multi_arena)));

  // the chain works, and its vector grows out of the same blob
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, m), ARNM_SUCCESS);
  ExpectAligned(buffer);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_GT(ARNM_INTERN(&store)->last_index, after_create);

  // the vector is given back first, which leaves the handle the tail again
  EXPECT_EQ(arnm_destroy(m, &store), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&store)->last_index, 0u);

  arnm_release(&store);
}

TEST(MultiArena, CreateGivesTheDescriptorBackWhenInitRefuses) {
  // The rejected arguments must not leave the descriptor stranded in the host's arena: it was
  // the tail when create took it, and it is handed straight back on the same allocator.
  alignas(8) uint8_t descriptor_blob[512];
  arnm descriptor_store{};
  ASSERT_EQ(
      arnm_init_arena_borrow(&descriptor_store, descriptor_blob, sizeof(descriptor_blob)),
      ARNM_SUCCESS
  );

  // a threshold that reaches the capacity is refused by init, after create allocated
  EXPECT_EQ(MakeChain(kArenaCapacity, kArenaCapacity, &descriptor_store), nullptr);
  EXPECT_EQ(ARNM_INTERN(&descriptor_store)->last_index, 0u); // nothing stranded

  // and the arena is still usable for the next attempt, at the very same address
  arnm *m = MakeChain(kArenaCapacity, 0, &descriptor_store);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(descriptor_blob));
  EXPECT_EQ(arnm_destroy(m, &descriptor_store), ARNM_SUCCESS);

  arnm_release(&descriptor_store);
}

TEST(MultiArena, FullThresholdDecidesWhenAnArenaLeavesTheScan) {
  // 512 is far above the default 128: an arena with half its room left is written off, so the
  // 256 byte request below never sees it again
  arnm *m = MakeChain(kArenaCapacity, 512, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 512, m), ARNM_SUCCESS);
  // exactly 512 left, which this chain calls used up
  EXPECT_EQ(Measure(m).open_count, 0u);

  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&second, 256, m), ARNM_SUCCESS);
  EXPECT_NE(second, first + 512); // room was there, the threshold gave it up
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, ASmallThresholdKeepsAnArenaInTheScan) {
  // 8 is far below the default 128, and the remainder below sits between the two: this chain
  // keeps chasing it where a default one would already have written the arena off
  arnm *m = MakeChain(kArenaCapacity, 8, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, kArenaCapacity - 64, m), ARNM_SUCCESS);
  // 64 bytes left: below the default threshold, above this chain's
  EXPECT_EQ(Measure(m).open_count, 1u);

  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&second, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(second, first + kArenaCapacity - 64); // served from the tail of arena 0
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_EQ(Measure(m).used, kArenaCapacity); // the arena was used down to the last byte

  // nothing left at all: even this chain passes it now
  EXPECT_EQ(Measure(m).open_count, 0u);
  uint8_t *third = nullptr;
  ASSERT_EQ(arnm_alloc(&third, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, AThresholdOneStepBelowTheRequestIsTheExactLine) {
  // What the module text rests on: an arena serves a request of n while n bytes are left, and is
  // written off at or below the threshold. n - 8 is therefore the value that writes it off
  // exactly when it can no longer take that request -- n itself already gives up one request's
  // worth per arena. Both chains are left with exactly 256 bytes, and only the choice differs.
  constexpr uint32_t kRequest = 256;
  arnm *exact = nullptr;
  arnm *one_step_high = nullptr;
  exact = MakeChain(kArenaCapacity, kRequest - 8, nullptr);
  ASSERT_NE(exact, nullptr);
  one_step_high = MakeChain(kArenaCapacity, kRequest, nullptr);
  ASSERT_NE(one_step_high, nullptr);

  uint8_t *a = nullptr;
  uint8_t *b = nullptr;
  ASSERT_EQ(arnm_alloc(&a, kArenaCapacity - kRequest, exact), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&b, kArenaCapacity - kRequest, one_step_high), ARNM_SUCCESS);
  EXPECT_EQ(Measure(exact).open_count, 1u);         // 256 left, above 248
  EXPECT_EQ(Measure(one_step_high).open_count, 0u); // 256 left, not above 256

  uint8_t *a2 = nullptr;
  uint8_t *b2 = nullptr;
  ASSERT_EQ(arnm_alloc(&a2, kRequest, exact), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&b2, kRequest, one_step_high), ARNM_SUCCESS);

  // the exact line uses the arena to its last byte; one step high opens fresh ground and leaves
  // a full request's worth behind, which is what "a little above" would have cost per arena
  EXPECT_EQ(a2, a + kArenaCapacity - kRequest);
  EXPECT_EQ(arnm_multi_arena_arena_count(exact), 1u);
  EXPECT_EQ(Measure(exact).used, kArenaCapacity);

  EXPECT_NE(b2, b + kArenaCapacity - kRequest);
  EXPECT_EQ(arnm_multi_arena_arena_count(one_step_high), 2u);

  // the same bytes handed out either way, twice the ground held to do it
  EXPECT_EQ(Measure(exact).used, Measure(one_step_high).used);
  EXPECT_EQ(Measure(exact).reserved, kArenaCapacity);
  EXPECT_EQ(Measure(one_step_high).reserved, 2u * kArenaCapacity);

  arnm_destroy(exact, nullptr);
  arnm_destroy(one_step_high, nullptr);
}

TEST(MultiArena, ZeroSelectsTheDefaultThreshold) {
  // 0 asks for the default, not for "write nothing off" -- the two chains must behave alike
  arnm *zero = MakeChain(kArenaCapacity, 0, nullptr);
  arnm *named = MakeChain(kArenaCapacity, ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING, nullptr);
  ASSERT_NE(zero, nullptr);
  ASSERT_NE(named, nullptr);

  // leaves exactly the default threshold behind, so both chains write the arena off. Named
  // through the macro rather than as a literal: the point is that the two agree, not what the
  // number happens to be today.
  constexpr uint32_t kDefault = ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING;
  uint8_t *a = nullptr;
  uint8_t *b = nullptr;
  ASSERT_EQ(arnm_alloc(&a, kArenaCapacity - kDefault, zero), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&b, kArenaCapacity - kDefault, named), ARNM_SUCCESS);
  EXPECT_EQ(Measure(zero).open_count, 0u);
  EXPECT_EQ(Measure(named).open_count, 0u);

  uint8_t *a2 = nullptr;
  uint8_t *b2 = nullptr;
  ASSERT_EQ(arnm_alloc(&a2, 64, zero), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&b2, 64, named), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(zero), 2u);
  EXPECT_EQ(arnm_multi_arena_arena_count(named), 2u);

  arnm_destroy(zero, nullptr);
  arnm_destroy(named, nullptr);
}

TEST(MultiArena, NullIsTheHostForAllocationAndAMistakeForTheChain) {
  // The two halves of the interface read NULL differently, on purpose. The generic entry points
  // take it as "use the host" -- that is what makes an allocator argument optional everywhere.
  // The chain specific ones have nothing to fall back to and say so.
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 8, nullptr), ARNM_SUCCESS);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(arnm_free(buffer, 8, nullptr), ARNM_SUCCESS);

  EXPECT_EQ(arnm_multi_arena_reserve(nullptr, 4), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_multi_arena_shrink(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_multi_arena_borrow(nullptr, nullptr, 64), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_multi_arena_measure(nullptr, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_FALSE(arnm_is_multi_arena(nullptr));

  // NULL is a no-op, not a crash. Destroy says so out loud: nothing was handed out, so nothing
  // stayed behind -- the arena warning would read as the opposite, and it must not appear here
  // whichever allocator is named.
  arnm_reset(nullptr);
  arnm_release(nullptr);
  EXPECT_EQ(arnm_destroy(nullptr, nullptr), ARNM_SUCCESS);

  alignas(8) uint8_t storage[64];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_destroy(nullptr, &arena), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&arena)->last_index, 0u); // did not move an index over a block it never had
  arnm_release(&arena);
}

TEST(MultiArena, CreateAndDestroy) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(Measure(m).reserved, kArenaCapacity);

  // malloc backed, so the descriptor really goes back and there is nothing left to warn about
  EXPECT_EQ(arnm_destroy(m, nullptr), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// allocation
// ---------------------------------------------------------------------------

TEST(MultiArena, AllocRejectsBadArguments) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // seeded, so "untouched" is distinguishable from "set to NULL", and checked after each
  uint8_t marker = 0;
  uint8_t *buffer = &marker;
  EXPECT_EQ(arnm_alloc(nullptr, 8, m), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_alloc(&buffer, 0, m), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer, &marker);
  EXPECT_EQ(arnm_alloc(&buffer, UINT32_MAX, m), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(buffer, &marker);
  // nothing was opened on the way
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, SizesRoundUpToEight) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 1, m), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&second, 1, m), ARNM_SUCCESS);
  // one byte asked for, eight reserved -- twice
  EXPECT_EQ(second - first, 8);
  EXPECT_EQ(Measure(m).used, 16u);
  ExpectAligned(first);
  ExpectAligned(second);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, OpensAnotherArenaWhenTheCurrentOneIsFull) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // 4 x 256 fills the first arena exactly
  std::vector<uint8_t *> blocks;
  for (int i = 0; i < 4; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, 256, m), ARNM_SUCCESS);
    blocks.push_back(buffer);
  }
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_EQ(Measure(m).used, kArenaCapacity);

  uint8_t *fifth = nullptr;
  ASSERT_EQ(arnm_alloc(&fifth, 256, m), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  const arnm_multi_arena_stats stats = Measure(m);
  EXPECT_EQ(stats.reserved, 2u * kArenaCapacity);
  EXPECT_EQ(stats.used, kArenaCapacity + 256u);
  EXPECT_EQ(stats.open_count, 1u); // only the young one still has room

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, EveryBlockKeepsItsBytesAcrossManyArenas) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  constexpr uint32_t kBlockSize = 96;
  constexpr uint32_t kBlocks = 500; // far more than one arena holds
  std::vector<uint8_t *> blocks;
  std::set<const void *> distinct;

  for (uint32_t i = 0; i < kBlocks; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, kBlockSize, m), ARNM_SUCCESS) << i;
    ASSERT_NE(buffer, nullptr);
    ExpectAligned(buffer);
    ASSERT_TRUE(distinct.insert(buffer).second) << "block " << i << " handed out twice";
    std::memset(buffer, static_cast<int>(i & 0xFF), kBlockSize);
    blocks.push_back(buffer);
  }
  EXPECT_GT(arnm_multi_arena_arena_count(m), 1u);

  // the pattern survives every later allocation: an arena, once opened, never moves
  for (uint32_t i = 0; i < kBlocks; ++i) {
    for (uint32_t byte = 0; byte < kBlockSize; ++byte) {
      ASSERT_EQ(blocks[i][byte], static_cast<uint8_t>(i & 0xFF)) << "block " << i;
    }
  }

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, OversizedRequestGetsAnArenaOfItsOwn) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *small = nullptr;
  ASSERT_EQ(arnm_alloc(&small, 256, m), ARNM_SUCCESS);

  // four times what a regular arena holds: not refused, given ground of its own
  uint8_t *huge = nullptr;
  ASSERT_EQ(arnm_alloc(&huge, 4 * kArenaCapacity, m), ARNM_SUCCESS);
  ASSERT_NE(huge, nullptr);
  std::memset(huge, 0xAB, 4 * kArenaCapacity);

  arnm_multi_arena_stats stats = Measure(m);
  EXPECT_EQ(stats.arena_count, 2u);
  EXPECT_EQ(stats.reserved, 5u * kArenaCapacity);
  EXPECT_EQ(stats.open_count, 1u); // the dedicated arena is full on arrival

  // the first arena still has room, and the dedicated one does not get in the way
  uint8_t *another = nullptr;
  ASSERT_EQ(arnm_alloc(&another, 256, m), ARNM_SUCCESS);
  EXPECT_EQ(another, small + 256);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, AnArenaTooSmallForOneRequestStillServesTheNext) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // leaves 512 bytes in arena 0 -- above the full threshold, below the next request
  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 512, m), ARNM_SUCCESS);

  uint8_t *big = nullptr;
  ASSERT_EQ(arnm_alloc(&big, 768, m), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  // arena 0 was skipped, not closed: a request it can hold lands there again
  uint8_t *small = nullptr;
  ASSERT_EQ(arnm_alloc(&small, 128, m), ARNM_SUCCESS);
  EXPECT_EQ(small, first + 512);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, AnArenaThatHasRunFullIsLeftBehindForGood) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // 64 bytes left, at or below the full threshold: the front marker moves past this arena and
  // does not come back to it, even for a request its remainder would still hold
  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 960, m), ARNM_SUCCESS);
  EXPECT_EQ(Measure(m).open_count, 0u);

  uint8_t *tiny = nullptr;
  ASSERT_EQ(arnm_alloc(&tiny, 32, m), ARNM_SUCCESS);
  EXPECT_NE(tiny, first + 960);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, TheScanCarriesOnPastAnArenaThatIsTooSmall) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // arena 0 keeps 224 bytes: too few for what follows, too many to count as full
  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 800, m), ARNM_SUCCESS);
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&second, 512, m), ARNM_SUCCESS);
  ASSERT_EQ(arnm_multi_arena_arena_count(m), 2u);

  // 400 fits neither arena 0 nor nothing: the scan steps over arena 0 and finds arena 1
  uint8_t *third = nullptr;
  ASSERT_EQ(arnm_alloc(&third, 400, m), ARNM_SUCCESS);
  EXPECT_EQ(third, second + 512);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, Clone) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  const uint8_t source[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint8_t *copy = nullptr;
  ASSERT_EQ(arnm_clone(&copy, source, sizeof(source), m), ARNM_SUCCESS);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, source);
  EXPECT_EQ(std::memcmp(copy, source, sizeof(source)), 0);
  // 9 bytes asked for, 16 reserved
  EXPECT_EQ(Measure(m).used, 16u);

  EXPECT_EQ(arnm_clone(nullptr, source, 4, m), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_clone(&copy, nullptr, 4, m), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_clone(&copy, source, 0, m), ARNM_ERROR_INVALID_PARAM);

  arnm_destroy(m, nullptr);
}

// ---------------------------------------------------------------------------
// giving memory back
// ---------------------------------------------------------------------------

TEST(MultiArena, FreeTakesBackOnlyTheTailOfItsArena) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 64, m), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&second, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(Measure(m).used, 128u);

  // buried: the block stays where it is
  EXPECT_EQ(arnm_free(first, 64, m), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(Measure(m).used, 128u);

  // the tail comes back, and the next request takes its place
  EXPECT_EQ(arnm_free(second, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(Measure(m).used, 64u);
  uint8_t *again = nullptr;
  ASSERT_EQ(arnm_alloc(&again, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(again, second);

  // NULL is never a tail
  EXPECT_EQ(arnm_free(nullptr, 64, m), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, FreeRejectsAnAddressFromSomewhereElse) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, m), ARNM_SUCCESS);

  alignas(8) uint8_t foreign[64] = {0};
  EXPECT_EQ(arnm_free(foreign, 64, m), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(Measure(m).used, 64u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, FreeReopensAnArenaThePathHadPassed) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  // fill arena 0 to the brim, then force arena 1 open
  uint8_t *last_in_first = nullptr;
  for (int i = 0; i < 4; ++i) { ASSERT_EQ(arnm_alloc(&last_in_first, 256, m), ARNM_SUCCESS); }
  uint8_t *in_second = nullptr;
  ASSERT_EQ(arnm_alloc(&in_second, 256, m), ARNM_SUCCESS);
  ASSERT_EQ(arnm_multi_arena_arena_count(m), 2u);

  // arena 0 has room again, and the front marker follows back to it
  ASSERT_EQ(arnm_free(last_in_first, 256, m), ARNM_SUCCESS);
  uint8_t *reused = nullptr;
  ASSERT_EQ(arnm_alloc(&reused, 256, m), ARNM_SUCCESS);
  EXPECT_EQ(reused, last_in_first);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, ResetKeepsTheArenasAndAsksTheHostForNothing) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *very_first = nullptr;
  ASSERT_EQ(arnm_alloc(&very_first, 256, m), ARNM_SUCCESS);
  for (int i = 0; i < 10; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, 256, m), ARNM_SUCCESS);
  }
  const uint32_t arenas = arnm_multi_arena_arena_count(m);
  ASSERT_GT(arenas, 1u);
  const uint64_t reserved = Measure(m).reserved;

  arnm_reset(m);
  arnm_multi_arena_stats stats = Measure(m);
  EXPECT_EQ(stats.used, 0u);
  EXPECT_EQ(stats.arena_count, arenas);
  EXPECT_EQ(stats.reserved, reserved);
  EXPECT_EQ(stats.open_count, arenas);

  // the second pass runs inside the ground of the first
  uint8_t *after_reset = nullptr;
  ASSERT_EQ(arnm_alloc(&after_reset, 256, m), ARNM_SUCCESS);
  EXPECT_EQ(after_reset, very_first);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), arenas);

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, ShrinkReleasesTheTrailingEmptyArenas) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  for (int i = 0; i < 10; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, 256, m), ARNM_SUCCESS);
  }
  ASSERT_GT(arnm_multi_arena_arena_count(m), 1u);

  // a shrink between allocations keeps everything: no trailing arena is empty
  ASSERT_EQ(arnm_multi_arena_shrink(m), ARNM_SUCCESS);
  EXPECT_GT(arnm_multi_arena_arena_count(m), 1u);

  // after a reset the whole chain is empty and goes back to the host
  arnm_reset(m);
  ASSERT_EQ(arnm_multi_arena_shrink(m), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);
  EXPECT_EQ(Measure(m).reserved, 0u);

  // and the chain still works afterwards
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);

  arnm_destroy(m, nullptr);
}

// ---------------------------------------------------------------------------
// borrowed ground
// ---------------------------------------------------------------------------

TEST(MultiArena, BorrowedBufferIsUsedAndNeverFreed) {
  alignas(8) uint8_t host_block[512];
  std::memset(host_block, 0x5A, sizeof(host_block));

  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(arnm_multi_arena_borrow(m, host_block, sizeof(host_block)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_EQ(Measure(m).reserved, sizeof(host_block));

  // borrowed before the first allocation, so it is filled first
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 128, m), ARNM_SUCCESS);
  EXPECT_EQ(buffer, host_block);

  // _release empties the chain but keeps the handle, which is what the count is read through;
  // _destroy hands the handle itself back and nothing may touch it afterwards
  arnm_release(m);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);
  // the host's block was borrowed, not owned: still ours, still readable
  EXPECT_EQ(host_block[511], 0x5A);
  EXPECT_EQ(arnm_destroy(m, nullptr), ARNM_SUCCESS);
}

TEST(MultiArena, ShrinkStopsAtBorrowedGround) {
  alignas(8) uint8_t host_block[512];

  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(arnm_multi_arena_borrow(m, host_block, sizeof(host_block)), ARNM_SUCCESS);

  // 512 bytes borrowed, then more than that asked for: an owned arena joins behind it
  for (int i = 0; i < 4; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, 256, m), ARNM_SUCCESS);
  }
  ASSERT_EQ(arnm_multi_arena_arena_count(m), 2u);

  arnm_reset(m);
  ASSERT_EQ(arnm_multi_arena_shrink(m), ARNM_SUCCESS);
  // the owned arena went back, the borrowed one stayed
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_EQ(Measure(m).reserved, sizeof(host_block));

  arnm_destroy(m, nullptr);
}

TEST(MultiArena, BorrowRejectsBadArguments) {
  alignas(8) uint8_t host_block[64];

  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  EXPECT_EQ(arnm_multi_arena_borrow(m, nullptr, 64), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_multi_arena_borrow(m, host_block, 0), ARNM_ERROR_INVALID_PARAM);
  // capacity not a multiple of 8, and a base that is not 8 byte aligned
  EXPECT_EQ(arnm_multi_arena_borrow(m, host_block, 60), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_multi_arena_borrow(m, host_block + 1, 56), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 0u);

  arnm_destroy(m, nullptr);
}

// ---------------------------------------------------------------------------
// bookkeeping
// ---------------------------------------------------------------------------

TEST(MultiArena, BookkeepingCanComeFromAnArena) {
  // the descriptor vector draws from the host's blob as well; nothing calls malloc but the
  // arenas themselves
  alignas(8) uint8_t bookkeeping_block[4096];
  arnm bookkeeping{};
  ASSERT_EQ(
      arnm_init_arena_borrow(&bookkeeping, bookkeeping_block, sizeof(bookkeeping_block)),
      ARNM_SUCCESS
  );

  arnm *m = MakeChain(kArenaCapacity, 0, &bookkeeping);
  ASSERT_NE(m, nullptr);
  // an arena cannot reclaim a superseded index array, so the slots are taken once, up front
  ASSERT_EQ(arnm_multi_arena_reserve(m, 16), ARNM_SUCCESS);
  const uint32_t after_reserve = ARNM_INTERN(&bookkeeping)->last_index;
  EXPECT_GT(after_reserve, 0u);

  for (int i = 0; i < 12; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, 512, m), ARNM_SUCCESS);
  }
  EXPECT_GT(arnm_multi_arena_arena_count(m), 1u);
  // the reservation covered every descriptor: the bookkeeping arena never grew again
  EXPECT_EQ(ARNM_INTERN(&bookkeeping)->last_index, after_reserve);

  // the handle came out of the bookkeeping arena, so that is where it has to go back; naming
  // nullptr here would hand an address the host never allocated to free()
  EXPECT_EQ(arnm_destroy(m, &bookkeeping), ARNM_SUCCESS);
  arnm_release(&bookkeeping);
}

// ---------------------------------------------------------------------------
// exhaustion
// ---------------------------------------------------------------------------

#if defined(__linux__) && !defined(ARNM_TEST_SKIP_MEMORY_LIMIT)
TEST(MultiArena, AnArenaThatCannotBeOpenedLeavesTheChainUntouched) {
  arnm *m = MakeChain(kArenaCapacity, 0, nullptr);
  ASSERT_NE(m, nullptr);

  uint8_t *kept = nullptr;
  ASSERT_EQ(arnm_alloc(&kept, 64, m), ARNM_SUCCESS);

  // beyond the address space this binary caps itself to (memory_limit.h)
  uint8_t *impossible = nullptr;
  EXPECT_EQ(arnm_alloc(&impossible, UINT32_MAX - 7, m), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(impossible, nullptr);

  // the failure changed nothing: same arena, same tail
  EXPECT_EQ(arnm_multi_arena_arena_count(m), 1u);
  EXPECT_EQ(Measure(m).used, 64u);
  uint8_t *next = nullptr;
  ASSERT_EQ(arnm_alloc(&next, 64, m), ARNM_SUCCESS);
  EXPECT_EQ(next, kept + 64);

  arnm_destroy(m, nullptr);
}
#endif
