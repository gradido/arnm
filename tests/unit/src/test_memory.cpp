#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/result.h"
#include "memory_intern.h"
#include "memory_limit.h"
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <utility>

// The arena rounds every size up to a multiple of 8, so a request of `n` moves the
// bump index by ARNM_ALIGN8(n). Tests that check `last_index` state that explicitly.

TEST(MemoryTest, CreateTakesTheDescriptorFromWhereverTheHostSays) {
  // The point of the parameter: a binding can keep the allocator struct itself inside storage
  // it owns, instead of it being the one allocation that escaped into malloc.
  alignas(8) uint8_t host_blob[512];
  arnm host{};
  ASSERT_EQ(arnm_init_arena_borrow(&host, host_blob, sizeof(host_blob)), ARNM_SUCCESS);

  arnm *carved = arnm_create(&host);
  ASSERT_NE(carved, nullptr);
  // it really came out of the blob, not from the heap
  EXPECT_GE(reinterpret_cast<uintptr_t>(carved), reinterpret_cast<uintptr_t>(host_blob));
  EXPECT_LT(
      reinterpret_cast<uintptr_t>(carved),
      reinterpret_cast<uintptr_t>(host_blob) + sizeof(host_blob)
  );
  EXPECT_EQ(reinterpret_cast<uintptr_t>(carved) % 8, 0u);
  EXPECT_EQ(ARNM_INTERN(&host)->last_index, ARNM_ALIGN8(sizeof(arnm)));

  // zeroed and usable, exactly as the malloc-backed one is
  EXPECT_EQ(ARNM_INTERN(carved)->allocation_type, ARNM_ALLOC_TYPE_DEFAULT);
  uint8_t *from_carved = nullptr;
  ASSERT_EQ(arnm_alloc(&from_carved, 16, carved), ARNM_SUCCESS); // default mode: malloc
  arnm_free(from_carved, 16, carved);

  // still the tail of the host arena, so it comes back whole
  EXPECT_EQ(arnm_destroy(carved, &host), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&host)->last_index, 0u);

  // and the host's own buffer is untouched by all of it
  arnm_release(&host);
  host_blob[0] = 0x42;
  EXPECT_EQ(host_blob[0], 0x42);
}

TEST(MemoryTest, DestroyReportsWhatTheArenaCouldNotTakeBack) {
  // A descriptor buried under a later allocation cannot move the bump index. Everything it held
  // is released regardless; only its own bytes wait for the arena's reset, and the caller is
  // told so rather than left to assume otherwise.
  alignas(8) uint8_t host_blob[512];
  arnm host{};
  ASSERT_EQ(arnm_init_arena_borrow(&host, host_blob, sizeof(host_blob)), ARNM_SUCCESS);

  arnm *buried = arnm_create(&host);
  ASSERT_NE(buried, nullptr);
  uint8_t *on_top = nullptr;
  ASSERT_EQ(arnm_alloc(&on_top, 32, &host), ARNM_SUCCESS); // now it is not the tail
  const uint32_t index_before = ARNM_INTERN(&host)->last_index;

  EXPECT_EQ(arnm_destroy(buried, &host), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(
      ARNM_INTERN(&host)->last_index, index_before
  ); // the index did not move by the wrong amount either

  // NULL is a no-op success, not a warning: nothing was ever handed out to keep
  EXPECT_EQ(arnm_destroy(nullptr, &host), ARNM_SUCCESS);
  EXPECT_EQ(arnm_destroy(nullptr, nullptr), ARNM_SUCCESS);

  arnm_release(&host);
}

TEST(MemoryTest, AFailedAllocLeavesTheOutputPointerAlone) {
  // "Failures leave every output untouched", checked where every rejection is decided by the
  // arguments alone. Seeded with a real address rather than nullptr, so a written NULL shows up
  // instead of hiding behind the value the pointer already had.
  uint8_t marker = 0;
  uint8_t *buffer = &marker;

  alignas(8) uint8_t storage[64];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  EXPECT_EQ(arnm_alloc(&buffer, 0, &arena), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer, &marker);
  EXPECT_EQ(arnm_alloc(&buffer, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer, &marker);
  // rounding up to 8 would wrap, so the arena refuses before reserving anything. Arena mode
  // only: the default path hands the size to the host untouched and has nothing to round.
  EXPECT_EQ(arnm_alloc(&buffer, ARNM_MAX_ALLOC_SIZE + 1, &arena), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(buffer, &marker);
  // a full arena, which needs no help from the host to say no
  EXPECT_EQ(arnm_alloc(&buffer, 128, &arena), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(buffer, &marker);

  // and a call that succeeds does replace it, so the checks above cannot pass by accident
  ASSERT_EQ(arnm_alloc(&buffer, 32, &arena), ARNM_SUCCESS);
  EXPECT_NE(buffer, &marker);
  arnm_release(&arena);
}

TEST(MemoryTest, AFailedMallocLeavesTheOutputPointerAlone) {
  // The default path is where the promise is easy to lose: assigning malloc's result straight
  // into *buffer writes a NULL over whatever the caller held. Only a real refusal exercises it,
  // and only the address space cap can promise one -- without it Linux overcommit answers a
  // 4 GiB request with an address, and this test would fail while leaking the block.
  constexpr unsigned long long kUnservable = ARNM_MAX_ALLOC_SIZE;
  if (!ArnmTestAllocationMustFail(kUnservable)) {
    GTEST_SKIP() << "no address space cap in force, so no allocation can be made to fail";
  }

  uint8_t marker = 0;
  uint8_t *buffer = &marker;
  const arnm_result result = arnm_alloc(&buffer, ARNM_MAX_ALLOC_SIZE, nullptr);

  // released before anything else, so an unexpected success cannot leave the block behind
  if (ARNM_SUCCESS == result) { arnm_free(buffer, ARNM_MAX_ALLOC_SIZE, nullptr); }

  EXPECT_EQ(result, ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(buffer, &marker);
}

TEST(MemoryTest, Align8RoundsUpAndRefusesToWrap) {
  // The rounding answers with the size now instead of through an out parameter, and 0 is the
  // refusal. That folds "nothing to round" into "cannot be rounded" -- which costs nothing,
  // because every caller rejects a size of 0 on its own terms before reaching here.
  EXPECT_EQ(arnm_align8_u32(0), 0u);

  for (const auto &c : {std::pair<uint32_t, uint32_t>{1, 8}, {3, 8}, {8, 8}, {9, 16}, {99, 104}}) {
    EXPECT_EQ(arnm_align8_u32(c.first), c.second) << c.first;
  }

  // the ceiling is a multiple of 8 already, so it survives the rounding untouched
  EXPECT_EQ(arnm_align8_u32(ARNM_MAX_ALLOC_SIZE), ARNM_MAX_ALLOC_SIZE);

  // one byte more cannot be rounded without wrapping
  EXPECT_EQ(arnm_align8_u32(ARNM_MAX_ALLOC_SIZE + 1), 0u);
  EXPECT_EQ(arnm_align8_u32(UINT32_MAX), 0u);
}

TEST(MemoryTest, Align8IsTheFigureTheArenaActuallyReserves) {
  // The reason this rounding is shared rather than copied: what it returns has to be what the
  // bump index moves by, or freeing with the caller's size would move it back by the wrong
  // amount. Checked against the arena instead of against the formula.
  alignas(8) uint8_t storage[256];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  for (uint32_t request : {1u, 7u, 8u, 9u, 31u, 32u}) {
    const uint32_t expected = arnm_align8_u32(request);
    ASSERT_NE(expected, 0u) << request;

    const uint32_t before = ARNM_INTERN(&arena)->last_index;
    uint8_t *block = nullptr;
    ASSERT_EQ(arnm_alloc(&block, request, &arena), ARNM_SUCCESS) << request;
    EXPECT_EQ(ARNM_INTERN(&arena)->last_index - before, expected) << request;

    // and back again with the size the caller passed, which is where the two must agree
    ASSERT_EQ(arnm_free(block, request, &arena), ARNM_SUCCESS) << request;
    EXPECT_EQ(ARNM_INTERN(&arena)->last_index, before) << request;
  }

  arnm_release(&arena);
}

TEST(MemoryTest, DynamicAreaAllocation) {
  // init
  arnm mem{};
  EXPECT_EQ(arnm_init_arena(&mem, 100), ARNM_SUCCESS);
  // capacity is rounded up to a multiple of 8
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 104u);

  // test valid alloc
  arnm_memory_block block{};
  EXPECT_EQ(arnm_memory_block_alloc(&block, 99, &mem), ARNM_SUCCESS);
  EXPECT_EQ(block.size, 99u);
  EXPECT_TRUE(block.data);
  // 99 bytes asked for, 104 reserved
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 104u);

  // test alloc over the allocated area
  EXPECT_EQ(arnm_memory_block_alloc(&block, 2, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 8u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// allocator lifecycle
// ---------------------------------------------------------------------------

TEST(MemoryTest, InitArenaRejectsBadArguments) {
  arnm mem{};
  EXPECT_EQ(arnm_init_arena(nullptr, 64), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_init_arena(&mem, 0), ARNM_ERROR_INVALID_PARAM);
  // rounding up to 8 would wrap uint32_t
  EXPECT_EQ(arnm_init_arena(&mem, UINT32_MAX), ARNM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(MemoryTest, InitArenaBorrowRejectsWhatItCannotHonour) {
  alignas(8) uint8_t storage[64];
  arnm mem{};

  EXPECT_EQ(arnm_init_arena_borrow(&mem, nullptr, 64), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_init_arena_borrow(&mem, storage, 0), ARNM_ERROR_INVALID_PARAM);
  // an unaligned base would break the "every pointer is 8 byte aligned" invariant
  EXPECT_EQ(arnm_init_arena_borrow(&mem, storage + 1, 32), ARNM_ERROR_INVALID_PARAM);
  // and a capacity that is not a multiple of 8 is refused rather than rounded up: rounding
  // would let the arena hand out bytes past the end of a buffer the caller sized exactly
  for (uint32_t bad : {1u, 7u, 33u, 63u}) {
    EXPECT_EQ(arnm_init_arena_borrow(&mem, storage, bad), ARNM_ERROR_INVALID_PARAM)
        << "capacity " << bad;
  }

  ASSERT_EQ(arnm_init_arena_borrow(&mem, storage, 64), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->allocation_type, ARNM_ALLOC_TYPE_ARENA_EXTERNAL);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 64u);

  // the arena stays inside what it was given, right up to the last byte
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, &mem), ARNM_SUCCESS);
  EXPECT_EQ(buffer, storage);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, ARNM_INTERN(&mem)->capacity);
  EXPECT_EQ(arnm_alloc(&buffer, 1, &mem), ARNM_ERROR_OUT_OF_MEMORY);

  // an external buffer belongs to the caller and survives the allocator
  arnm_release(&mem);
  storage[0] = 0x42;
  EXPECT_EQ(storage[0], 0x42);
}

TEST(MemoryTest, InitArenaBorrowCanBeRepeatedWithoutFreeing) {
  // nothing is owned, so switching external buffers is just another init
  alignas(8) uint8_t first[64];
  alignas(8) uint8_t second[128];
  arnm mem{};

  ASSERT_EQ(arnm_init_arena_borrow(&mem, first, sizeof(first)), ARNM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &mem), ARNM_SUCCESS);

  ASSERT_EQ(arnm_init_arena_borrow(&mem, second, sizeof(second)), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 128u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);
  ASSERT_EQ(arnm_alloc(&buffer, 128, &mem), ARNM_SUCCESS);
  EXPECT_EQ(buffer, second);

  arnm_release(&mem);
}

TEST(MemoryTest, InitArenaDoesNotReadPriorState) {
  // The point of splitting init and reinit: `arnm mem;` followed by an init is the
  // most natural line to write, and it has to be correct. This emulates the stack garbage
  // that used to make init free a pointer it never owned.
  alignas(8) uint8_t not_from_malloc[64];
  arnm mem;
  memset(&mem, 0xCD, sizeof(mem));
  ARNM_INTERN(&mem)->allocation_type = ARNM_ALLOC_TYPE_ARENA_OWNED; // looks like a live owned arena
  ARNM_INTERN(&mem)->data = not_from_malloc;                        // but this was never malloc'd
  ARNM_INTERN(&mem)->capacity = sizeof(not_from_malloc);

  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 128u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);
  EXPECT_EQ(ARNM_INTERN(&mem)->out_of_memory_capacity, 0u);
  EXPECT_NE(ARNM_INTERN(&mem)->data, not_from_malloc);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 128, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

TEST(MemoryTest, InitArenaLeavesTheAllocatorAloneOnFailure) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);
  uint8_t *before = ARNM_INTERN(&mem)->data;

  // the allocation happens before anything is written, so a rejected request changes nothing
  EXPECT_EQ(arnm_init_arena(&mem, UINT32_MAX), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(ARNM_INTERN(&mem)->data, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 64u);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 64, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

TEST(MemoryTest, ReinitArenaReplacesTheOwnedBuffer) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 64, &mem), ARNM_SUCCESS);
  ASSERT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  // releases the old arena and starts over: no leak, no double free
  ASSERT_EQ(arnm_reinit_arena(&mem, 128), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 128u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  ASSERT_EQ(arnm_alloc(&buffer, 128, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

TEST(MemoryTest, ReinitArenaWorksOnAZeroedAllocator) {
  // the free half has nothing to do, so reinit doubles as a plain init on a zeroed struct
  arnm mem{};
  ASSERT_EQ(arnm_reinit_arena(&mem, 64), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->capacity, 64u);
  EXPECT_EQ(ARNM_INTERN(&mem)->allocation_type, ARNM_ALLOC_TYPE_ARENA_OWNED);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 64, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

TEST(MemoryTest, CreateAndDestroy) {
  arnm *mem = arnm_create(nullptr);
  ASSERT_TRUE(mem);
  // fresh from create it is in default mode: malloc/free
  EXPECT_EQ(ARNM_INTERN(mem)->allocation_type, ARNM_ALLOC_TYPE_DEFAULT);
  EXPECT_EQ(ARNM_INTERN(mem)->capacity, 0u);
  EXPECT_EQ(ARNM_INTERN(mem)->last_index, 0u);

  ASSERT_EQ(arnm_init_arena(mem, 64), ARNM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 16, mem), ARNM_SUCCESS);

  // destroy releases the arena and the allocator itself. Malloc backed, so the descriptor
  // really goes back: nothing is left for the arena warning to be about.
  EXPECT_EQ(arnm_destroy(mem, nullptr), ARNM_SUCCESS);
  // NULL is tolerated and is a success, not a warning -- nothing was handed out to keep
  EXPECT_EQ(arnm_destroy(nullptr, nullptr), ARNM_SUCCESS);
}

TEST(MemoryTest, ResetDropsEverythingAtOnce) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&second, 32, &mem), ARNM_SUCCESS);
  // overflow the arena so the counter is non zero
  uint8_t *third = nullptr;
  ASSERT_EQ(arnm_alloc(&third, 32, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  ASSERT_EQ(arnm_arena_overflow_total(&mem), 32u);

  arnm_reset(&mem);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 0u);

  // the arena buffer is kept, so the first allocation lands where it did before
  uint8_t *again = nullptr;
  ASSERT_EQ(arnm_alloc(&again, 32, &mem), ARNM_SUCCESS);
  EXPECT_EQ(again, first);

  arnm_reset(nullptr); // tolerated
  arnm_release(&mem);
}

TEST(MemoryTest, OverflowCounterSaturatesInsteadOfWrapping) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  // each of these overshoots by nearly the whole uint32_t range
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(arnm_alloc(&buffer, UINT32_MAX - 8, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  }
  // capped, not rolled over to a small number
  EXPECT_EQ(arnm_arena_overflow_total(&mem), UINT32_MAX);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arena introspection
// ---------------------------------------------------------------------------

TEST(MemoryTest, RemainingIsWhatLiesBetweenTheIndexAndTheEnd) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  // an untouched arena still holds all of it
  EXPECT_EQ(arnm_arena_remaining(&mem), 64u);

  uint8_t *first = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 8, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 56u);

  // the reserved figure is what is subtracted, not the requested one: 20 costs 24
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&second, 20, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 32u);

  // the tail comes back and the remainder grows again
  EXPECT_EQ(arnm_free(second, 20, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 56u);

  // which puts the first block back at the tail, so it comes back too
  EXPECT_EQ(arnm_free(first, 8, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 64u);

  // and the whole of it returns at once
  ASSERT_EQ(arnm_alloc(&first, 40, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 24u);
  arnm_reset(&mem);
  EXPECT_EQ(arnm_arena_remaining(&mem), 64u);

  arnm_release(&mem);
}

TEST(MemoryTest, RemainingIsExactlyTheLargestRequestStillServed) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *head = nullptr;
  ASSERT_EQ(arnm_alloc(&head, 17, &mem), ARNM_SUCCESS); // 24 reserved
  const uint32_t remaining = arnm_arena_remaining(&mem);
  ASSERT_EQ(remaining, 40u);

  // one byte past the figure is refused, and refused without touching the index
  uint8_t *too_big = nullptr;
  EXPECT_EQ(arnm_alloc(&too_big, remaining + 1, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(too_big, nullptr);
  EXPECT_EQ(arnm_arena_remaining(&mem), remaining);

  // the figure itself fits exactly, and leaves nothing behind
  uint8_t *exact = nullptr;
  ASSERT_EQ(arnm_alloc(&exact, remaining, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 0u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, ARNM_INTERN(&mem)->capacity);

  arnm_release(&mem);
}

TEST(MemoryTest, RemainingCountsTheRoundedCapacityOfAnOwnedArena) {
  // an owned arena rounds its capacity up to 8, so the remainder starts above what was asked for
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 100), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 104u);
  arnm_release(&mem);

  // a borrowed one is taken exactly as it is, and is measured the same way afterwards
  alignas(8) uint8_t storage[64];
  arnm borrowed{};
  ASSERT_EQ(arnm_init_arena_borrow(&borrowed, storage, sizeof(storage)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&borrowed), 64u);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 33, &borrowed), ARNM_SUCCESS); // 40 reserved
  EXPECT_EQ(arnm_arena_remaining(&borrowed), 24u);

  // letting a borrowed block go leaves the caller's buffer alone, and nothing to report
  arnm_release(&borrowed);
  EXPECT_EQ(arnm_arena_remaining(&borrowed), 0u);
}

TEST(MemoryTest, RemainingAnswersZeroWhereThereIsNoArenaToMeasure) {
  // nothing to ask
  EXPECT_EQ(arnm_arena_remaining(nullptr), 0u);

  // host mode has no ceiling of its own, so it reports none
  arnm host{};
  EXPECT_EQ(arnm_arena_remaining(&host), 0u);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &host), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&host), 0u);
  arnm_free(buffer, 32, &host);

  // an arena type without a buffer -- released, or never initialized
  arnm uninitialized{};
  ARNM_INTERN(&uninitialized)->allocation_type = ARNM_ALLOC_TYPE_ARENA_OWNED;
  EXPECT_EQ(arnm_arena_remaining(&uninitialized), 0u);

  arnm released{};
  ASSERT_EQ(arnm_init_arena(&released, 64), ARNM_SUCCESS);
  ASSERT_EQ(arnm_arena_remaining(&released), 64u);
  arnm_release(&released);
  EXPECT_EQ(arnm_arena_remaining(&released), 0u);
  // it still reads as an arena; the 0 is about the ground, not about the kind of handle
  EXPECT_TRUE(arnm_is_arena(&released));
}

TEST(MemoryTest, RemainingAndOverflowTotalReadTheSameLineFromBothSides) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 48, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&mem), 16u);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 0u);

  // what was refused is the reserved size of the request, not the part that did not fit
  uint8_t *refused = nullptr;
  EXPECT_EQ(arnm_alloc(&refused, 17, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(arnm_arena_remaining(&mem), 16u); // a refusal moves no index
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 24u);

  // reset clears the record and opens the ground again
  arnm_reset(&mem);
  EXPECT_EQ(arnm_arena_remaining(&mem), 64u);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 0u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arnm_alloc
// ---------------------------------------------------------------------------

TEST(MemoryTest, AllocRejectsBadArguments) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(nullptr, 8, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_alloc(&buffer, 0, &mem), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_alloc(&buffer, UINT32_MAX, &mem), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  // nothing of that touched the arena
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryTest, AllocOnUninitializedArenaReportsInvalidState) {
  // an arena type without a buffer can only come from writing the fields directly
  arnm mem{};
  ARNM_INTERN(&mem)->allocation_type = ARNM_ALLOC_TYPE_ARENA_OWNED;

  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_alloc(&buffer, 8, &mem), ARNM_ERROR_INVALID_STATE);
}

TEST(MemoryTest, ArenaHandsOutEightByteAlignedPointers) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  // odd sizes, so only the internal rounding can keep the addresses aligned
  for (uint32_t size : {1u, 3u, 7u, 9u, 13u, 17u}) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(arnm_alloc(&buffer, size, &mem), ARNM_SUCCESS) << "size " << size;
    EXPECT_EQ((uintptr_t)buffer % 8, 0u) << "size " << size;
    EXPECT_EQ(ARNM_INTERN(&mem)->last_index % 8, 0u) << "size " << size;
  }

  arnm_release(&mem);
}

TEST(MemoryTest, NullAllocatorMeansMalloc) {
  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, nullptr), ARNM_SUCCESS);
  ASSERT_TRUE(buffer);
  memset(buffer, 0x11, 32);

  ASSERT_EQ(arnm_realloc(&buffer, 32, 64, nullptr), ARNM_SUCCESS);
  ASSERT_TRUE(buffer);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(buffer[i], 0x11) << "at " << i; }

  EXPECT_EQ(arnm_free(buffer, 64, nullptr), ARNM_SUCCESS);
}

TEST(MemoryTest, DefaultModeBehavesLikeNullAllocator) {
  // a zeroed arnm is default mode
  arnm mem{};
  EXPECT_EQ(ARNM_INTERN(&mem)->allocation_type, ARNM_ALLOC_TYPE_DEFAULT);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 16, &mem), ARNM_SUCCESS);
  // default mode owns nothing collectively, so nothing is tracked
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 0u);

  EXPECT_EQ(arnm_free(buffer, 16, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arnm_free
// ---------------------------------------------------------------------------

TEST(MemoryTest, FreeNothingIsHarmless) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  // an empty buffer is not the arena's tail, so the arena reports it did not reclaim.
  EXPECT_EQ(arnm_free(nullptr, 0, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(arnm_free(nullptr, 32, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  // outside arena mode free(NULL) is simply a no-op
  EXPECT_EQ(arnm_free(nullptr, 32, nullptr), ARNM_SUCCESS);

  arnm_release(&mem);
}

TEST(MemoryTest, FreeReclaimsOnlyTheTail) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 64, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&second, 64, &mem), ARNM_SUCCESS);
  ASSERT_EQ(ARNM_INTERN(&mem)->last_index, 128u);

  // buried block: the bytes only come back on reset, and the call says so
  EXPECT_EQ(arnm_free(first, 64, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 128u);

  // the tail comes back, and then the block before it is the new tail
  EXPECT_EQ(arnm_free(second, 64, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);
  EXPECT_EQ(arnm_free(first, 64, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryTest, FreeUnwindsUnalignedSizesExactly) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  // 13 -> 16 reserved, 5 -> 8 reserved; freeing in reverse must land back on 0
  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 13, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&second, 5, &mem), ARNM_SUCCESS);
  ASSERT_EQ(ARNM_INTERN(&mem)->last_index, 24u);

  EXPECT_EQ(arnm_free(second, 5, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 16u);
  EXPECT_EQ(arnm_free(first, 13, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arnm_realloc
// ---------------------------------------------------------------------------

TEST(MemoryTest, ReallocRejectsBadArguments) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 16, &mem), ARNM_SUCCESS);

  EXPECT_EQ(arnm_realloc(nullptr, 16, 8, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_realloc(&buffer, 16, UINT32_MAX, &mem), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(arnm_realloc(&buffer, UINT32_MAX, 16, &mem), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  // none of that moved anything
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 16u);

  // same size is a no-op success
  EXPECT_EQ(arnm_realloc(&buffer, 16, 16, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 16u);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocToZeroFreesAndClearsThePointer) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(ARNM_INTERN(&mem)->last_index, 32u);

  EXPECT_EQ(arnm_realloc(&buffer, 32, 0, &mem), ARNM_SUCCESS);
  EXPECT_EQ(buffer, nullptr);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  // releasing nothing answers on arnm_free()'s terms, which in arena mode is the warning:
  // NULL is never the tail, so nothing was reclaimed. Same as calling arnm_free() directly.
  EXPECT_EQ(arnm_realloc(&buffer, 0, 0, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(arnm_free(nullptr, 0, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(buffer, nullptr);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocToZeroKeepsThePointerWhenNothingWasReleased) {
  // the release path answers on arnm_free()'s terms: the pointer is only cleared when
  // the bytes really came back, so a buried block stays addressable and says so
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_alloc(&tail, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = first;

  EXPECT_EQ(arnm_realloc(&first, 32, 0, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(first, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  // once it is the tail the very same call releases it and clears the pointer
  ASSERT_EQ(arnm_free(tail, 32, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_realloc(&first, 32, 0, &mem), ARNM_SUCCESS);
  EXPECT_EQ(first, nullptr);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocFromNullAllocates) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  // an empty buffer is not the arena's tail, so this goes down the fresh-block path
  uint8_t *buffer = nullptr;
  EXPECT_EQ(arnm_realloc(&buffer, 0, 32, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 32u);

  uint8_t *heap = nullptr;
  EXPECT_EQ(arnm_realloc(&heap, 0, 32, nullptr), ARNM_SUCCESS);
  ASSERT_TRUE(heap);
  EXPECT_EQ(arnm_free(heap, 32, nullptr), ARNM_SUCCESS);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocArenaTailShrinkReclaims) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 128, &mem), ARNM_SUCCESS);
  memset(buffer, 0xAB, 128);

  ASSERT_EQ(arnm_realloc(&buffer, 128, 32, &mem), ARNM_SUCCESS);
  // the tail is bumped back and the block keeps its address and contents
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 32u);
  EXPECT_EQ(buffer[31], 0xAB);

  // the released bytes are handed out again
  uint8_t *reused = nullptr;
  ASSERT_EQ(arnm_alloc(&reused, 96, &mem), ARNM_SUCCESS);
  EXPECT_EQ(reused, buffer + 32);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocArenaTailGrowsInPlace) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = buffer;
  memset(buffer, 0xCD, 32);

  ASSERT_EQ(arnm_realloc(&buffer, 32, 64, &mem), ARNM_SUCCESS);
  EXPECT_EQ(buffer, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);
  EXPECT_EQ(buffer[31], 0xCD);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocArenaGrowBeyondCapacityFails) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = buffer;

  EXPECT_EQ(arnm_realloc(&buffer, 32, 128, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  // the buffer is left untouched and stays usable at its old size
  EXPECT_EQ(buffer, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 32u);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 96u);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocArenaNonTailGrowMovesAndWarns) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 32, &mem), ARNM_SUCCESS);
  memset(first, 0x5A, 32);
  ASSERT_EQ(arnm_alloc(&tail, 32, &mem), ARNM_SUCCESS);

  uint8_t *before = first;
  // the resize happened, the abandoned block did not come back -- that is the warning
  EXPECT_EQ(arnm_realloc(&first, 32, 48, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_NE(first, before);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(first[i], 0x5A) << "at " << i; }
  // 32 + 32 abandoned + 48 rounded to 48
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 112u);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocArenaNonTailShrinkChangesNothing) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(arnm_alloc(&first, 64, &mem), ARNM_SUCCESS);
  memset(first, 0x77, 64);
  ASSERT_EQ(arnm_alloc(&tail, 32, &mem), ARNM_SUCCESS);

  uint8_t *before = first;
  uint32_t used_before = ARNM_INTERN(&mem)->last_index;

  EXPECT_EQ(arnm_realloc(&first, 64, 16, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  // a buried shrink cannot reclaim, so nothing at all moves
  EXPECT_EQ(first, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, used_before);
  EXPECT_EQ(first[63], 0x77);

  arnm_release(&mem);
}

TEST(MemoryTest, ReallocDefaultAllocatorResizes) {
  arnm mem{};

  uint8_t *buffer = nullptr;
  ASSERT_EQ(arnm_alloc(&buffer, 16, &mem), ARNM_SUCCESS);
  memset(buffer, 0x3C, 16);

  ASSERT_EQ(arnm_realloc(&buffer, 16, 64, &mem), ARNM_SUCCESS);
  ASSERT_TRUE(buffer);
  for (size_t i = 0; i < 16; ++i) { EXPECT_EQ(buffer[i], 0x3C) << "at " << i; }

  ASSERT_EQ(arnm_realloc(&buffer, 64, 8, &mem), ARNM_SUCCESS);
  EXPECT_EQ(buffer[7], 0x3C);

  EXPECT_EQ(arnm_free(buffer, 8, &mem), ARNM_SUCCESS);
  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arnm_clone
// ---------------------------------------------------------------------------

TEST(MemoryTest, CloneCopiesExactlyTheRequestedSize) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  const uint8_t source[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  uint8_t *copy = nullptr;

  EXPECT_EQ(arnm_clone(nullptr, source, 13, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_clone(&copy, nullptr, 13, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_clone(&copy, source, 0, &mem), ARNM_ERROR_INVALID_PARAM);

  ASSERT_EQ(arnm_clone(&copy, source, 13, &mem), ARNM_SUCCESS);
  ASSERT_TRUE(copy);
  EXPECT_EQ(memcmp(copy, source, 13), 0);
  // 13 asked for, 16 reserved
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 16u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// arnm_memory_block wrappers
// ---------------------------------------------------------------------------

TEST(MemoryBlockTest, RejectsNullDescriptors) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  arnm_memory_block block{};
  EXPECT_EQ(arnm_memory_block_alloc(nullptr, 8, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_memory_block_realloc(nullptr, 8, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_memory_block_free(nullptr, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_memory_block_clone(nullptr, &block, &mem), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_memory_block_clone(&block, nullptr, &mem), ARNM_ERROR_NULL_POINTER);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, FreeLeavesTheEmptyState) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  arnm_memory_block block{};
  ASSERT_EQ(arnm_memory_block_alloc(&block, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_memory_block_free(&block, &mem), ARNM_SUCCESS);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);

  // freeing an already empty block changes nothing; the arena reports it reclaimed nothing,
  // which is true, and the descriptor stays in the empty state
  EXPECT_EQ(arnm_memory_block_free(&block, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, FreeKeepsTheDescriptorWhenTheArenaKeepsTheBytes) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block first{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&first, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = first.data;

  // the descriptor is only reset when the bytes really came back; a buried block keeps
  // pointing at memory that is still valid until the arena resets
  EXPECT_EQ(arnm_memory_block_free(&first, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(first.data, before);
  EXPECT_EQ(first.size, 32u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  // once the block above it is gone, the same call reclaims and does reset
  ASSERT_EQ(arnm_memory_block_free(&tail, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_memory_block_free(&first, &mem), ARNM_SUCCESS);
  EXPECT_EQ(first.data, nullptr);
  EXPECT_EQ(first.size, 0u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, FreeAndReallocToZeroAgreeOnABuriedBlock) {
  // Both spellings mean "release this block" and must leave the same descriptor behind:
  // a block the arena would not take back keeps both its pointer and its size. A half
  // cleared {data, size: 0} could never be reclaimed afterwards, because a size of 0
  // never matches the arena tail.
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block viaFree{};
  arnm_memory_block viaRealloc{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&viaFree, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_memory_block_alloc(&viaRealloc, 32, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);
  uint8_t *freeBefore = viaFree.data;
  uint8_t *reallocBefore = viaRealloc.data;

  EXPECT_EQ(arnm_memory_block_free(&viaFree, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(viaFree.data, freeBefore);
  EXPECT_EQ(viaFree.size, 32u);

  EXPECT_EQ(
      arnm_memory_block_realloc(&viaRealloc, 0, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  EXPECT_EQ(viaRealloc.data, reallocBefore);
  EXPECT_EQ(viaRealloc.size, 32u);

  // neither gave anything back to the arena
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 96u);

  // and because both descriptors are intact, the blocks unwind properly once they are the tail
  ASSERT_EQ(arnm_memory_block_free(&tail, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_memory_block_realloc(&viaRealloc, 0, &mem), ARNM_SUCCESS);
  EXPECT_EQ(viaRealloc.data, nullptr);
  EXPECT_EQ(viaRealloc.size, 0u);
  EXPECT_EQ(arnm_memory_block_free(&viaFree, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocKeepsSizeAndPointerInStep) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block block{};
  ASSERT_EQ(arnm_memory_block_alloc(&block, 128, &mem), ARNM_SUCCESS);
  memset(block.data, 0xAB, block.size);
  uint8_t *before = block.data;

  ASSERT_EQ(arnm_memory_block_realloc(&block, 32, &mem), ARNM_SUCCESS);
  EXPECT_EQ(block.size, 32u);
  EXPECT_EQ(block.data, before);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 32u);

  // and the block is still consistent enough to free itself completely
  EXPECT_EQ(arnm_memory_block_free(&block, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocRecordsTheNewSizeOnTheArenaWarning) {
  // the regression this guards: a warning is not a failure, so the descriptor has to
  // follow the resize -- otherwise data points at the new block and size at the old one
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block first{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&first, 32, &mem), ARNM_SUCCESS);
  memset(first.data, 0x5A, first.size);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);

  uint8_t *before = first.data;
  EXPECT_EQ(arnm_memory_block_realloc(&first, 48, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  // the block could not grow in place, so it moved -- and the recorded size has to move with
  // it, or the caller cannot use the space it just paid for
  EXPECT_NE(first.data, before);
  EXPECT_EQ(first.size, 48u);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(first.data[i], 0x5A) << "at " << i; }

  // the descriptor is right, so freeing it as the tail actually reclaims
  EXPECT_EQ(arnm_memory_block_free(&first, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  arnm_release(&mem);
}

// The two buried-block resizes are where `size` earns its definition: it records what the
// block was allocated with, because that is the number arnm_free() is told later. Both tests
// check the descriptor *and* then prove it by reclaiming -- a descriptor that disagrees with
// the arena silently strands the block forever.

TEST(MemoryBlockTest, ReallocToZeroIsInterchangeableWithFree) {
  // Sweeps every allocator state a release can start from and requires the two spellings to
  // agree on all of it: return value, descriptor, and what the arena took back. The empty
  // arena case is the subtle one -- NULL is never the tail, so both must report the warning.
  enum Scenario { ARENA_TAIL, ARENA_BURIED, ARENA_EMPTY, HEAP_BLOCK, HEAP_EMPTY };
  const char *names[] = {"arena tail", "arena buried", "arena empty", "heap block", "heap empty"};

  for (int k = ARENA_TAIL; k <= HEAP_EMPTY; ++k) {
    arnm_result result[2];
    bool cleared[2];
    uint32_t size[2];
    uint32_t last_index[2];

    for (int op = 0; op < 2; ++op) { // 0 = _free, 1 = _realloc(0)
      arnm mem{};
      const bool arena = k <= ARENA_EMPTY;
      if (arena) { ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS) << names[k]; }

      arnm_memory_block victim{};
      arnm_memory_block tail{};
      if (k == ARENA_TAIL || k == ARENA_BURIED || k == HEAP_BLOCK) {
        ASSERT_EQ(arnm_memory_block_alloc(&victim, 32, &mem), ARNM_SUCCESS) << names[k];
      }
      if (k == ARENA_BURIED) {
        ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS) << names[k];
      }

      result[op] = op == 0 ? arnm_memory_block_free(&victim, &mem)
                           : arnm_memory_block_realloc(&victim, 0, &mem);
      cleared[op] = victim.data == nullptr;
      size[op] = victim.size;
      last_index[op] = ARNM_INTERN(&mem)->last_index;

      if (arena) {
        arnm_release(&mem);
      } else if (victim.data) {
        EXPECT_EQ(arnm_memory_block_free(&victim, &mem), ARNM_SUCCESS);
      }
    }

    EXPECT_EQ(result[0], result[1]) << names[k];
    EXPECT_EQ(cleared[0], cleared[1]) << names[k];
    EXPECT_EQ(size[0], size[1]) << names[k];
    EXPECT_EQ(last_index[0], last_index[1]) << names[k];
  }
}

TEST(MemoryBlockTest, ReallocBuriedGrowKeepsTheBlockReclaimable) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block victim{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&victim, 32, &mem), ARNM_SUCCESS);
  memset(victim.data, 0xA5, victim.size);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = victim.data;

  // buried, so the arena cannot extend in place: it takes a fresh 48 byte block and copies
  EXPECT_EQ(arnm_memory_block_realloc(&victim, 48, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_NE(victim.data, before);
  EXPECT_EQ(victim.size, 48u);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(victim.data[i], 0xA5) << "at " << i; }
  // 32 abandoned + 32 tail + 48 new
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 112u);

  // the caller really owns 48 usable bytes now, not the 32 it started with
  memset(victim.data, 0x3C, victim.size);
  EXPECT_EQ(victim.data[47], 0x3C);

  // and because size followed the move, the new block is reclaimable once it is the tail
  ASSERT_EQ(arnm_memory_block_free(&tail, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(arnm_memory_block_free(&victim, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocBuriedShrinkKeepsTheRecordedSize) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block victim{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&victim, 64, &mem), ARNM_SUCCESS);
  memset(victim.data, 0x77, victim.size);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = victim.data;

  // a buried shrink cannot reclaim, so literally nothing happens -- and the descriptor must
  // keep the size the arena reserved, not the smaller one the caller asked for
  EXPECT_EQ(arnm_memory_block_realloc(&victim, 16, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(victim.data, before);
  EXPECT_EQ(victim.size, 64u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 96u);
  EXPECT_EQ(victim.data[63], 0x77);

  // recording 16 here would have made the block permanently unreclaimable, because a size
  // that does not match the reservation never matches the arena tail
  ASSERT_EQ(arnm_memory_block_free(&tail, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_memory_block_free(&victim, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocBuriedShrinkThenRegrowStaysConsistent) {
  // the follow-up the recorded size protects: after a refused shrink the block is still 64,
  // so a later resize hands arnm_realloc the right old size
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block victim{};
  arnm_memory_block tail{};
  ASSERT_EQ(arnm_memory_block_alloc(&victim, 64, &mem), ARNM_SUCCESS);
  ASSERT_EQ(arnm_memory_block_alloc(&tail, 32, &mem), ARNM_SUCCESS);

  ASSERT_EQ(arnm_memory_block_realloc(&victim, 16, &mem), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  // free the tail: victim is the tail again, and still 64 bytes wide
  ASSERT_EQ(arnm_memory_block_free(&tail, &mem), ARNM_SUCCESS);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 64u);

  // now the shrink can be honoured in place, exactly by the 48 bytes it should be
  EXPECT_EQ(arnm_memory_block_realloc(&victim, 16, &mem), ARNM_SUCCESS);
  EXPECT_EQ(victim.size, 16u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 16u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocToZeroReleasesTheBlock) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 128), ARNM_SUCCESS);

  arnm_memory_block block{};
  ASSERT_EQ(arnm_memory_block_alloc(&block, 32, &mem), ARNM_SUCCESS);

  EXPECT_EQ(arnm_memory_block_realloc(&block, 0, &mem), ARNM_SUCCESS);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 0u);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, ReallocLeavesTheDescriptorAloneOnFailure) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 64), ARNM_SUCCESS);

  arnm_memory_block block{};
  ASSERT_EQ(arnm_memory_block_alloc(&block, 32, &mem), ARNM_SUCCESS);
  uint8_t *before = block.data;

  EXPECT_EQ(arnm_memory_block_realloc(&block, 128, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  // still usable at its previous size
  EXPECT_EQ(block.size, 32u);
  EXPECT_EQ(block.data, before);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, CloneCopiesContentAndSize) {
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 256), ARNM_SUCCESS);

  arnm_memory_block source{};
  ASSERT_EQ(arnm_memory_block_alloc(&source, 13, &mem), ARNM_SUCCESS);
  memset(source.data, 0x2B, source.size);

  arnm_memory_block copy{};
  ASSERT_EQ(arnm_memory_block_clone(&copy, &source, &mem), ARNM_SUCCESS);
  EXPECT_EQ(copy.size, source.size);
  EXPECT_NE(copy.data, source.data);
  EXPECT_EQ(memcmp(copy.data, source.data, source.size), 0);

  // a source and destination on different allocators is fine too
  arnm_memory_block heap_copy{};
  ASSERT_EQ(arnm_memory_block_clone(&heap_copy, &source, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(memcmp(heap_copy.data, source.data, source.size), 0);
  EXPECT_EQ(arnm_memory_block_free(&heap_copy, nullptr), ARNM_SUCCESS);

  arnm_release(&mem);
}

TEST(MemoryBlockTest, CloneLeavesDestinationAloneOnFailure) {
  // the descriptor must not claim a size it never got
  arnm mem{};
  ASSERT_EQ(arnm_init_arena(&mem, 32), ARNM_SUCCESS);

  uint8_t payload[64];
  memset(payload, 0x99, sizeof(payload));
  arnm_memory_block source = {payload, sizeof(payload)};

  arnm_memory_block copy{};
  EXPECT_EQ(arnm_memory_block_clone(&copy, &source, &mem), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(copy.data, nullptr);
  EXPECT_EQ(copy.size, 0u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// the pattern the wire decoders use: hand back the unused tail of a scratch block
// ---------------------------------------------------------------------------

TEST(MemoryBlockTest, ReleasesUnusedScratchTail) {
  alignas(8) uint8_t storage[256];
  arnm mem{};
  ASSERT_EQ(arnm_init_arena_borrow(&mem, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_memory_block keep{};
  ASSERT_EQ(arnm_memory_block_alloc(&keep, 16, &mem), ARNM_SUCCESS);

  // take everything that is left as scratch space, like the decoders do for pbtools
  arnm_memory_block scratch{};
  ASSERT_EQ(
      arnm_memory_block_alloc(
          &scratch, ARNM_INTERN(&mem)->capacity - ARNM_INTERN(&mem)->last_index, &mem
      ),
      ARNM_SUCCESS
  );
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, ARNM_INTERN(&mem)->capacity);

  // only the first 40 bytes were actually used
  ASSERT_EQ(arnm_memory_block_realloc(&scratch, 40, &mem), ARNM_SUCCESS);
  EXPECT_EQ(scratch.size, 40u);
  EXPECT_EQ(ARNM_INTERN(&mem)->last_index, 56u);

  // and the rest is available again
  arnm_memory_block after{};
  EXPECT_EQ(arnm_memory_block_alloc(&after, 128, &mem), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_overflow_total(&mem), 0u);

  arnm_release(&mem);
}

// ---------------------------------------------------------------------------
// the guard around the guards
// ---------------------------------------------------------------------------

#if defined(__linux__) && !defined(ARNM_TEST_SKIP_MEMORY_LIMIT)
#include <sys/resource.h>

TEST(TestMemoryLimit, IsInEffectForThisBinary) {
  // memory_limit.h caps this process so a runaway allocation dies here instead of taking the
  // machine down. If that ever stops working, everything else still passes and nobody notices
  // until the next 64 GB afternoon -- so check it directly.
  const char *env = std::getenv("ARNM_TEST_MEMORY_LIMIT_MB");
  if (env && std::string(env) == "0") { GTEST_SKIP() << "cap disabled via environment"; }

  rlimit limit{};
  ASSERT_EQ(getrlimit(RLIMIT_AS, &limit), 0);
  ASSERT_NE(limit.rlim_cur, RLIM_INFINITY) << "address space is uncapped";

  // and it actually bites: far more than any test legitimately needs
  void *huge = malloc(static_cast<size_t>(64) * 1024 * 1024 * 1024);
  EXPECT_EQ(huge, nullptr) << "a 64 GB request went through";
  free(huge);
}
#endif
