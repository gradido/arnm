#include "arnm/arena.h"
#include "arnm/bucket_vector.h"
#include "arnm/mono_timer.h"
#include <gtest/gtest.h>

#include "memory_intern.h"
#include "memory_limit.h"
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <vector>

namespace {
struct payload {
  uint64_t id;
  uint8_t blob[24];
};
} // namespace

/*
 * The bucket size moved out of the type and into _init, so the generated accessors say what a
 * vector holds and the exponent is passed alongside. Every vector is an arnm_bvec; these
 * constants are what the tests open theirs with, and what the invariant checks measure against.
 *
 * Tiny buckets on purpose: every test crosses bucket boundaries again and again.
 */
ARNM_BVEC_DEFINE(u32_vec, uint32_t)
ARNM_BVEC_DEFINE(pay_vec, payload)

constexpr uint8_t kU32Log2 = 3;
constexpr uint8_t kPayLog2 = 4;
/* smallest bucket _init admits: two elements, so nearly every push opens fresh ground. The
   old degenerate case of one element per bucket is refused outright now -- see InitRejects. */
constexpr uint8_t kOneLog2 = 1;
constexpr uint8_t kGrowDefault = 0; /* 0 asks for ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE */

constexpr uint32_t u32_vec_BUCKET_CAPACITY = 1u << kU32Log2;
constexpr uint32_t u32_vec_BUCKET_MASK = u32_vec_BUCKET_CAPACITY - 1u;
constexpr uint32_t u32_vec_BUCKET_SHIFT = kU32Log2;
constexpr uint32_t pay_vec_BUCKET_CAPACITY = 1u << kPayLog2;
constexpr uint32_t pay_vec_BUCKET_MASK = pay_vec_BUCKET_CAPACITY - 1u;
constexpr uint32_t one_vec_BUCKET_CAPACITY = 1u << kOneLog2;

namespace {

/**
 * Largest element count `_reserve` still admits for a payload of @p T.
 *
 * Beyond it either the payload would leave the allocator's uint32_t or rounding up to whole
 * buckets would wrap -- one bound covers both, and this mirrors it, so the tests can step over
 * the edge from either side.
 *
 * @param bucket_mask The generated `name##_BUCKET_MASK` of the container under test.
 */
template <typename T> constexpr uint32_t MaxReserve(uint32_t bucket_mask) {
  return static_cast<uint32_t>((UINT32_MAX - bucket_mask) / sizeof(T));
}

/**
 * Buckets the vector is holding, carrying elements or not.
 *
 * _bucket_count() answers the other question -- how many hold elements, which is the bound for
 * bucket wise iteration and drops back to 0 on _clear. What a reserve took and what a clear
 * keeps is this one, and it has no accessor because nothing outside a test needs it.
 */
uint16_t AllocatedBuckets(const arnm_bvec &v) {
  uint16_t held = 0;
  for (uint16_t i = 0; v.buckets && i < v.bucket_capacity; ++i) {
    if (v.buckets[i]) { ++held; }
  }
  return held;
}

/**
 * Verify the internal state a vector must always be in.
 *
 * One function rather than a template now: every vector is an arnm_bvec, and only the element
 * size and the bucket exponent differ. @p capacity is the bucket capacity it was opened with.
 */
void CheckInvariants(const arnm_bvec &v, uint32_t capacity) {
  // buckets holding elements, which is what tail_index is an index into
  const uint16_t in_use = u32_vec_bucket_count(&v);
  ASSERT_LE(in_use, v.bucket_capacity);
  if (v.bucket_capacity > 0) { ASSERT_NE(v.buckets, nullptr); }
  EXPECT_EQ(uint32_t{1} << v.bucket_capacity_max_log2, capacity);

  // a slot is either empty or holds a bucket no other slot holds. Reserved-but-unused buckets
  // sit past in_use and are just as much the vector's to keep distinct.
  std::set<const void *> distinct;
  for (uint16_t i = 0; v.buckets && i < v.bucket_capacity; ++i) {
    if (!v.buckets[i]) { continue; }
    ASSERT_TRUE(distinct.insert(v.buckets[i]).second) << "bucket " << i << " listed twice";
  }
  for (uint16_t i = 0; i < in_use; ++i) {
    ASSERT_NE(v.buckets[i], nullptr) << "bucket " << i << " is in use but empty";
  }

  if (v.size == 0) {
    // the empty marker is the missing tail; _init and _clear park tail_used at the capacity,
    // which is the same value a full tail carries and is what sends the next push to _grow
    EXPECT_EQ(v.tail, nullptr);
    EXPECT_EQ(v.tail_index, 0u);
    EXPECT_EQ(v.tail_used, capacity)
        << "tail_used " << v.tail_used << " is not the parked capacity";
    return;
  }
  ASSERT_NE(v.tail, nullptr);
  ASSERT_LT(v.tail_index, in_use);
  EXPECT_EQ(v.tail, v.buckets[v.tail_index]);
  EXPECT_GE(v.tail_used, 1u);
  EXPECT_LE(v.tail_used, capacity);
  // the one equation tying the three counters together
  EXPECT_EQ(v.size, v.tail_index * capacity + v.tail_used);
}

} // namespace

TEST(BucketVector, EmptyState) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(u32_vec_bucket_count(&v), 0u);
  EXPECT_EQ(u32_vec_front(&v), nullptr);
  EXPECT_EQ(u32_vec_back(&v), nullptr);
  EXPECT_EQ(u32_vec_at(&v, 0), nullptr);
  EXPECT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  u32_vec_free(&v);
}

TEST(BucketVector, InitNullPointer) {
  EXPECT_EQ(u32_vec_init(nullptr, kU32Log2, kGrowDefault, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(u32_vec_reserve(nullptr, 10), ARNM_ERROR_NULL_POINTER);
}

TEST(BucketVector, PushAndRandomAccess) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 1000; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i * 3)), ARNM_SUCCESS);
    ASSERT_EQ(u32_vec_size(&v), i + 1);
    EXPECT_EQ(*u32_vec_back(&v), static_cast<uint32_t>(i * 3));
    EXPECT_EQ(*u32_vec_front(&v), 0u);
  }
  for (uint32_t i = 0; i < 1000; ++i) {
    ASSERT_EQ(*u32_vec_at(&v, i), static_cast<uint32_t>(i * 3));
    ASSERT_EQ(*u32_vec_get(&v, i), static_cast<uint32_t>(i * 3));
  }
  EXPECT_EQ(u32_vec_at(&v, 1000), nullptr);
  EXPECT_EQ(u32_vec_bucket_count(&v), 1000u / u32_vec_BUCKET_CAPACITY);
  u32_vec_free(&v);
}

TEST(BucketVector, PointerStabilityAcrossGrowth) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_push(&v, 11u), ARNM_SUCCESS);
  uint32_t *first = u32_vec_at(&v, 0);
  for (uint32_t i = 0; i < 5000; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  EXPECT_EQ(first, u32_vec_at(&v, 0));
  EXPECT_EQ(*first, 11u);
  u32_vec_free(&v);
}

TEST(BucketVector, IterationInOrder) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 300; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i * 7)), ARNM_SUCCESS);
  }

  uint32_t seen = 0;
  for (uint32_t index = 0, count = u32_vec_size(&v); index < count; ++index) {
    const uint32_t *item = u32_vec_get(&v, index);
    ASSERT_NE(item, nullptr) << index;
    ASSERT_EQ(*item, static_cast<uint32_t>(index * 7));
    ++seen;
  }
  EXPECT_EQ(seen, 300u);

  uint32_t total = 0;
  for (uint32_t b = 0, buckets = u32_vec_bucket_count(&v); b < buckets; ++b) {
    const uint32_t *data = u32_vec_bucket_data(&v, b);
    const uint32_t count = u32_vec_bucket_size(&v, b);
    for (uint32_t k = 0; k < count; ++k) {
      ASSERT_EQ(data[k], static_cast<uint32_t>((total + k) * 7));
    }
    total += count;
  }
  EXPECT_EQ(total, 300u);
  u32_vec_free(&v);
}

TEST(BucketVector, PopDrainsAndRefills) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 500; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  for (uint32_t i = 500; i > 0; --i) {
    ASSERT_EQ(*u32_vec_back(&v), static_cast<uint32_t>(i - 1));
    ASSERT_EQ(u32_vec_size(&v), i);
    ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
  }
  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(u32_vec_back(&v), nullptr);
  EXPECT_EQ(u32_vec_bucket_count(&v), 0u);
  EXPECT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);

  // the drained buckets stay allocated and are taken up again
  const uint32_t buckets_before = AllocatedBuckets(v);
  for (uint32_t i = 0; i < 500; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  EXPECT_EQ(AllocatedBuckets(v), buckets_before);
  for (uint32_t i = 0; i < 500; ++i) ASSERT_EQ(*u32_vec_at(&v, i), static_cast<uint32_t>(i));
  u32_vec_free(&v);
}

TEST(BucketVector, PopOnBucketBoundary) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < u32_vec_BUCKET_CAPACITY; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  EXPECT_EQ(u32_vec_bucket_count(&v), 1u);

  ASSERT_EQ(u32_vec_push(&v, 99u), ARNM_SUCCESS); // opens the second bucket
  EXPECT_EQ(u32_vec_bucket_count(&v), 2u);
  ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS); // and falls back into the first
  EXPECT_EQ(u32_vec_bucket_count(&v), 1u);
  EXPECT_EQ(u32_vec_size(&v), static_cast<uint32_t>(u32_vec_BUCKET_CAPACITY));
  EXPECT_EQ(*u32_vec_back(&v), static_cast<uint32_t>(u32_vec_BUCKET_CAPACITY - 1));

  ASSERT_EQ(u32_vec_push(&v, 123u), ARNM_SUCCESS);
  EXPECT_EQ(*u32_vec_back(&v), 123u);
  EXPECT_EQ(u32_vec_bucket_count(&v), 2u);
  u32_vec_free(&v);
}

TEST(BucketVector, ClearKeepsBuckets) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 100; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  const uint32_t buckets_before = AllocatedBuckets(v);
  u32_vec_clear(&v);
  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(u32_vec_bucket_count(&v), 0u);
  EXPECT_EQ(AllocatedBuckets(v), buckets_before);
  ASSERT_EQ(u32_vec_push(&v, 5u), ARNM_SUCCESS);
  EXPECT_EQ(*u32_vec_front(&v), 5u);
  EXPECT_EQ(*u32_vec_back(&v), 5u);
  EXPECT_EQ(AllocatedBuckets(v), buckets_before);
  u32_vec_free(&v);
}

TEST(BucketVector, FreeIsIdempotent) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_push(&v, 1u), ARNM_SUCCESS);
  u32_vec_free(&v);
  u32_vec_free(&v);
  EXPECT_EQ(u32_vec_size(&v), 0u);
  ASSERT_EQ(u32_vec_push(&v, 2u), ARNM_SUCCESS); // usable again after free
  EXPECT_EQ(*u32_vec_front(&v), 2u);
  u32_vec_free(&v);
}

TEST(BucketVector, ReserveAllocatesUpFront) {
  arnm_bvec v;
  ASSERT_EQ(pay_vec_init(&v, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(pay_vec_reserve(&v, 500), ARNM_SUCCESS);
  const uint32_t expected = (500 + pay_vec_BUCKET_MASK) / pay_vec_BUCKET_CAPACITY;
  EXPECT_EQ(AllocatedBuckets(v), expected);
  EXPECT_EQ(pay_vec_size(&v), 0u);

  ASSERT_EQ(pay_vec_reserve(&v, 10), ARNM_SUCCESS); // never shrinks
  EXPECT_EQ(AllocatedBuckets(v), expected);

  for (uint32_t i = 0; i < 500; ++i) {
    payload *slot = nullptr;
    ASSERT_EQ(pay_vec_emplace(&v, &slot), ARNM_SUCCESS);
    slot->id = i;
    std::memset(slot->blob, static_cast<int>(i & 0xff), sizeof(slot->blob));
  }
  EXPECT_EQ(AllocatedBuckets(v), expected); // reserved buckets covered every push
  for (uint32_t i = 0; i < 500; ++i) {
    ASSERT_EQ(pay_vec_at(&v, i)->id, i);
    ASSERT_EQ(pay_vec_at(&v, i)->blob[0], static_cast<uint8_t>(i & 0xff));
  }
  pay_vec_free(&v);
}

TEST(BucketVector, PushPtrCopiesPayload) {
  arnm_bvec v;
  ASSERT_EQ(pay_vec_init(&v, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  payload p{};
  p.id = 777;
  std::memset(p.blob, 3, sizeof(p.blob));
  ASSERT_EQ(pay_vec_push_ptr(&v, &p), ARNM_SUCCESS);
  ASSERT_EQ(pay_vec_push(&v, p), ARNM_SUCCESS);
  p.id = 0; // source may change, the stored copies must not
  EXPECT_EQ(pay_vec_at(&v, 0)->id, 777u);
  EXPECT_EQ(pay_vec_at(&v, 1)->id, 777u);
  EXPECT_EQ(pay_vec_at(&v, 1)->blob[23], 3u);
  pay_vec_free(&v);
}

TEST(BucketVector, CopyTo) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 100; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i * 2)), ARNM_SUCCESS);
  }
  uint32_t flat[100];
  EXPECT_EQ(u32_vec_copy_to(&v, flat, 99), ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL);
  EXPECT_EQ(u32_vec_copy_to(&v, nullptr, 100), ARNM_ERROR_NULL_POINTER);
  ASSERT_EQ(u32_vec_copy_to(&v, flat, 100), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(flat[i], static_cast<uint32_t>(i * 2));
  u32_vec_free(&v);
}

TEST(BucketVector, ArenaAllocator) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 1024 * 1024), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(pay_vec_init(&v, kPayLog2, kGrowDefault, &arena), ARNM_SUCCESS);
  ASSERT_EQ(pay_vec_reserve(&v, 2000), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 2000; ++i) {
    payload p{};
    p.id = i * 2;
    ASSERT_EQ(pay_vec_push_ptr(&v, &p), ARNM_SUCCESS);
  }
  for (uint32_t i = 0; i < 2000; ++i) ASSERT_EQ(pay_vec_at(&v, i)->id, i * 2);
  pay_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVector, ExhaustedArenaReportsOutOfMemory) {
  alignas(8) uint8_t buffer[256];
  arnm small{};
  ASSERT_EQ(arnm_init_arena_borrow(&small, buffer, sizeof(buffer)), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &small), ARNM_SUCCESS);
  uint32_t pushed = 0;
  arnm_result result = ARNM_SUCCESS;
  while ((result = u32_vec_push(&v, static_cast<uint32_t>(pushed))) == ARNM_SUCCESS) ++pushed;
  EXPECT_EQ(result, ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_GT(pushed, 0u);
  EXPECT_EQ(u32_vec_size(&v), pushed); // the failed push left the vector untouched
  for (uint32_t i = 0; i < pushed; ++i) ASSERT_EQ(*u32_vec_at(&v, i), static_cast<uint32_t>(i));
  u32_vec_free(&v);
  arnm_release(&small);
}

// --- zero-initialized descriptors ------------------------------------------------------------
//
// `name v = {0};` is the C idiom for an empty aggregate, and reaching for it instead of _init
// must not be a trap: an all-zero descriptor has to be a usable empty vector.

TEST(BucketVectorZeroInit, ZeroedDescriptorReadsAsEmpty) {
  // A zeroed descriptor used to be a ready to use empty vector. The element size and the bucket
  // exponent live in the descriptor now, so a zeroed one has neither and can hold nothing. What
  // it must still do is answer every read path without reaching for storage it does not have.
  arnm_bvec v;
  std::memset(&v, 0, sizeof(v)); // strictly all bytes zero, whatever the padding

  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(u32_vec_bucket_count(&v), 0u);
  EXPECT_EQ(u32_vec_front(&v), nullptr);
  EXPECT_EQ(u32_vec_back(&v), nullptr);
  EXPECT_EQ(u32_vec_at(&v, 0), nullptr);
  EXPECT_EQ(u32_vec_at(&v, 12345), nullptr);
  EXPECT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  EXPECT_EQ(AllocatedBuckets(v), 0u);

  // an empty vector fits any destination, and copy_to must not read the buckets to find out
  uint32_t sink[8] = {0};
  EXPECT_EQ(u32_vec_copy_to(&v, sink, 1), ARNM_SUCCESS);
  EXPECT_EQ(sink[0], 0u);

  u32_vec_free(&v); // and giving nothing back is not a crash
}

TEST(BucketVectorZeroInit, ZeroedDescriptorRefusesEveryWrite) {
  // The other half of the contract: it stays empty. Without an element size there is no bucket
  // to open, and the refusal has to come back as a result rather than as a write through the
  // null tail. _init is what turns the descriptor into a vector, and it is not optional.
  arnm_bvec v;
  std::memset(&v, 0, sizeof(v));

  EXPECT_EQ(u32_vec_push(&v, 7u), ARNM_ERROR_INVALID_STATE);
  uint32_t *slot = reinterpret_cast<uint32_t *>(&v); // seeded, so a written NULL would show
  EXPECT_EQ(u32_vec_emplace(&v, &slot), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(u32_vec_reserve(&v, 10), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(v.buckets, nullptr);

  // and _init on the very same storage makes it work
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 300; ++i) { ASSERT_EQ(u32_vec_push(&v, i * 3), ARNM_SUCCESS); }
  for (uint32_t i = 0; i < 300; ++i) { ASSERT_EQ(*u32_vec_at(&v, i), i * 3); }
  EXPECT_EQ(*u32_vec_front(&v), 0u);
  EXPECT_EQ(*u32_vec_back(&v), 299u * 3);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVector, EmplaceReserveClearAndShrinkAllHold) {
  // Used to start from zeroed descriptors, which are inert now -- see BucketVectorZeroInit.
  // What it is really about survives: the four operations have to hold together on one vector.
  arnm_bvec v;
  ASSERT_EQ(pay_vec_init(&v, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  payload *slot = nullptr;
  ASSERT_EQ(pay_vec_emplace(&v, &slot), ARNM_SUCCESS);
  ASSERT_NE(slot, nullptr);
  slot->id = 4711;
  EXPECT_EQ(pay_vec_size(&v), 1u);
  EXPECT_EQ(pay_vec_back(&v)->id, 4711u);
  pay_vec_free(&v);

  arnm_bvec fresh;
  ASSERT_EQ(pay_vec_init(&fresh, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(pay_vec_reserve(&fresh, 100), ARNM_SUCCESS);
  EXPECT_GT(AllocatedBuckets(fresh), 0u);
  EXPECT_EQ(pay_vec_shrink(&fresh), ARNM_SUCCESS); // nothing pushed, so everything goes back
  EXPECT_EQ(AllocatedBuckets(fresh), 0u);
  pay_vec_clear(&fresh);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(fresh, pay_vec_BUCKET_CAPACITY));
  pay_vec_free(&fresh);

  arnm_bvec smallest; // two elements per bucket: every second push takes the cold path
  ASSERT_EQ(u32_vec_init(&smallest, kOneLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 50; ++i) { ASSERT_EQ(u32_vec_push(&smallest, i), ARNM_SUCCESS); }
  for (uint32_t i = 0; i < 50; ++i) { ASSERT_EQ(*u32_vec_at(&smallest, i), i); }
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(smallest, one_vec_BUCKET_CAPACITY));
  u32_vec_free(&smallest);
}

TEST(BucketVectorShrink, ReleasesTheBucketsPastTheLastElement) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 500; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  const uint32_t peak = AllocatedBuckets(v);

  for (int i = 0; i < 450; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
  EXPECT_EQ(AllocatedBuckets(v), peak); // popping alone never hands anything back

  const uint32_t *stable = u32_vec_at(&v, 7);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);

  const uint32_t used = (50 + u32_vec_BUCKET_MASK) / u32_vec_BUCKET_CAPACITY;
  EXPECT_EQ(AllocatedBuckets(v), used);
  EXPECT_EQ(v.bucket_capacity, used); // the index array is tightened along with the buckets
  EXPECT_LT(AllocatedBuckets(v), peak);
  EXPECT_EQ(u32_vec_size(&v), 50u);
  EXPECT_EQ(u32_vec_at(&v, 7), stable); // not one live element moved
  for (uint32_t i = 0; i < 50; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  // and the vector grows again from the tightened state
  for (uint32_t i = 50; i < 500; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 500; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorShrink, AfterClearHandsBackEverything) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 300; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  u32_vec_clear(&v);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);

  // an empty vector keeps no bucket and no index array at all
  EXPECT_EQ(v.buckets, nullptr);
  EXPECT_EQ(AllocatedBuckets(v), 0u);
  EXPECT_EQ(v.bucket_capacity, 0u);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  ASSERT_EQ(u32_vec_push(&v, 9u), ARNM_SUCCESS); // usable immediately afterwards
  EXPECT_EQ(*u32_vec_front(&v), 9u);
  EXPECT_EQ(*u32_vec_back(&v), 9u);
  u32_vec_free(&v);
}

TEST(BucketVectorShrink, DropsReservedButUntouchedBuckets) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_reserve(&v, 1000), ARNM_SUCCESS);
  ASSERT_GT(AllocatedBuckets(v), 0u);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS); // nothing was ever pushed into the reservation
  EXPECT_EQ(AllocatedBuckets(v), 0u);
  EXPECT_EQ(v.bucket_capacity, 0u);
  EXPECT_EQ(v.buckets, nullptr);

  // reserve again, fill a corner of it: only the untouched tail goes
  ASSERT_EQ(u32_vec_reserve(&v, 1000), ARNM_SUCCESS);
  const uint32_t reserved = AllocatedBuckets(v);
  for (uint32_t i = 0; i < 20; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  EXPECT_LT(AllocatedBuckets(v), reserved);
  EXPECT_EQ(AllocatedBuckets(v), u32_vec_bucket_count(&v));
  for (uint32_t i = 0; i < 20; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorShrink, IsIdempotentAndNullSafe) {
  EXPECT_EQ(u32_vec_shrink(nullptr), ARNM_ERROR_NULL_POINTER);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS); // on a vector that never allocated
  EXPECT_EQ(AllocatedBuckets(v), 0u);
  EXPECT_EQ(v.buckets, nullptr);

  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  const uint32_t buckets_after_first = AllocatedBuckets(v);
  const uint32_t capacity_after_first = v.bucket_capacity;
  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS); // the second pass finds nothing left to release
  EXPECT_EQ(AllocatedBuckets(v), buckets_after_first);
  EXPECT_EQ(v.bucket_capacity, capacity_after_first);
  EXPECT_EQ(u32_vec_size(&v), 100u);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  u32_vec_free(&v);
}

TEST(BucketVectorShrink, DefaultModeAllocatorReclaims) {
  // a arnm in default mode frees each block individually, so shrinking pays off -- and
  // this is the one path where the superseded index array is really handed back
  arnm heap{}; // zeroed is default mode: malloc/free

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &heap), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 600; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  const uint32_t peak = AllocatedBuckets(v);
  for (int i = 0; i < 590; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);

  ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  EXPECT_LT(AllocatedBuckets(v), peak);
  EXPECT_EQ(AllocatedBuckets(v), u32_vec_bucket_count(&v));
  EXPECT_EQ(v.bucket_capacity, AllocatedBuckets(v));
  for (uint32_t i = 0; i < 10; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);

  // growing again after the index array was replaced must not read the released one
  for (uint32_t i = 10; i < 600; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 600; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
  arnm_release(&heap);
}

TEST(BucketVectorShrink, ArenaReclaimsWhatItCanAndStopsThere) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 256 * 1024), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 400; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (int i = 0; i < 380; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);

  const uint32_t buckets_before = AllocatedBuckets(v);
  const uint32_t live = (20 + u32_vec_BUCKET_MASK) >> u32_vec_BUCKET_SHIFT;
  const uint32_t arena_before = ARNM_INTERN(&arena)->last_index;

  // An arena only gives back its most recent allocation, so _shrink unwinds from the top
  // and stops at the first bucket it cannot reclaim -- here the index array, which was
  // re-allocated part way through the growth and now sits between the buckets.
  EXPECT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  EXPECT_LT(AllocatedBuckets(v), buckets_before);
  EXPECT_GE(AllocatedBuckets(v), live);
  // whatever it released really came back
  EXPECT_LT(ARNM_INTERN(&arena)->last_index, arena_before);

  // and the vector is intact either way
  EXPECT_EQ(u32_vec_size(&v), 20u);
  for (uint32_t i = 0; i < 20; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);

  for (uint32_t i = 20; i < 400; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 400; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorShrink, ArenaTailBucketsComeBack) {
  // the clean case: nothing was allocated after the buckets, so every empty one is at the
  // arena tail when _shrink reaches it and the whole peak is handed back
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 256 * 1024), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_reserve(&v, 400), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 400; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (int i = 0; i < 380; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);

  const uint32_t arena_before = ARNM_INTERN(&arena)->last_index;
  const uint32_t live = (20 + u32_vec_BUCKET_MASK) >> u32_vec_BUCKET_SHIFT;

  EXPECT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  EXPECT_EQ(AllocatedBuckets(v), live);
  EXPECT_LT(ARNM_INTERN(&arena)->last_index, arena_before);

  EXPECT_EQ(u32_vec_size(&v), 20u);
  for (uint32_t i = 0; i < 20; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorShrink, RefusedTighteningKeepsPointerAndSize) {
  // A shrink the arena will not honour must change nothing at all. The recorded capacity is
  // the size the index array was allocated with, and it is what every later free and resize
  // has to be told -- a capacity that drifts below the real one strands the block for good.
  alignas(8) uint8_t storage[8192];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 200; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);

  void **index_before = v.buckets;
  const uint32_t capacity_before = v.bucket_capacity;
  const uint32_t buckets_before = AllocatedBuckets(v);

  // someone else takes the tail, so nothing the vector holds is the arena's last allocation
  uint8_t *blocker = nullptr;
  ASSERT_EQ(arnm_alloc(&blocker, 64, &arena), ARNM_SUCCESS);
  const uint32_t arena_with_blocker = ARNM_INTERN(&arena)->last_index;

  for (uint32_t i = 0; i < 190; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
  EXPECT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);

  // nothing came back, so nothing may have moved
  EXPECT_EQ(v.buckets, index_before);
  EXPECT_EQ(v.bucket_capacity, capacity_before);
  EXPECT_EQ(AllocatedBuckets(v), buckets_before);
  EXPECT_EQ(ARNM_INTERN(&arena)->last_index, arena_with_blocker);
  EXPECT_EQ(u32_vec_size(&v), 10u);
  for (uint32_t i = 0; i < 10; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorShrink, WhatTheArenaRefusedComesBackLater) {
  // The refusal is a matter of timing, not of ownership: once the block above is gone the
  // same buckets are reclaimable, and a second _shrink has to hand them over.
  alignas(8) uint8_t storage[8192];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 200; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);

  uint8_t *blocker = nullptr;
  ASSERT_EQ(arnm_alloc(&blocker, 64, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 190; ++i) ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);

  EXPECT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  const uint32_t buckets_refused = AllocatedBuckets(v);

  // the blocker goes, the tail is the vector's again
  ASSERT_EQ(arnm_free(blocker, 64, &arena), ARNM_SUCCESS);
  const uint32_t arena_before_retry = ARNM_INTERN(&arena)->last_index;

  EXPECT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
  EXPECT_LT(AllocatedBuckets(v), buckets_refused);
  EXPECT_LT(ARNM_INTERN(&arena)->last_index, arena_before_retry);

  EXPECT_EQ(u32_vec_size(&v), 10u);
  for (uint32_t i = 0; i < 10; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  // and it still grows from there
  for (uint32_t i = 10; i < 200; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 200; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorShrink, BuriedGrowthRecordsTheNewBlock) {
  // The other half of the warning: a buried index array cannot grow in place, so the arena
  // hands out a fresh block and copies. The resize did happen -- the descriptor has to follow
  // the new address and the new capacity, or every bucket pointer is read from stale memory.
  alignas(8) uint8_t storage[8192];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  // one bucket forces the first index array; from here every growth is buried behind buckets
  ASSERT_EQ(u32_vec_push(&v, 0u), ARNM_SUCCESS);
  void **first_index = v.buckets;

  for (uint32_t i = 1; i < 200; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  EXPECT_NE(v.buckets, first_index) << "the index array must have moved at least once";
  EXPECT_GE(v.bucket_capacity, AllocatedBuckets(v));
  for (uint32_t i = 0; i < 200; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorShrink, HoldsInvariantsThroughRandomShrinking) {
  std::mt19937 rng(777001u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint32_t> ref;

  for (int step = 0; step < 20000; ++step) {
    const int roll = pick(rng);
    if (roll < 55) {
      ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(step)), ARNM_SUCCESS);
      ref.push_back(static_cast<uint32_t>(step));
    } else if (roll < 85) {
      if (ref.empty()) {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
      } else {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
        ref.pop_back();
      }
    } else if (roll < 97) {
      ASSERT_EQ(u32_vec_shrink(&v), ARNM_SUCCESS);
      // after a shrink nothing beyond the used buckets survives, in either counter
      ASSERT_EQ(AllocatedBuckets(v), u32_vec_bucket_count(&v)) << "step " << step;
      ASSERT_EQ(v.bucket_capacity, AllocatedBuckets(v)) << "step " << step;
    } else {
      u32_vec_clear(&v);
      ref.clear();
    }
    ASSERT_EQ(u32_vec_size(&v), ref.size()) << "step " << step;
    ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY)) << "step " << step;
  }
  for (uint32_t i = 0; i < ref.size(); ++i) ASSERT_EQ(*u32_vec_at(&v, i), ref[i]) << "at " << i;
  u32_vec_free(&v);
}

// --- reference tests against std::vector ---------------------------------------------------

namespace {

/** Compare the whole sequence -- through every read path the API offers. */
void ExpectMatches(const arnm_bvec &v, const std::vector<uint32_t> &ref) {
  ASSERT_EQ(u32_vec_size(&v), ref.size());
  if (ref.empty()) {
    EXPECT_EQ(u32_vec_front(&v), nullptr);
    EXPECT_EQ(u32_vec_back(&v), nullptr);
    EXPECT_EQ(u32_vec_bucket_count(&v), 0u);
    return;
  }
  EXPECT_EQ(*u32_vec_front(&v), ref.front());
  EXPECT_EQ(*u32_vec_back(&v), ref.back());

  for (uint32_t i = 0; i < ref.size(); ++i) {
    ASSERT_EQ(*u32_vec_at(&v, i), ref[i]) << "at index " << i;
    ASSERT_EQ(*u32_vec_get(&v, i), ref[i]) << "at index " << i;
  }
  EXPECT_EQ(u32_vec_at(&v, ref.size()), nullptr);

  // bucket-wise traversal must yield the same sequence, and cover it exactly once
  std::vector<uint32_t> walked;
  walked.reserve(ref.size());
  for (uint32_t b = 0, buckets = u32_vec_bucket_count(&v); b < buckets; ++b) {
    const uint32_t *data = u32_vec_bucket_data(&v, b);
    const uint32_t count = u32_vec_bucket_size(&v, b);
    ASSERT_GE(count, 1u);
    ASSERT_LE(count, static_cast<uint32_t>(u32_vec_BUCKET_CAPACITY));
    walked.insert(walked.end(), data, data + count);
  }
  EXPECT_EQ(walked, ref);

  std::vector<uint32_t> flat(ref.size());
  ASSERT_EQ(u32_vec_copy_to(&v, flat.data(), flat.size()), ARNM_SUCCESS);
  EXPECT_EQ(flat, ref);
}

} // namespace

TEST(BucketVectorReference, RandomOperationSequence) {
  std::mt19937 rng(20260725u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint32_t> ref;

  for (int step = 0; step < 60000; ++step) {
    const int roll = pick(rng);
    if (roll < 55) { // push
      const uint32_t value = static_cast<uint32_t>(rng());
      ASSERT_EQ(u32_vec_push(&v, value), ARNM_SUCCESS);
      ref.push_back(value);
    } else if (roll < 85) { // pop
      if (ref.empty()) {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
      } else {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
        ref.pop_back();
      }
    } else if (roll < 95) { // spot check a random index
      if (!ref.empty()) {
        const uint32_t index = rng() % ref.size();
        ASSERT_EQ(*u32_vec_at(&v, index), ref[index]) << "step " << step;
      }
      ASSERT_EQ(u32_vec_at(&v, ref.size()), nullptr);
    } else if (roll < 98) { // reserve, must never disturb the content
      ASSERT_EQ(u32_vec_reserve(&v, ref.size() + (rng() % 500)), ARNM_SUCCESS);
    } else { // clear
      u32_vec_clear(&v);
      ref.clear();
    }

    ASSERT_EQ(u32_vec_size(&v), ref.size()) << "step " << step;
    if (step % 250 == 0) {
      ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY)) << "step " << step;
    }
    if (step % 2000 == 0) { ASSERT_NO_FATAL_FAILURE(ExpectMatches(v, ref)) << "step " << step; }
  }

  ASSERT_NO_FATAL_FAILURE(ExpectMatches(v, ref));
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorReference, PushHeavySequenceStaysInSync) {
  // push-dominated: drives the index array through many doublings
  std::mt19937 rng(4711u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint32_t> ref;

  for (int step = 0; step < 40000; ++step) {
    if (pick(rng) < 90) {
      const uint32_t value = static_cast<uint32_t>(step);
      ASSERT_EQ(u32_vec_push(&v, value), ARNM_SUCCESS);
      ref.push_back(value);
    } else if (!ref.empty()) {
      ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
      ref.pop_back();
    }
    ASSERT_EQ(u32_vec_size(&v), ref.size());
  }
  EXPECT_GT(ref.size(), 20000u);
  ASSERT_NO_FATAL_FAILURE(ExpectMatches(v, ref));
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorReference, SmallestBucketsMatchStdVector) {
  // The smallest bucket _init admits: two elements, so every second push opens fresh ground.
  // One element per bucket used to be reachable and is refused now -- see InitRejectsBadArguments.
  std::mt19937 rng(99991u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kOneLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint32_t> ref;

  for (int step = 0; step < 5000; ++step) {
    if (pick(rng) < 65) {
      const uint32_t value = static_cast<uint32_t>(rng());
      ASSERT_EQ(u32_vec_push(&v, value), ARNM_SUCCESS);
      ref.push_back(value);
    } else if (!ref.empty()) {
      ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
      ref.pop_back();
    }
    ASSERT_EQ(u32_vec_size(&v), ref.size());
    // buckets in use: the elements rounded up to whole buckets
    ASSERT_EQ(
        u32_vec_bucket_count(&v),
        (ref.size() + one_vec_BUCKET_CAPACITY - 1) / one_vec_BUCKET_CAPACITY
    );
  }
  for (uint32_t i = 0; i < ref.size(); ++i) ASSERT_EQ(*u32_vec_at(&v, i), ref[i]);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, one_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorReference, ArenaBackedSequence) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 4 * 1024 * 1024), ARNM_SUCCESS);

  // the arena has no realloc, so index growth takes the copy path here
  std::mt19937 rng(1312u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  std::vector<uint32_t> ref;

  for (int step = 0; step < 20000; ++step) {
    const int roll = pick(rng);
    if (roll < 70) {
      const uint32_t value = static_cast<uint32_t>(rng());
      ASSERT_EQ(u32_vec_push(&v, value), ARNM_SUCCESS);
      ref.push_back(value);
    } else if (roll < 95) {
      if (!ref.empty()) {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
        ref.pop_back();
      }
    } else {
      u32_vec_clear(&v);
      ref.clear();
    }
    ASSERT_EQ(u32_vec_size(&v), ref.size());
  }
  ASSERT_NO_FATAL_FAILURE(ExpectMatches(v, ref));
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  u32_vec_free(&v);
  arnm_release(&arena);
}

// --- extreme values ------------------------------------------------------------------------

TEST(BucketVectorLimits, ReserveZeroIsRefusedAndAllocatesNothing) {
  // 0 used to be an accepted no-op. It is a refusal now, which is the more useful reading: a
  // count computed to 0 is a caller's arithmetic going wrong, not a reservation of nothing.
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(u32_vec_reserve(&v, 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(AllocatedBuckets(v), 0u);
  EXPECT_EQ(v.bucket_capacity, 0u);
  EXPECT_EQ(v.buckets, nullptr);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  EXPECT_EQ(u32_vec_reserve(&v, 1), ARNM_SUCCESS);
  EXPECT_EQ(AllocatedBuckets(v), 1u);
  EXPECT_EQ(u32_vec_size(&v), 0u); // reserve never creates elements
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorLimits, ReserveRejectsCountOverflow) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(u32_vec_push(&v, 42u), ARNM_SUCCESS);

  // rounding up to whole buckets would overflow before the shift
  EXPECT_EQ(u32_vec_reserve(&v, UINT32_MAX), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(u32_vec_reserve(&v, UINT32_MAX - 1), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(
      u32_vec_reserve(&v, UINT32_MAX - u32_vec_BUCKET_MASK + 1), ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  // One bound serves both demands, and the payload half is the tighter one: that many
  // elements could never be addressed in uint32_t. Without it a reserve just under
  // UINT32_MAX would pass and start allocating hundreds of millions of buckets one at a time.
  EXPECT_EQ(u32_vec_reserve(&v, UINT32_MAX / 2), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(
      u32_vec_reserve(&v, MaxReserve<uint32_t>(u32_vec_BUCKET_MASK) + 1),
      ARNM_ERROR_ARITHMETIC_OVERFLOW
  );

  // the bound follows the payload size: the larger the element, the earlier it bites
  arnm_bvec big;
  ASSERT_EQ(pay_vec_init(&big, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(
      pay_vec_reserve(&big, MaxReserve<payload>(pay_vec_BUCKET_MASK) + 1),
      ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(pay_vec_reserve(&big, 64), ARNM_SUCCESS); // a sane count still goes through
  pay_vec_free(&big);

  // the rejected calls left the vector untouched and usable
  EXPECT_EQ(u32_vec_size(&v), 1u);
  EXPECT_EQ(*u32_vec_at(&v, 0), 42u);
  ASSERT_EQ(u32_vec_push(&v, 43u), ARNM_SUCCESS);
  EXPECT_EQ(*u32_vec_back(&v), 43u);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorLimits, ReserveHugeFailsWithoutDamage) {
  // Backed by a small arena on purpose: a request the guard lets through must still fail on
  // the allocator rather than run away. Against malloc this test would spend the machine's
  // RAM before returning.
  alignas(8) uint8_t storage[4096];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  const uint32_t buckets_before = AllocatedBuckets(v);

  // rejected by the guard, so the allocator is never asked for the impossible
  EXPECT_EQ(u32_vec_reserve(&v, UINT32_MAX - u32_vec_BUCKET_MASK), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(u32_vec_reserve(&v, UINT32_MAX / 3), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  // Both of these used to reach the allocator and fail there. The index array has its own
  // ceiling now and it is the tighter one, so the guard answers first -- worth pinning down,
  // because the difference is "refused before anything was asked for" either way but for a
  // different reason.
  EXPECT_EQ(
      u32_vec_reserve(&v, MaxReserve<uint32_t>(u32_vec_BUCKET_MASK)),
      ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(u32_vec_reserve(&v, 1u << 20), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  // The largest count the guard still lets through: exactly the buckets the index can address.
  // It is only ever exercised against a bounded arena -- asking malloc for it would spend real
  // memory before returning -- and there it fails on the allocator, as it should.
  constexpr uint32_t kLargestAllowed =
      ARNM_BVEC_MAX_INDEX_CAPACITY * u32_vec_BUCKET_CAPACITY;
  EXPECT_EQ(u32_vec_reserve(&v, kLargestAllowed), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(u32_vec_reserve(&v, kLargestAllowed + 1u), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  EXPECT_EQ(AllocatedBuckets(v), buckets_before);
  EXPECT_EQ(u32_vec_size(&v), 100u);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i);
  ASSERT_EQ(u32_vec_push(&v, 100u), ARNM_SUCCESS);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
  arnm_release(&arena);
}

TEST(BucketVectorLimits, IndexCapacityIsBoundedByItsOwnCounters) {
  // The ceiling moved: the index array used to be bounded by what the allocator would hand out
  // in one go, and is now bounded by the uint16_t the slot count is carried in and by the
  // uint16_t its byte size is derived through. Stated as the relation rather than as 8191, so
  // widening a counter moves the number and this test with it.
  static_assert(
      static_cast<uint64_t>(ARNM_BVEC_MAX_INDEX_CAPACITY) * sizeof(void *) <= UINT16_MAX,
      "the whole index array must be measurable in the uint16_t its size is derived through"
  );
  static_assert(
      ARNM_BVEC_MAX_INDEX_CAPACITY <= UINT16_MAX,
      "the slot count itself is a uint16_t"
  );

  // What the ceiling means for a caller: a reservation past it is refused, and refused before
  // anything is taken -- the vector is as usable afterwards as it was before.
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);

  // one element more than the buckets the index can ever address
  constexpr uint64_t kBeyond =
      static_cast<uint64_t>(ARNM_BVEC_MAX_INDEX_CAPACITY) * u32_vec_BUCKET_CAPACITY + 1u;
  ASSERT_LE(kBeyond, UINT32_MAX);
  EXPECT_EQ(
      u32_vec_reserve(&v, static_cast<uint32_t>(kBeyond)), ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(u32_vec_size(&v), 0u);
  EXPECT_EQ(v.buckets, nullptr); // refused before the allocator was ever asked
  CheckInvariants(v, u32_vec_BUCKET_CAPACITY);

  // and the vector still works
  ASSERT_EQ(u32_vec_push(&v, 42u), ARNM_SUCCESS);
  EXPECT_EQ(*u32_vec_back(&v), 42u);
  u32_vec_free(&v);

  // the allocator's own ceiling is still a ceiling, just no longer this one
  alignas(8) uint8_t storage[64];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
  uint8_t *const sentinel = storage;
  uint8_t *block = sentinel;
  EXPECT_EQ(arnm_alloc(&block, ARNM_MAX_ALLOC_SIZE + 1u, &arena), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(block, sentinel);
  EXPECT_EQ(arnm_alloc(&block, ARNM_MAX_ALLOC_SIZE, &arena), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(block, sentinel);
  arnm_release(&arena);
}

TEST(BucketVectorLimits, GrowAddsAFixedStepBelowTheCeiling) {
  // The index array grows by a named step now rather than by doubling, and the step is asked
  // for at init. Worth knowing which way the trade runs: a step regrows far more often than
  // doubling does, and behind an arena every regrowth strands the previous array -- so a
  // vector that knows its size should still _reserve rather than lean on the growth.
  constexpr uint8_t kStep = 4;
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kStep, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(v.index_grow_step_size, kStep);

  ASSERT_EQ(u32_vec_push(&v, 0u), ARNM_SUCCESS);
  EXPECT_EQ(v.bucket_capacity, kStep); // the first bucket brings the first index array

  uint32_t expected = kStep;
  for (uint32_t i = 1; i < 300u; ++i) {
    ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS) << "push " << i;
    if (AllocatedBuckets(v) > expected) { expected += kStep; }
    ASSERT_EQ(v.bucket_capacity, expected) << "push " << i;
    ASSERT_LE(v.bucket_capacity, ARNM_BVEC_MAX_INDEX_CAPACITY);
  }
  CheckInvariants(v, u32_vec_BUCKET_CAPACITY);
  u32_vec_free(&v);

  // 0 asks for the library's step rather than for "never grow"
  arnm_bvec defaulted;
  ASSERT_EQ(u32_vec_init(&defaulted, kU32Log2, 0, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(defaulted.index_grow_step_size, ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE);
  u32_vec_free(&defaulted);
}

TEST(BucketVectorLimits, RepeatedClearAndReserve) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  uint32_t buckets_high_water = 0;

  for (int round = 0; round < 200; ++round) {
    const uint32_t count = static_cast<uint32_t>(round % 50) * 7 + 1;
    ASSERT_EQ(u32_vec_reserve(&v, count), ARNM_SUCCESS);
    ASSERT_EQ(u32_vec_reserve(&v, count), ARNM_SUCCESS); // idempotent
    ASSERT_EQ(u32_vec_reserve(&v, 0), ARNM_ERROR_INVALID_PARAM); // 0 is not a reservation

    for (uint32_t i = 0; i < count; ++i) {
      ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
    }
    ASSERT_EQ(u32_vec_size(&v), count);
    ASSERT_EQ(*u32_vec_back(&v), static_cast<uint32_t>(count - 1));

    // buckets only ever accumulate, they are never given back by clear
    ASSERT_GE(AllocatedBuckets(v), buckets_high_water);
    buckets_high_water = AllocatedBuckets(v);

    u32_vec_clear(&v);
    u32_vec_clear(&v); // idempotent
    ASSERT_EQ(u32_vec_size(&v), 0u);
    ASSERT_EQ(AllocatedBuckets(v), buckets_high_water);
    ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY)) << "round " << round;
  }

  // the high water mark covers the largest round, nothing beyond it
  const uint32_t largest = 49u * 7u + 1u;
  EXPECT_EQ(buckets_high_water, (largest + u32_vec_BUCKET_MASK) / u32_vec_BUCKET_CAPACITY);
  u32_vec_free(&v);
}

TEST(BucketVectorLimits, ReserveKeepsExistingContent) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 37; ++i) ASSERT_EQ(u32_vec_push(&v, i * 5), ARNM_SUCCESS);
  const uint32_t *stable = u32_vec_at(&v, 12);

  ASSERT_EQ(u32_vec_reserve(&v, 10000), ARNM_SUCCESS);
  EXPECT_EQ(u32_vec_size(&v), 37u);
  EXPECT_EQ(u32_vec_at(&v, 12), stable); // reserve moves no payload
  for (uint32_t i = 0; i < 37; ++i) ASSERT_EQ(*u32_vec_at(&v, i), i * 5);
  EXPECT_EQ(u32_vec_at(&v, 37), nullptr); // reserved capacity is not content
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));
  u32_vec_free(&v);
}

TEST(BucketVectorLimits, FreeAfterHeavyUseResetsEverything) {
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 5000; ++i) ASSERT_EQ(u32_vec_push(&v, i), ARNM_SUCCESS);
  u32_vec_free(&v);

  EXPECT_EQ(v.buckets, nullptr);
  EXPECT_EQ(AllocatedBuckets(v), 0u);
  EXPECT_EQ(v.bucket_capacity, 0u);
  EXPECT_EQ(v.size, 0u);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY));

  // and the descriptor is immediately usable again, allocator setting intact
  ASSERT_EQ(u32_vec_push(&v, 1u), ARNM_SUCCESS);
  EXPECT_EQ(u32_vec_size(&v), 1u);
  u32_vec_free(&v);

  // The same has to hold for an attached arena: _free returns the descriptor to the empty
  // state _init writes, and the allocator has to survive that passage. A lost one would go
  // unnoticed with malloc -- here the next bucket has to come out of the caller's storage.
  alignas(8) uint8_t storage[4096];
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_bvec a;
  ASSERT_EQ(u32_vec_init(&a, kU32Log2, kGrowDefault, &arena), ARNM_SUCCESS);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(u32_vec_push(&a, i), ARNM_SUCCESS);
  u32_vec_free(&a);
  EXPECT_EQ(a.allocator, &arena);
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(a, u32_vec_BUCKET_CAPACITY));

  ASSERT_EQ(u32_vec_push(&a, 7u), ARNM_SUCCESS);
  const uintptr_t bucket = reinterpret_cast<uintptr_t>(a.buckets[0]);
  const uintptr_t base = reinterpret_cast<uintptr_t>(storage);
  EXPECT_GE(bucket, base);
  EXPECT_LT(bucket, base + sizeof(storage));
  u32_vec_free(&a);
  arnm_release(&arena);
}

// --- internal invariants over long operation sequences --------------------------------------

TEST(BucketVectorInvariants, HoldThroughLongMixedSequence) {
  std::mt19937 rng(20250101u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  uint32_t expected_size = 0;

  for (int step = 0; step < 30000; ++step) {
    const int roll = pick(rng);
    if (roll < 50) {
      ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(step)), ARNM_SUCCESS);
      ++expected_size;
    } else if (roll < 90) {
      if (expected_size) {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
        --expected_size;
      } else {
        ASSERT_EQ(u32_vec_pop(&v), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
      }
    } else if (roll < 97) {
      ASSERT_EQ(u32_vec_reserve(&v, expected_size + 40), ARNM_SUCCESS);
    } else {
      u32_vec_clear(&v);
      expected_size = 0;
    }

    ASSERT_EQ(u32_vec_size(&v), expected_size) << "step " << step;
    // checked on every single step: the counters must never drift, not even briefly
    ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, u32_vec_BUCKET_CAPACITY)) << "step " << step;

    // used buckets follow from the size alone
    const uint32_t used_buckets = (expected_size + u32_vec_BUCKET_MASK) / u32_vec_BUCKET_CAPACITY;
    ASSERT_EQ(u32_vec_bucket_count(&v), used_buckets) << "step " << step;
    ASSERT_LE(used_buckets, AllocatedBuckets(v));
  }
  u32_vec_free(&v);
}

TEST(BucketVectorInvariants, BucketBoundaryWalk) {
  // step back and forth across the same boundary many times
  arnm_bvec v;
  ASSERT_EQ(u32_vec_init(&v, kU32Log2, kGrowDefault, nullptr), ARNM_SUCCESS);
  const uint32_t capacity = u32_vec_BUCKET_CAPACITY;
  // three full buckets plus a single element in the fourth: sitting right on the boundary
  for (uint32_t i = 0; i <= capacity * 3; ++i) {
    ASSERT_EQ(u32_vec_push(&v, static_cast<uint32_t>(i)), ARNM_SUCCESS);
  }
  ASSERT_EQ(v.tail_index, 3u);
  ASSERT_EQ(v.tail_used, 1u);

  for (int round = 0; round < 500; ++round) {
    ASSERT_EQ(u32_vec_pop(&v), ARNM_SUCCESS);
    EXPECT_EQ(v.tail_used, capacity); // stepped back into the now-last full bucket
    EXPECT_EQ(v.tail_index, 2u);
    EXPECT_EQ(u32_vec_size(&v), capacity * 3);
    ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, capacity)) << "round " << round;

    ASSERT_EQ(u32_vec_push(&v, 7u), ARNM_SUCCESS);
    EXPECT_EQ(v.tail_used, 1u); // and forward into the reused bucket
    EXPECT_EQ(v.tail_index, 3u);
    EXPECT_EQ(*u32_vec_back(&v), 7u);
    ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, capacity)) << "round " << round;
  }
  EXPECT_EQ(AllocatedBuckets(v), 4u); // no bucket was allocated twice for the same slot
  u32_vec_free(&v);
}

TEST(BucketVectorInvariants, PayloadTypeHoldsTheSameInvariants) {
  std::mt19937 rng(864231u);
  std::uniform_int_distribution<int> pick(0, 99);

  arnm_bvec v;
  ASSERT_EQ(pay_vec_init(&v, kPayLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint64_t> ref;

  for (int step = 0; step < 10000; ++step) {
    if (pick(rng) < 60) {
      payload *slot = nullptr;
      ASSERT_EQ(pay_vec_emplace(&v, &slot), ARNM_SUCCESS);
      slot->id = static_cast<uint64_t>(step);
      std::memset(slot->blob, static_cast<int>(step & 0xff), sizeof(slot->blob));
      ref.push_back(static_cast<uint64_t>(step));
    } else if (!ref.empty()) {
      ASSERT_EQ(pay_vec_pop(&v), ARNM_SUCCESS);
      ref.pop_back();
    }
    ASSERT_EQ(pay_vec_size(&v), ref.size());
    if (step % 100 == 0) {
      ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, pay_vec_BUCKET_CAPACITY)) << "step " << step;
    }
  }
  for (uint32_t i = 0; i < ref.size(); ++i) {
    ASSERT_EQ(pay_vec_at(&v, i)->id, ref[i]) << "at index " << i;
    ASSERT_EQ(pay_vec_at(&v, i)->blob[0], static_cast<uint8_t>(ref[i] & 0xff));
  }
  ASSERT_NO_FATAL_FAILURE(CheckInvariants(v, pay_vec_BUCKET_CAPACITY));
  pay_vec_free(&v);
}

// --- performance comparison against the C++ standard containers -----------------------------
//
// These tests measure, they do not gate: the only assertions are on the computed results, so a
// slow or loaded machine can never turn them red. The timings are printed for reading, in the
// same build configuration the rest of the suite runs in.

// realistic bucket size for the comparison, named alongside the accessors as everywhere else
ARNM_BVEC_DEFINE(perf_vec, uint64_t)
constexpr uint8_t kPerfLog2 = 9;    // 512 * 8 B = 4 KiB
constexpr uint8_t kPerfPayLog2 = 7; // 128 * 32 B = 4 KiB

namespace {

constexpr uint32_t kPerfElements = 1000000;
constexpr int kPerfRepeats = 5;
/** Prime stride: long orbit through the whole range, no prefetchable pattern. */
constexpr uint32_t kPerfStride = 524287;

/**
 * Run @p fn a few times and keep the fastest -- the run least disturbed by the machine.
 *
 * One untimed warm-up runs first. Without it the first measurement carries the cost of the
 * heap growing into a size it has never held, which says more about the allocator's history
 * than about the container under test.
 */
template <typename Fn> double MeasureBestNs(Fn &&fn, uint32_t elements) {
  double best = -1.0;
  fn();
  for (int repeat = 0; repeat < kPerfRepeats; ++repeat) {
    arnm_mono_timer timer;
    arnm_mono_timer_reset(&timer);
    fn();
    const double ns =
        static_cast<double>(arnm_mono_timer_nanos(timer)) / static_cast<double>(elements);
    if (best < 0.0 || ns < best) best = ns;
  }
  return best;
}

void PrintTiming(const char *name, double nanos_per_element) {
  std::printf("  %-40s %7.2f ns/element\n", name, nanos_per_element);
}

/** Bytes an arena needs for @p elements uint64 plus the bucket index and alignment slack. */
constexpr uint32_t kPerfArenaBytes = kPerfElements * sizeof(uint64_t) + 1024 * 1024;

/** Sum every element of a prefilled bucket vector, bucket by bucket. */
uint64_t SumBuckets(const arnm_bvec &v) {
  uint64_t sum = 0;
  for (uint32_t b = 0, buckets = perf_vec_bucket_count(&v); b < buckets; ++b) {
    const uint64_t *data = perf_vec_bucket_data(&v, b);
    const uint32_t count = perf_vec_bucket_size(&v, b);
    for (uint32_t k = 0; k < count; ++k) sum += data[k];
  }
  return sum;
}

} // namespace

TEST(BucketVectorPerformance, Append) {
  arnm_mono_timer_init();
  const uint32_t n = kPerfElements;
  uint64_t sum_bvec = 0, sum_bvec_reserved = 0, sum_emplace = 0;
  uint64_t sum_arena = 0, sum_arena_fresh = 0;
  uint64_t sum_vector = 0, sum_vector_reserved = 0, sum_deque = 0, sum_array = 0;

  const double ns_bvec = MeasureBestNs(
      [&] {
        arnm_bvec v;
        perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr);
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&v, i);
        sum_bvec = SumBuckets(v);
        perf_vec_free(&v);
      },
      n
  );

  const double ns_bvec_reserved = MeasureBestNs(
      [&] {
        arnm_bvec v;
        perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr);
        perf_vec_reserve(&v, n);
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&v, i);
        sum_bvec_reserved = SumBuckets(v);
        perf_vec_free(&v);
      },
      n
  );

  const double ns_emplace = MeasureBestNs(
      [&] {
        arnm_bvec v;
        perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr);
        for (uint32_t i = 0; i < n; ++i) {
          uint64_t *slot = nullptr;
          if (perf_vec_emplace(&v, &slot) != ARNM_SUCCESS) break;
          *slot = i;
        }
        sum_emplace = SumBuckets(v);
        perf_vec_free(&v);
      },
      n
  );

  // arena: every bucket is bump-allocated from one contiguous block, _free is a no-op
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kPerfArenaBytes), ARNM_SUCCESS);
  const double ns_arena = MeasureBestNs(
      [&] {
        arnm_reset(&arena);
        arnm_bvec v;
        perf_vec_init(&v, kPerfLog2, kGrowDefault, &arena);
        perf_vec_reserve(&v, n);
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&v, i);
        sum_arena = SumBuckets(v);
        perf_vec_free(&v);
      },
      n
  );

  // the same, but paying for the arena itself on every run
  const double ns_arena_fresh = MeasureBestNs(
      [&] {
        arnm fresh{};
        arnm_init_arena(&fresh, kPerfArenaBytes);
        arnm_bvec v;
        perf_vec_init(&v, kPerfLog2, kGrowDefault, &fresh);
        perf_vec_reserve(&v, n);
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&v, i);
        sum_arena_fresh = SumBuckets(v);
        perf_vec_free(&v);
        arnm_release(&fresh);
      },
      n
  );

  const double ns_vector = MeasureBestNs(
      [&] {
        std::vector<uint64_t> v;
        for (uint32_t i = 0; i < n; ++i) v.push_back(i);
        sum_vector = 0;
        for (uint64_t value : v) sum_vector += value;
      },
      n
  );

  const double ns_vector_reserved = MeasureBestNs(
      [&] {
        std::vector<uint64_t> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; ++i) v.push_back(i);
        sum_vector_reserved = 0;
        for (uint64_t value : v) sum_vector_reserved += value;
      },
      n
  );

  const double ns_deque = MeasureBestNs(
      [&] {
        std::deque<uint64_t> d;
        for (uint32_t i = 0; i < n; ++i) d.push_back(i);
        sum_deque = 0;
        for (uint64_t value : d) sum_deque += value;
      },
      n
  );

  // std::array is fixed size -- no growth, no allocation, the floor this can be measured against
  const double ns_array = MeasureBestNs(
      [&] {
        auto a = std::make_unique<std::array<uint64_t, kPerfElements>>();
        for (uint32_t i = 0; i < n; ++i) (*a)[i] = i;
        sum_array = 0;
        for (uint64_t value : *a) sum_array += value;
      },
      n
  );

  std::printf("\nappend %" PRIu32 " elements (uint64, fill + sum)\n", n);
  PrintTiming("arnm bucket vector push", ns_bvec);
  PrintTiming("arnm bucket vector push, reserved", ns_bvec_reserved);
  PrintTiming("arnm bucket vector emplace", ns_emplace);
  PrintTiming("arnm bucket vector push, arena reset", ns_arena);
  PrintTiming("arnm bucket vector push, fresh arena", ns_arena_fresh);
  PrintTiming("std::vector push_back", ns_vector);
  PrintTiming("std::vector push_back, reserved", ns_vector_reserved);
  PrintTiming("std::deque push_back", ns_deque);
  PrintTiming("std::array indexed write", ns_array);

  // every container has to have produced the very same sequence
  const uint64_t expected = (static_cast<uint64_t>(n) - 1) * static_cast<uint64_t>(n) / 2;
  EXPECT_EQ(sum_bvec, expected);
  EXPECT_EQ(sum_bvec_reserved, expected);
  EXPECT_EQ(sum_emplace, expected);
  EXPECT_EQ(sum_arena, expected);
  EXPECT_EQ(sum_arena_fresh, expected);
  arnm_release(&arena);
  EXPECT_EQ(sum_vector, expected);
  EXPECT_EQ(sum_vector_reserved, expected);
  EXPECT_EQ(sum_deque, expected);
  EXPECT_EQ(sum_array, expected);
}

TEST(BucketVectorPerformance, AppendIntoWarmStorage) {
  // The append test above measures container *and* allocator: fresh buckets have to be taken
  // and their pages touched for the first time. Here every container already owns its memory
  // and has been written once, so what remains is the append path itself.
  arnm_mono_timer_init();
  const uint32_t n = kPerfElements;
  uint64_t sum_bvec = 0, sum_arena = 0, sum_vector = 0, sum_deque = 0;

  arnm_bvec v;
  ASSERT_EQ(perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(perf_vec_reserve(&v, n), ARNM_SUCCESS);
  for (uint32_t i = 0; i < n; ++i) ASSERT_EQ(perf_vec_push(&v, i), ARNM_SUCCESS);
  perf_vec_clear(&v); // keeps every bucket

  // arena-backed twin: buckets already taken, and they lie back to back in one block
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kPerfArenaBytes), ARNM_SUCCESS);
  arnm_bvec av;
  ASSERT_EQ(perf_vec_init(&av, kPerfLog2, kGrowDefault, &arena), ARNM_SUCCESS);
  ASSERT_EQ(perf_vec_reserve(&av, n), ARNM_SUCCESS);
  for (uint32_t i = 0; i < n; ++i) ASSERT_EQ(perf_vec_push(&av, i), ARNM_SUCCESS);
  perf_vec_clear(&av);

  std::vector<uint64_t> vec;
  vec.reserve(n);
  for (uint32_t i = 0; i < n; ++i) vec.push_back(i);
  vec.clear(); // keeps the capacity

  std::deque<uint64_t> deq;
  for (uint32_t i = 0; i < n; ++i) deq.push_back(i);
  deq.clear(); // keeps some of its chunks

  const double ns_bvec = MeasureBestNs(
      [&] {
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&v, i);
        sum_bvec = SumBuckets(v);
        perf_vec_clear(&v);
      },
      n
  );
  const double ns_arena = MeasureBestNs(
      [&] {
        for (uint32_t i = 0; i < n; ++i) perf_vec_push(&av, i);
        sum_arena = SumBuckets(av);
        perf_vec_clear(&av);
      },
      n
  );
  const double ns_vector = MeasureBestNs(
      [&] {
        for (uint32_t i = 0; i < n; ++i) vec.push_back(i);
        uint64_t sum = 0;
        for (uint64_t value : vec) sum += value;
        sum_vector = sum;
        vec.clear();
      },
      n
  );
  const double ns_deque = MeasureBestNs(
      [&] {
        for (uint32_t i = 0; i < n; ++i) deq.push_back(i);
        uint64_t sum = 0;
        for (uint64_t value : deq) sum += value;
        sum_deque = sum;
        deq.clear();
      },
      n
  );

  std::printf("\nappend %" PRIu32 " elements into storage already owned (fill + sum)\n", n);
  PrintTiming("arnm bucket vector push", ns_bvec);
  PrintTiming("arnm bucket vector push, arena", ns_arena);
  PrintTiming("std::vector push_back", ns_vector);
  PrintTiming("std::deque push_back", ns_deque);

  const uint64_t expected = (static_cast<uint64_t>(n) - 1) * static_cast<uint64_t>(n) / 2;
  EXPECT_EQ(sum_bvec, expected);
  EXPECT_EQ(sum_arena, expected);
  EXPECT_EQ(sum_vector, expected);
  EXPECT_EQ(sum_deque, expected);
  perf_vec_free(&v);
  perf_vec_free(&av);
  arnm_release(&arena);
}

TEST(BucketVectorPerformance, SequentialRead) {
  arnm_mono_timer_init();
  const uint32_t n = kPerfElements;

  arnm_bvec v;
  ASSERT_EQ(perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint64_t> vec;
  vec.reserve(n);
  std::deque<uint64_t> deq;
  auto arr = std::make_unique<std::array<uint64_t, kPerfElements>>();
  for (uint32_t i = 0; i < n; ++i) {
    ASSERT_EQ(perf_vec_push(&v, i), ARNM_SUCCESS);
    vec.push_back(i);
    deq.push_back(i);
    (*arr)[i] = i;
  }

  // arena-backed twin: same bucket layout, but the buckets lie back to back in one block
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kPerfArenaBytes), ARNM_SUCCESS);
  arnm_bvec av;
  ASSERT_EQ(perf_vec_init(&av, kPerfLog2, kGrowDefault, &arena), ARNM_SUCCESS);
  // reserved up front, and not only to save time: an arena cannot take a superseded index
  // array back, so growing it a step at a time would strand every earlier one in the arena
  ASSERT_EQ(perf_vec_reserve(&av, n), ARNM_SUCCESS);
  for (uint32_t i = 0; i < n; ++i) ASSERT_EQ(perf_vec_push(&av, i), ARNM_SUCCESS);

  uint64_t sum_buckets = 0, sum_arena = 0, sum_indexed = 0;
  uint64_t sum_vector = 0, sum_deque = 0, sum_array = 0;

  const double ns_buckets = MeasureBestNs([&] { sum_buckets = SumBuckets(v); }, n);
  const double ns_arena = MeasureBestNs([&] { sum_arena = SumBuckets(av); }, n);
  const double ns_indexed = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        for (uint32_t index = 0, count = perf_vec_size(&v); index < count; ++index) {
          sum += *perf_vec_get(&v, index);
        }
        sum_indexed = sum;
      },
      n
  );
  const double ns_vector = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        for (uint64_t value : vec) sum += value;
        sum_vector = sum;
      },
      n
  );
  const double ns_deque = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        for (uint64_t value : deq) sum += value;
        sum_deque = sum;
      },
      n
  );
  const double ns_array = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        for (uint64_t value : *arr) sum += value;
        sum_array = sum;
      },
      n
  );

  std::printf("\nsequential read of %" PRIu32 " elements\n", n);
  PrintTiming("arnm bucket vector, bucket wise", ns_buckets);
  PrintTiming("arnm bucket vector, arena, bucket wise", ns_arena);
  PrintTiming("arnm bucket vector, FOREACH", ns_indexed);
  PrintTiming("std::vector, range for", ns_vector);
  PrintTiming("std::deque, range for", ns_deque);
  PrintTiming("std::array, range for", ns_array);

  const uint64_t expected = (static_cast<uint64_t>(n) - 1) * static_cast<uint64_t>(n) / 2;
  EXPECT_EQ(sum_buckets, expected);
  EXPECT_EQ(sum_arena, expected);
  EXPECT_EQ(sum_indexed, expected);
  EXPECT_EQ(sum_vector, expected);
  EXPECT_EQ(sum_deque, expected);
  EXPECT_EQ(sum_array, expected);
  perf_vec_free(&v);
  perf_vec_free(&av);
  arnm_release(&arena);
}

TEST(BucketVectorPerformance, RandomAccess) {
  arnm_mono_timer_init();
  const uint32_t n = kPerfElements;

  arnm_bvec v;
  ASSERT_EQ(perf_vec_init(&v, kPerfLog2, kGrowDefault, nullptr), ARNM_SUCCESS);
  std::vector<uint64_t> vec;
  vec.reserve(n);
  std::deque<uint64_t> deq;
  auto arr = std::make_unique<std::array<uint64_t, kPerfElements>>();
  for (uint32_t i = 0; i < n; ++i) {
    ASSERT_EQ(perf_vec_push(&v, i), ARNM_SUCCESS);
    vec.push_back(i);
    deq.push_back(i);
    (*arr)[i] = i;
  }

  // arena-backed twin: the extra indirection stays, but the buckets are contiguous
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kPerfArenaBytes), ARNM_SUCCESS);
  arnm_bvec av;
  ASSERT_EQ(perf_vec_init(&av, kPerfLog2, kGrowDefault, &arena), ARNM_SUCCESS);
  // reserved up front, and not only to save time: an arena cannot take a superseded index
  // array back, so growing it a step at a time would strand every earlier one in the arena
  ASSERT_EQ(perf_vec_reserve(&av, n), ARNM_SUCCESS);
  for (uint32_t i = 0; i < n; ++i) ASSERT_EQ(perf_vec_push(&av, i), ARNM_SUCCESS);

  uint64_t sum_bvec = 0, sum_arena = 0, sum_vector = 0, sum_deque = 0, sum_array = 0;

  const double ns_bvec = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        uint32_t index = 0;
        for (uint32_t i = 0; i < n; ++i) {
          index = (index + kPerfStride) % n;
          sum += *perf_vec_get(&v, index);
        }
        sum_bvec = sum;
      },
      n
  );
  const double ns_arena = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        uint32_t index = 0;
        for (uint32_t i = 0; i < n; ++i) {
          index = (index + kPerfStride) % n;
          sum += *perf_vec_get(&av, index);
        }
        sum_arena = sum;
      },
      n
  );
  const double ns_vector = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        uint32_t index = 0;
        for (uint32_t i = 0; i < n; ++i) {
          index = (index + kPerfStride) % n;
          sum += vec[index];
        }
        sum_vector = sum;
      },
      n
  );
  const double ns_deque = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        uint32_t index = 0;
        for (uint32_t i = 0; i < n; ++i) {
          index = (index + kPerfStride) % n;
          sum += deq[index];
        }
        sum_deque = sum;
      },
      n
  );
  const double ns_array = MeasureBestNs(
      [&] {
        uint64_t sum = 0;
        uint32_t index = 0;
        for (uint32_t i = 0; i < n; ++i) {
          index = (index + kPerfStride) % n;
          sum += (*arr)[index];
        }
        sum_array = sum;
      },
      n
  );

  std::printf("\nrandom access, %" PRIu32 " scattered reads\n", n);
  PrintTiming("arnm bucket vector _get", ns_bvec);
  PrintTiming("arnm bucket vector _get, arena", ns_arena);
  PrintTiming("std::vector operator[]", ns_vector);
  PrintTiming("std::deque operator[]", ns_deque);
  PrintTiming("std::array operator[]", ns_array);

  // the stride is coprime to n, so each container is hit exactly once per element
  const uint64_t expected = (static_cast<uint64_t>(n) - 1) * static_cast<uint64_t>(n) / 2;
  EXPECT_EQ(sum_bvec, expected);
  EXPECT_EQ(sum_arena, expected);
  EXPECT_EQ(sum_vector, expected);
  EXPECT_EQ(sum_deque, expected);
  EXPECT_EQ(sum_array, expected);
  perf_vec_free(&v);
  perf_vec_free(&av);
  arnm_release(&arena);
}

TEST(BucketVectorPerformance, AppendLargePayload) {
  arnm_mono_timer_init();
  const uint32_t n = kPerfElements / 4;
  uint64_t sum_emplace = 0, sum_arena = 0, sum_push = 0, sum_vector = 0, sum_deque = 0;

  // 32 byte payload: here growth of a contiguous container means copying real weight
  const double ns_emplace = MeasureBestNs(
      [&] {
        arnm_bvec v;
        pay_vec_init(&v, kPerfPayLog2, kGrowDefault, nullptr);
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
          payload *slot = nullptr;
          if (pay_vec_emplace(&v, &slot) != ARNM_SUCCESS) break;
          std::memset(slot, 0, sizeof(*slot));
          slot->id = i;
        }
        for (uint32_t i = 0, held = pay_vec_size(&v); i < held; ++i) sum += pay_vec_at(&v, i)->id;
        sum_emplace = sum;
        pay_vec_free(&v);
      },
      n
  );

  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, n * sizeof(payload) + 1024 * 1024), ARNM_SUCCESS);
  const double ns_arena = MeasureBestNs(
      [&] {
        arnm_reset(&arena);
        arnm_bvec v;
        pay_vec_init(&v, kPerfPayLog2, kGrowDefault, &arena);
        // one index array instead of one per growth step: an arena keeps every superseded one,
        // and the stranded copies would outweigh the payload long before the run is over
        pay_vec_reserve(&v, n);
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
          payload *slot = nullptr;
          if (pay_vec_emplace(&v, &slot) != ARNM_SUCCESS) break;
          std::memset(slot, 0, sizeof(*slot));
          slot->id = i;
        }
        for (uint32_t i = 0, held = pay_vec_size(&v); i < held; ++i) sum += pay_vec_at(&v, i)->id;
        sum_arena = sum;
        pay_vec_free(&v);
      },
      n
  );

  const double ns_push = MeasureBestNs(
      [&] {
        arnm_bvec v;
        pay_vec_init(&v, kPerfPayLog2, kGrowDefault, nullptr);
        payload p;
        std::memset(&p, 0, sizeof(p));
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
          p.id = i;
          pay_vec_push(&v, p);
        }
        for (uint32_t i = 0, held = pay_vec_size(&v); i < held; ++i) sum += pay_vec_at(&v, i)->id;
        sum_push = sum;
        pay_vec_free(&v);
      },
      n
  );

  const double ns_vector = MeasureBestNs(
      [&] {
        std::vector<payload> v;
        payload p;
        std::memset(&p, 0, sizeof(p));
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
          p.id = i;
          v.push_back(p);
        }
        for (const payload &stored : v) sum += stored.id;
        sum_vector = sum;
      },
      n
  );

  const double ns_deque = MeasureBestNs(
      [&] {
        std::deque<payload> d;
        payload p;
        std::memset(&p, 0, sizeof(p));
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
          p.id = i;
          d.push_back(p);
        }
        for (const payload &stored : d) sum += stored.id;
        sum_deque = sum;
      },
      n
  );

  std::printf(
      "\nappend %" PRIu32 " elements of %zu byte payload (fill + sum)\n", n, sizeof(payload)
  );
  PrintTiming("arnm bucket vector emplace", ns_emplace);
  PrintTiming("arnm bucket vector emplace, arena", ns_arena);
  PrintTiming("arnm bucket vector push by value", ns_push);
  PrintTiming("std::vector push_back", ns_vector);
  PrintTiming("std::deque push_back", ns_deque);

  const uint64_t expected = (static_cast<uint64_t>(n) - 1) * static_cast<uint64_t>(n) / 2;
  EXPECT_EQ(sum_emplace, expected);
  EXPECT_EQ(sum_arena, expected);
  EXPECT_EQ(sum_push, expected);
  EXPECT_EQ(sum_vector, expected);
  EXPECT_EQ(sum_deque, expected);
  arnm_release(&arena);
}
