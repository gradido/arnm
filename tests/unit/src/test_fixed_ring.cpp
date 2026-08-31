#include "arnm/arena.h"
#include "arnm/fixed_ring.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

// A capacity that is not a power of two and not a divisor of anything the tests count in, so a
// wrap lands in the middle of a run rather than tidily at its edge.
namespace {

constexpr uint32_t kCapacity = 5;

/** A ring of uint32_t, the shape the session store's creation order queue has. */
ARNM_FIXED_RING_DEFINE(slot_ring, uint32_t)

/** Bigger than a machine word, to exercise the emplace path and the copies around it. */
struct Entry {
  uint64_t id;
  uint64_t due_ms;
  char tag[16];
};
ARNM_FIXED_RING_DEFINE(entry_ring, Entry)

/** Push @p count elements counting up from @p first, expecting every one to be taken. */
void PushRange(arnm_fixed_ring *ring, uint32_t first, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    ASSERT_EQ(slot_ring_push(ring, first + i), ARNM_SUCCESS) << "element " << i;
  }
}

/** Take everything the ring holds, front first, and hand the values back in that order. */
std::vector<uint32_t> DrainRing(arnm_fixed_ring *ring) {
  std::vector<uint32_t> taken;
  uint32_t value = 0;
  while (slot_ring_pop_copy(ring, &value) == ARNM_SUCCESS) { taken.push_back(value); }
  return taken;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(FixedRing, InitReservesEverythingUpFront) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);

  EXPECT_EQ(ring.capacity, kCapacity);
  EXPECT_EQ(ring.element_size, sizeof(uint32_t));
  EXPECT_EQ(ring.head, 0u);
  EXPECT_EQ(slot_ring_size(&ring), 0u);
  EXPECT_EQ(slot_ring_available(&ring), kCapacity);
  EXPECT_TRUE(slot_ring_is_empty(&ring));
  EXPECT_FALSE(slot_ring_is_full(&ring));

  // the whole footprint is known now and does not move afterwards
  const uint32_t reserved = slot_ring_reserved(&ring);
  EXPECT_EQ(reserved, kCapacity * sizeof(uint32_t));
  PushRange(&ring, 0, kCapacity);
  EXPECT_EQ(slot_ring_reserved(&ring), reserved);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, InitRefusesWhatItCannotHold) {
  arnm_fixed_ring ring;
  EXPECT_EQ(arnm_fixed_ring_init(nullptr, kCapacity, 4, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_init(&ring, 0, 4, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_fixed_ring_init(&ring, kCapacity, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
  // element_size lives in a uint16_t, so one past its range is a refusal and not a wrap
  EXPECT_EQ(
      arnm_fixed_ring_init(&ring, kCapacity, (size_t)UINT16_MAX + 1, nullptr),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(arnm_fixed_ring_init(&ring, kCapacity, UINT16_MAX, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_fixed_ring_free(&ring), ARNM_SUCCESS);

  // capacity times element size has to stay inside what the allocator counts in
  EXPECT_EQ(arnm_fixed_ring_init(&ring, UINT32_MAX, 8, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(FixedRing, InitLeavesTheDescriptorAloneWhenTheAllocatorRefuses) {
  alignas(8) uint8_t blob[64];
  arnm arena;
  ASSERT_EQ(arnm_init_arena_borrow(&arena, blob, sizeof(blob)), ARNM_SUCCESS);

  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, 4, &arena), ARNM_SUCCESS);

  // the arena has 48 bytes left; a second ring of 64 does not fit, and the descriptor it was
  // handed has to come out of that untouched -- field by field, because the padding between
  // them is nobody's to compare
  arnm_fixed_ring refused = ring;
  EXPECT_EQ(slot_ring_init(&refused, 16, &arena), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(refused.allocator, ring.allocator);
  EXPECT_EQ(refused.slots, ring.slots);
  EXPECT_EQ(refused.capacity, ring.capacity);
  EXPECT_EQ(refused.head, ring.head);
  EXPECT_EQ(refused.size, ring.size);
  EXPECT_EQ(refused.element_size, ring.element_size);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(FixedRing, FreeGivesTheBlockBackToTheArena) {
  alignas(8) uint8_t blob[256];
  arnm arena;
  ASSERT_EQ(arnm_init_arena_borrow(&arena, blob, sizeof(blob)), ARNM_SUCCESS);
  const uint32_t before = arnm_arena_remaining(&arena);

  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, &arena), ARNM_SUCCESS);
  EXPECT_LT(arnm_arena_remaining(&arena), before);

  // the ring is the arena's most recent allocation, so the bytes really come back
  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&arena), before);

  // and the descriptor is back where it started: nothing held, every write refused
  EXPECT_EQ(ring.slots, nullptr);
  EXPECT_EQ(slot_ring_capacity(&ring), 0u);
  EXPECT_EQ(slot_ring_reserved(&ring), 0u);
  uint32_t *slot = nullptr;
  EXPECT_EQ(slot_ring_emplace(&ring, &slot), ARNM_ERROR_NOT_INITIALIZED);

  // freeing again has nothing to give back and says so with a success
  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(FixedRing, FreeReportsWhatAnArenaCouldNotReclaim) {
  alignas(8) uint8_t blob[256];
  arnm arena;
  ASSERT_EQ(arnm_init_arena_borrow(&arena, blob, sizeof(blob)), ARNM_SUCCESS);

  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, &arena), ARNM_SUCCESS);

  // something else is at the tail now, so the ring's block is buried
  uint8_t *later = nullptr;
  ASSERT_EQ(arnm_alloc(&later, 32, &arena), ARNM_SUCCESS);

  // the operation happened, the memory did not come back -- neither success nor failure
  EXPECT_EQ(slot_ring_free(&ring), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(ring.slots, nullptr);

  arnm_release(&arena);
}

TEST(FixedRing, AZeroedDescriptorReadsAsEmptyAndRefusesEveryWrite) {
  arnm_fixed_ring ring;
  std::memset(&ring, 0, sizeof(ring));

  EXPECT_EQ(slot_ring_size(&ring), 0u);
  EXPECT_EQ(slot_ring_capacity(&ring), 0u);
  EXPECT_EQ(slot_ring_reserved(&ring), 0u);
  EXPECT_TRUE(slot_ring_is_empty(&ring));
  EXPECT_EQ(slot_ring_front(&ring), nullptr);
  EXPECT_EQ(slot_ring_back(&ring), nullptr);
  EXPECT_EQ(slot_ring_at(&ring, 0), nullptr);

  uint32_t *slot = nullptr;
  EXPECT_EQ(slot_ring_emplace(&ring, &slot), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(slot_ring_push(&ring, 1u), ARNM_ERROR_NOT_INITIALIZED);
  // holding nothing is the true answer to a pop, whatever the reason for it
  EXPECT_EQ(slot_ring_pop(&ring), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
}

TEST(FixedRing, NullIsAnswered) {
  uint32_t value = 0;
  void *slot = nullptr;
  EXPECT_EQ(arnm_fixed_ring_free(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_emplace(nullptr, &slot), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_push_ptr(nullptr, &value), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_pop(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_pop_copy(nullptr, &value), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_copy_to(nullptr, &value, 1), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_reserved(nullptr), 0u);
  arnm_fixed_ring_clear(nullptr); // a no-op, and reaching this line is the assertion

  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_fixed_ring_emplace(&ring, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_push_ptr(&ring, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_pop_copy(&ring, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_fixed_ring_copy_to(&ring, nullptr, 1), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(slot_ring_size(&ring), 0u); // and none of them added anything
  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// first in, first out
// ---------------------------------------------------------------------------

TEST(FixedRing, ElementsLeaveInTheOrderTheyArrived) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);

  PushRange(&ring, 10, kCapacity);
  EXPECT_EQ(slot_ring_size(&ring), kCapacity);
  EXPECT_EQ(*slot_ring_front(&ring), 10u);
  EXPECT_EQ(*slot_ring_back(&ring), 10u + kCapacity - 1);

  // indexed from the front, which is where the queue's own order is counted from
  for (uint32_t i = 0; i < kCapacity; ++i) { EXPECT_EQ(*slot_ring_at(&ring, i), 10u + i) << i; }
  EXPECT_EQ(slot_ring_at(&ring, kCapacity), nullptr);

  const std::vector<uint32_t> taken = DrainRing(&ring);
  ASSERT_EQ(taken.size(), kCapacity);
  for (uint32_t i = 0; i < kCapacity; ++i) { EXPECT_EQ(taken[i], 10u + i) << i; }
  EXPECT_TRUE(slot_ring_is_empty(&ring));

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, FullIsAnAnswerAndChangesNothing) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);
  PushRange(&ring, 0, kCapacity);

  EXPECT_TRUE(slot_ring_is_full(&ring));
  EXPECT_EQ(slot_ring_available(&ring), 0u);
  EXPECT_EQ(slot_ring_push(&ring, 999u), ARNM_ERROR_RESOURCE_EXHAUSTED);
  // the refusal keeps the oldest element rather than dropping it to make room
  EXPECT_EQ(slot_ring_size(&ring), kCapacity);
  EXPECT_EQ(*slot_ring_front(&ring), 0u);
  EXPECT_EQ(*slot_ring_back(&ring), kCapacity - 1);

  // room reopens the moment the front leaves, and the refused element then fits
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  EXPECT_EQ(slot_ring_push(&ring, 999u), ARNM_SUCCESS);
  EXPECT_EQ(*slot_ring_back(&ring), 999u);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, PopOnAnEmptyRingIsRefusedAndLeavesTheDestinationAlone) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);

  uint32_t value = 0xABCDEF01u;
  EXPECT_EQ(slot_ring_pop(&ring), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  EXPECT_EQ(slot_ring_pop_copy(&ring, &value), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  EXPECT_EQ(value, 0xABCDEF01u);
  EXPECT_EQ(slot_ring_front(&ring), nullptr);
  EXPECT_EQ(slot_ring_back(&ring), nullptr);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, ClearDropsEverythingAndKeepsTheBlock) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);
  PushRange(&ring, 0, kCapacity);
  const uint32_t reserved = slot_ring_reserved(&ring);

  slot_ring_clear(&ring);
  EXPECT_EQ(slot_ring_size(&ring), 0u);
  EXPECT_EQ(slot_ring_available(&ring), kCapacity);
  EXPECT_EQ(slot_ring_reserved(&ring), reserved); // the storage stays warm

  PushRange(&ring, 100, kCapacity);
  EXPECT_EQ(*slot_ring_front(&ring), 100u);
  EXPECT_EQ(slot_ring_reserved(&ring), reserved);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// the wrap
// ---------------------------------------------------------------------------

TEST(FixedRing, TheQueueSurvivesTheWrap) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);

  PushRange(&ring, 0, kCapacity);
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(ring.head, 3u);

  // these two go into the slots the first pops left, so the queue straddles the end
  ASSERT_EQ(slot_ring_push(&ring, 50u), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_push(&ring, 51u), ARNM_SUCCESS);
  EXPECT_EQ(slot_ring_size(&ring), 4u);

  // the front is still ahead of the back in the block, and the order is unaffected by it
  EXPECT_GT(slot_ring_front(&ring), slot_ring_back(&ring));
  const std::vector<uint32_t> taken = DrainRing(&ring);
  EXPECT_EQ(taken, (std::vector<uint32_t>{3u, 4u, 50u, 51u}));
  // draining took the front past the last slot, and it has to have come back to the first
  EXPECT_LT(ring.head, ring.capacity);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, TurningOverManyTimesCostsNoMemoryAndLosesNothing) {
  alignas(8) uint8_t blob[512];
  arnm arena;
  ASSERT_EQ(arnm_init_arena_borrow(&arena, blob, sizeof(blob)), ARNM_SUCCESS);

  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, &arena), ARNM_SUCCESS);
  const uint32_t remaining_after_init = arnm_arena_remaining(&arena);

  // three elements in flight while ten thousand pass through, the head wrapping two thousand
  // times over -- what a queue drained as fast as it is filled does, and it must sit still
  PushRange(&ring, 0, 3);
  for (uint32_t i = 3; i < 10000; ++i) {
    uint32_t oldest = 0;
    ASSERT_EQ(slot_ring_pop_copy(&ring, &oldest), ARNM_SUCCESS) << i;
    ASSERT_EQ(oldest, i - 3) << i;
    ASSERT_EQ(slot_ring_push(&ring, i), ARNM_SUCCESS) << i;
    ASSERT_EQ(slot_ring_size(&ring), 3u) << i;
    // asked every round rather than at the end: a front that walks past the block reads memory
    // that is not the ring's, and the diagnosis is worth more than the crash it becomes
    ASSERT_LT(ring.head, ring.capacity) << i;
  }
  EXPECT_EQ(arnm_arena_remaining(&arena), remaining_after_init);

  const std::vector<uint32_t> taken = DrainRing(&ring);
  EXPECT_EQ(taken, (std::vector<uint32_t>{9997u, 9998u, 9999u}));

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
  arnm_release(&arena);
}

// ---------------------------------------------------------------------------
// payloads bigger than a word
// ---------------------------------------------------------------------------

TEST(FixedRing, EmplaceBuildsTheElementWhereItWillLive) {
  arnm_fixed_ring ring;
  ASSERT_EQ(entry_ring_init(&ring, 3, nullptr), ARNM_SUCCESS);

  Entry *slot = nullptr;
  ASSERT_EQ(entry_ring_emplace(&ring, &slot), ARNM_SUCCESS);
  ASSERT_NE(slot, nullptr);
  // an element whose size is a multiple of 8 stays 8 byte aligned in a packed block
  EXPECT_EQ(reinterpret_cast<uintptr_t>(slot) % 8, 0u);
  slot->id = 7;
  slot->due_ms = 1234;
  std::snprintf(slot->tag, sizeof(slot->tag), "mail");

  // the ring counted the slot before handing it over, so it is already the back
  EXPECT_EQ(entry_ring_size(&ring), 1u);
  EXPECT_EQ(entry_ring_back(&ring), slot);
  EXPECT_EQ(entry_ring_front(&ring), slot);

  Entry taken;
  ASSERT_EQ(entry_ring_pop_copy(&ring, &taken), ARNM_SUCCESS);
  EXPECT_EQ(taken.id, 7u);
  EXPECT_EQ(taken.due_ms, 1234u);
  EXPECT_STREQ(taken.tag, "mail");
  EXPECT_TRUE(entry_ring_is_empty(&ring));

  EXPECT_EQ(entry_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, PushCopiesTheWholeElement) {
  arnm_fixed_ring ring;
  ASSERT_EQ(entry_ring_init(&ring, 2, nullptr), ARNM_SUCCESS);

  Entry first = {};
  first.id = 1;
  first.due_ms = 100;
  std::snprintf(first.tag, sizeof(first.tag), "first");
  ASSERT_EQ(entry_ring_push_ptr(&ring, &first), ARNM_SUCCESS);

  // the source may go out of scope right after; the ring holds a copy of its own
  first.id = 999;
  EXPECT_EQ(entry_ring_front(&ring)->id, 1u);
  EXPECT_STREQ(entry_ring_front(&ring)->tag, "first");

  EXPECT_EQ(entry_ring_free(&ring), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// copy_to
// ---------------------------------------------------------------------------

TEST(FixedRing, CopyToLaysTheWrappedLineOutStraight) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, kCapacity, nullptr), ARNM_SUCCESS);

  uint32_t dst[kCapacity + 1];
  std::memset(dst, 0xFF, sizeof(dst));

  // an empty ring copies nothing and succeeds, whatever the destination says
  EXPECT_EQ(slot_ring_copy_to(&ring, dst, 0), ARNM_SUCCESS);
  EXPECT_EQ(dst[0], 0xFFFFFFFFu);

  // one run: nothing has been popped yet, so the queue starts at slot 0
  PushRange(&ring, 0, kCapacity);
  EXPECT_EQ(slot_ring_copy_to(&ring, dst, kCapacity), ARNM_SUCCESS);
  for (uint32_t i = 0; i < kCapacity; ++i) { EXPECT_EQ(dst[i], i) << i; }

  // two runs: pop three and push two, and the queue straddles the end of the block
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_pop(&ring), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_push(&ring, 50u), ARNM_SUCCESS);
  ASSERT_EQ(slot_ring_push(&ring, 51u), ARNM_SUCCESS);

  std::memset(dst, 0xFF, sizeof(dst));
  EXPECT_EQ(slot_ring_copy_to(&ring, dst, kCapacity + 1), ARNM_SUCCESS);
  EXPECT_EQ(dst[0], 3u);
  EXPECT_EQ(dst[1], 4u);
  EXPECT_EQ(dst[2], 50u);
  EXPECT_EQ(dst[3], 51u);
  EXPECT_EQ(dst[4], 0xFFFFFFFFu); // nothing past the queue's own length is written

  // a destination one element short is refused, and refused before anything is written
  std::memset(dst, 0xFF, sizeof(dst));
  EXPECT_EQ(slot_ring_copy_to(&ring, dst, 3), ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL);
  EXPECT_EQ(dst[0], 0xFFFFFFFFu);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// what the mail queue and the session store actually do with it
// ---------------------------------------------------------------------------

TEST(FixedRing, ARingOfOneIsAHandover) {
  arnm_fixed_ring ring;
  ASSERT_EQ(slot_ring_init(&ring, 1, nullptr), ARNM_SUCCESS);

  ASSERT_EQ(slot_ring_push(&ring, 42u), ARNM_SUCCESS);
  EXPECT_TRUE(slot_ring_is_full(&ring));
  EXPECT_EQ(slot_ring_push(&ring, 43u), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(slot_ring_front(&ring), slot_ring_back(&ring));

  uint32_t value = 0;
  ASSERT_EQ(slot_ring_pop_copy(&ring, &value), ARNM_SUCCESS);
  EXPECT_EQ(value, 42u);
  EXPECT_TRUE(slot_ring_is_empty(&ring));
  // the single slot is handed straight back out
  EXPECT_EQ(slot_ring_push(&ring, 43u), ARNM_SUCCESS);
  EXPECT_EQ(*slot_ring_front(&ring), 43u);

  EXPECT_EQ(slot_ring_free(&ring), ARNM_SUCCESS);
}

TEST(FixedRing, TheFrontAloneSaysWhetherAnythingIsDue) {
  // every entry waits the same span, so arrival order is also due order and the earliest
  // deadline is at the front -- the property the retry ring leans on
  arnm_fixed_ring ring;
  ASSERT_EQ(entry_ring_init(&ring, 4, nullptr), ARNM_SUCCESS);

  constexpr uint64_t kDelayMs = 30000;
  for (uint64_t created = 100; created < 500; created += 100) {
    Entry entry = {};
    entry.id = created;
    entry.due_ms = created + kDelayMs;
    ASSERT_EQ(entry_ring_push_ptr(&ring, &entry), ARNM_SUCCESS);
  }

  const uint64_t now_ms = 30250;
  uint32_t due = 0;
  while (const Entry *head = entry_ring_front(&ring)) {
    if (head->due_ms > now_ms) { break; }
    ++due;
    ASSERT_EQ(entry_ring_pop(&ring), ARNM_SUCCESS);
  }
  EXPECT_EQ(due, 2u); // 30100 and 30200 are due, 30300 and 30400 are not
  EXPECT_EQ(entry_ring_front(&ring)->due_ms, 30300u);

  EXPECT_EQ(entry_ring_free(&ring), ARNM_SUCCESS);
}
