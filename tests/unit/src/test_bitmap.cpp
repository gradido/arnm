#include "arnm/bitmap.h"

#include "memory_limit.h"
#include <cstdint>
#include <gtest/gtest.h>

// What these check is the seam, not the arithmetic: the builtins underneath are the compiler's
// to get right. What is this header's own is that both spellings answer the same index for the
// same mask, whichever toolchain built them. The empty mask is deliberately not tested -- it is
// documented as undefined and the builtins underneath leave it that way, so a test could only
// pin down whatever this machine happens to do.

// promise: the answer is the index of the lowest set bit, counted from the least significant
// end, for every single bit mask a word can hold
TEST(Bitmap, NamesThePositionOfTheOnlyBitThatIsSet) {
  for (unsigned position = 0; position < 32u; ++position) {
    EXPECT_EQ(arnm_ctz(1u << position), static_cast<int>(position)) << "at " << position;
  }
  for (unsigned position = 0; position < 64u; ++position) {
    EXPECT_EQ(arnm_ctzll(1ull << position), static_cast<int>(position)) << "at " << position;
  }
}

// promise: bits above the lowest one do not move the answer, which is what lets a caller clear
// one bit at a time and walk a mask down to nothing
TEST(Bitmap, IgnoresEverythingAboveTheLowestSetBit) {
  EXPECT_EQ(arnm_ctz(0xfffffff0u), 4);
  EXPECT_EQ(arnm_ctz(UINT32_MAX), 0);
  EXPECT_EQ(arnm_ctz(1u << 31), 31);

  EXPECT_EQ(arnm_ctzll(UINT64_MAX), 0);
  EXPECT_EQ(arnm_ctzll(1ull << 63), 63);
  EXPECT_EQ(arnm_ctzll(0xffffffff00000000ull), 32)
      << "a mask whose whole low word is empty is where a 32 bit scan would answer wrongly";
}

// promise: walking a mask down by clearing its lowest bit visits every set bit once, in order.
// This is what the callers of this header actually do with it.
TEST(Bitmap, WalksAMaskDownOneBitAtATime) {
  uint64_t mask = 0b1001000100ull;
  const int expected[] = {2, 6, 9};
  for (unsigned step = 0; step < 3u; ++step) {
    ASSERT_NE(mask, 0ull) << "step " << step;
    EXPECT_EQ(arnm_ctzll(mask), expected[step]) << "step " << step;
    mask &= mask - 1u;
  }
  EXPECT_EQ(mask, 0ull) << "three bits, three steps";
}

// promise: the two widths agree wherever both can answer, so a caller that widens a mask does
// not have to re-check what the answer means
TEST(Bitmap, TheTwoWidthsAgreeOnEveryMaskBothCanHold) {
  const uint32_t masks[] = {1u, 2u, 3u, 0x80u, 0xff00u, 0x12345678u, 0x80000000u, UINT32_MAX};
  for (uint32_t mask : masks) {
    EXPECT_EQ(arnm_ctz(mask), arnm_ctzll(mask)) << "mask 0x" << std::hex << mask;
  }
}

// promise: the count is the number of set bits, for the masks whose answer is known by
// construction. The empty mask is tested here where the scans could not be: a count has an
// answer for it.
TEST(Bitmap, CountsTheBitsThatAreSet) {
  EXPECT_EQ(arnm_popcount(0u), 0);
  EXPECT_EQ(arnm_popcount(UINT32_MAX), 32);
  for (unsigned position = 0; position < 32u; ++position) {
    EXPECT_EQ(arnm_popcount(1u << position), 1) << "at " << position;
    // a mask of the bits below one is what turns a set bit into a dense index
    EXPECT_EQ(arnm_popcount((1u << position) - 1u), static_cast<int>(position))
        << "at " << position;
  }
  EXPECT_EQ(arnm_popcount(0b1001000100u), 3);
}

// promise: counting the bits below each set bit of a mask numbers them 0, 1, 2 ... in order --
// the dense index arnm_graded_arena_pool builds its grade lookup on
TEST(Bitmap, CountingBelowASetBitNumbersTheBitsDensely) {
  const uint32_t mask = (1u << 3) | (1u << 7) | (1u << 8) | (1u << 31);
  const unsigned positions[] = {3, 7, 8, 31};
  for (unsigned i = 0; i < 4u; ++i) {
    EXPECT_EQ(arnm_popcount(mask & ((1u << positions[i]) - 1u)), static_cast<int>(i))
        << "bit " << positions[i];
  }
  EXPECT_EQ(arnm_popcount(mask), 4);
}
