#include "arnm/byte_buffer.h"

#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

// What this buffer promises is narrow and mostly about what it does *not* do: it does not grow,
// it does not write half a record, and it does not confuse the room it took with the content it
// holds. The tests below are that list.

namespace {

/** The content of @p buffer as a string, read the way a caller reads it -- through access(). */
std::string Content(const arnm_byte_buffer *buffer) {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
  if (ARNM_SUCCESS != arnm_byte_buffer_access(buffer, &data, &size)) { return "<not initialized>"; }
  return std::string(reinterpret_cast<const char *>(data), size);
}

/** Append a literal without its terminator, which is how packed records go in. */
arnm_result Append(arnm_byte_buffer *buffer, const char *text) {
  return arnm_byte_buffer_copy(buffer, text, static_cast<uint32_t>(strlen(text)));
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

// promise: init reserves everything up front and starts empty
TEST(ByteBuffer, InitReservesTheWholeBlockAndHoldsNothing) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 64, nullptr), ARNM_SUCCESS);

  EXPECT_NE(buffer.data, nullptr);
  EXPECT_EQ(buffer.size, 64u);
  EXPECT_EQ(buffer.last_index, 0u);
  EXPECT_EQ(arnm_byte_buffer_available(&buffer), 64u);
  EXPECT_EQ(Content(&buffer), "");

  EXPECT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: a failed init leaves the descriptor untouched, so a buffer that was already usable
// stays usable and one that was not stays visibly not
TEST(ByteBuffer, InitRefusesWhatItCannotAnswerAndChangesNothing) {
  EXPECT_EQ(arnm_byte_buffer_init(nullptr, 8, nullptr), ARNM_ERROR_NULL_POINTER);

  arnm_byte_buffer buffer = {};
  EXPECT_EQ(arnm_byte_buffer_init(&buffer, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer.data, nullptr);
  EXPECT_EQ(buffer.size, 0u);

  arnm arena;
  ASSERT_EQ(arnm_init_arena(&arena, 64), ARNM_SUCCESS);
  EXPECT_EQ(arnm_byte_buffer_init(&buffer, 4096, &arena), ARNM_ERROR_OUT_OF_MEMORY)
      << "the arena is far too small for this and nothing partial may be kept";
  EXPECT_EQ(buffer.data, nullptr);
  arnm_release(&arena);
}

// promise: free gives the block back and leaves the empty state behind
TEST(ByteBuffer, FreeClearsTheDescriptorWhenTheBytesCameBack) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 32, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "something"), ARNM_SUCCESS);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(buffer.data, nullptr);
  EXPECT_EQ(buffer.size, 0u);
  EXPECT_EQ(buffer.last_index, 0u);
}

// promise: an arena that could not reclaim says so and the descriptor keeps pointing at memory
// that is still there -- the warning is the only way a caller learns the difference
TEST(ByteBuffer, FreeReportsWhatAnArenaCouldNotReclaim) {
  arnm arena;
  ASSERT_EQ(arnm_init_arena(&arena, 1024), ARNM_SUCCESS);

  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 64, &arena), ARNM_SUCCESS);
  uint8_t *buried_behind = nullptr;
  ASSERT_EQ(arnm_alloc(&buried_behind, 16, &arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_byte_buffer_free(&buffer, &arena), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_NE(buffer.data, nullptr) << "nothing came back, so nothing may be forgotten";
  EXPECT_EQ(buffer.size, 64u);

  arnm_release(&arena);
}

// promise: the empty state is answerable everywhere and never mistaken for a usable buffer
TEST(ByteBuffer, AZeroedBufferHoldsNothingAndSaysSo) {
  arnm_byte_buffer buffer = {};

  EXPECT_EQ(arnm_byte_buffer_available(&buffer), 0u);
  EXPECT_EQ(arnm_byte_buffer_copy(&buffer, "x", 1), ARNM_ERROR_NOT_INITIALIZED);

  const uint8_t *data = reinterpret_cast<const uint8_t *>("untouched");
  uint32_t size = 42;
  EXPECT_EQ(arnm_byte_buffer_access(&buffer, &data, &size), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(size, 42u) << "a failure leaves every output alone";

  EXPECT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS)
      << "the host is handed a NULL block, and free(NULL) is a no-op";

  arnm arena;
  ASSERT_EQ(arnm_init_arena(&arena, 64), ARNM_SUCCESS);
  EXPECT_EQ(arnm_byte_buffer_free(&buffer, &arena), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED)
      << "an arena has no tail at NULL to move back to";
  arnm_release(&arena);

  EXPECT_EQ(arnm_byte_buffer_available(nullptr), 0u);
  arnm_byte_buffer_clear(nullptr);
}

// ---------------------------------------------------------------------------
// filling it
// ---------------------------------------------------------------------------

// promise: records land back to back, with nothing between them and nothing after them
TEST(ByteBuffer, CopiesLandWhereTheLastOneEnded) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 64, nullptr), ARNM_SUCCESS);

  ASSERT_EQ(Append(&buffer, "{\"a\":1}"), ARNM_SUCCESS);
  EXPECT_EQ(buffer.last_index, 7u);
  ASSERT_EQ(Append(&buffer, "\n"), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "{\"b\":2}"), ARNM_SUCCESS);

  EXPECT_EQ(Content(&buffer), "{\"a\":1}\n{\"b\":2}");
  EXPECT_EQ(buffer.last_index, 15u);
  EXPECT_EQ(arnm_byte_buffer_available(&buffer), 64u - 15u);
  EXPECT_EQ(buffer.size, 64u) << "the room is what init took, whatever is written into it";

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: the source is read here and nothing of it is remembered, so a caller may reuse the
// memory it copied from the moment the call returns
TEST(ByteBuffer, TakesACopyAndKeepsNoPointerToTheSource) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 32, nullptr), ARNM_SUCCESS);

  char scratch[8];
  memcpy(scratch, "first", 5);
  ASSERT_EQ(arnm_byte_buffer_copy(&buffer, scratch, 5), ARNM_SUCCESS);
  memcpy(scratch, "SECOND", 6);
  ASSERT_EQ(arnm_byte_buffer_copy(&buffer, scratch, 6), ARNM_SUCCESS);

  EXPECT_EQ(Content(&buffer), "firstSECOND");
  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: bytes that are not text go in unchanged -- this copies, it does not interpret
TEST(ByteBuffer, CopiesArbitraryBytesIncludingZeros) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 16, nullptr), ARNM_SUCCESS);

  const uint8_t raw[] = {0x00, 0xff, 0x00, 0x7f};
  ASSERT_EQ(arnm_byte_buffer_copy(&buffer, raw, sizeof(raw)), ARNM_SUCCESS);

  const uint8_t *data = nullptr;
  uint32_t size = 0;
  ASSERT_EQ(arnm_byte_buffer_access(&buffer, &data, &size), ARNM_SUCCESS);
  ASSERT_EQ(size, sizeof(raw));
  EXPECT_EQ(memcmp(data, raw, sizeof(raw)), 0);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: the last byte is usable, and the one after it is refused rather than grown into
TEST(ByteBuffer, FillsToTheLastByteAndThenRefuses) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 8, nullptr), ARNM_SUCCESS);

  ASSERT_EQ(Append(&buffer, "1234"), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "5678"), ARNM_SUCCESS) << "an exact fit is a fit";
  EXPECT_EQ(arnm_byte_buffer_available(&buffer), 0u);

  EXPECT_EQ(Append(&buffer, "9"), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(buffer.last_index, 8u);
  EXPECT_EQ(Content(&buffer), "12345678");
  EXPECT_EQ(buffer.size, 8u) << "a full buffer refuses, it does not reach for more memory";

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: a record that does not fit writes nothing at all -- not the part that would have
// fit, because a truncated record in a packed stream cannot be told from a whole one
TEST(ByteBuffer, WritesNothingWhenTheWholeRecordDoesNotFit) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 8, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "abcde"), ARNM_SUCCESS);

  EXPECT_EQ(Append(&buffer, "XXXX"), ARNM_ERROR_RESOURCE_EXHAUSTED)
      << "three bytes are free and four were offered";
  EXPECT_EQ(buffer.last_index, 5u);
  EXPECT_EQ(Content(&buffer), "abcde");

  ASSERT_EQ(Append(&buffer, "fgh"), ARNM_SUCCESS) << "the refusal left the buffer usable";
  EXPECT_EQ(Content(&buffer), "abcdefgh");

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: a size near the top of a uint32_t is refused by the bound and not by a wrap, which
// is the one arithmetic this buffer has to get right
TEST(ByteBuffer, ASizeThatWouldWrapIsStillJustTooLarge) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 8, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "abcd"), ARNM_SUCCESS);

  const char source[4] = {'w', 'x', 'y', 'z'};
  EXPECT_EQ(arnm_byte_buffer_copy(&buffer, source, UINT32_MAX), ARNM_ERROR_RESOURCE_EXHAUSTED)
      << "last_index + size would wrap to a number that fits; nothing may be copied";
  EXPECT_EQ(buffer.last_index, 4u);
  EXPECT_EQ(Content(&buffer), "abcd");

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: the arguments a copy needs are the ones it checks
TEST(ByteBuffer, CopyRefusesArgumentsItCannotUse) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 16, nullptr), ARNM_SUCCESS);

  EXPECT_EQ(arnm_byte_buffer_copy(nullptr, "x", 1), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_byte_buffer_copy(&buffer, nullptr, 1), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_byte_buffer_copy(&buffer, "x", 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer.last_index, 0u);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: a pushed byte lands exactly where a copy of one byte would have, and the two
// interleave without a seam -- push is the same append, spelled for a single byte
TEST(ByteBuffer, PushAppendsOneByteWhereACopyWouldHavePutIt) {
  arnm_byte_buffer pushed;
  arnm_byte_buffer copied;
  ASSERT_EQ(arnm_byte_buffer_init(&pushed, 32, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_init(&copied, 32, nullptr), ARNM_SUCCESS);

  const char *record = "{\"a\":1}";
  ASSERT_EQ(Append(&pushed, record), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_push(&pushed, '\n'), ARNM_SUCCESS);
  ASSERT_EQ(Append(&pushed, record), ARNM_SUCCESS);

  ASSERT_EQ(Append(&copied, record), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_copy(&copied, "\n", 1), ARNM_SUCCESS);
  ASSERT_EQ(Append(&copied, record), ARNM_SUCCESS);

  EXPECT_EQ(Content(&pushed), Content(&copied));
  EXPECT_EQ(pushed.last_index, copied.last_index);
  EXPECT_EQ(pushed.last_index, 15u);

  ASSERT_EQ(arnm_byte_buffer_free(&pushed, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_free(&copied, nullptr), ARNM_SUCCESS);
}

// promise: every byte value goes in unchanged, the ones above 0x7F included. That is what the
// uint8_t parameter is for: with a char one, half of these are a narrowing conversion at the
// call site and gcc says so under -Wconversion.
TEST(ByteBuffer, PushTakesEveryByteValueUnchanged) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 256, nullptr), ARNM_SUCCESS);

  for (int value = 0; value <= 255; ++value) {
    ASSERT_EQ(arnm_byte_buffer_push(&buffer, static_cast<uint8_t>(value)), ARNM_SUCCESS)
        << "byte " << value;
  }
  EXPECT_EQ(buffer.last_index, 256u);

  const uint8_t *data = nullptr;
  uint32_t size = 0;
  ASSERT_EQ(arnm_byte_buffer_access(&buffer, &data, &size), ARNM_SUCCESS);
  ASSERT_EQ(size, 256u);
  for (int value = 0; value <= 255; ++value) {
    ASSERT_EQ(data[value], static_cast<uint8_t>(value)) << "at " << value;
  }

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: the last byte is usable and the one after it is refused, with nothing written and
// the mark where it was -- the same all or nothing a copy promises
TEST(ByteBuffer, PushFillsTheLastByteAndThenRefuses) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 4, nullptr), ARNM_SUCCESS);

  ASSERT_EQ(Append(&buffer, "abc"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_push(&buffer, 'd'), ARNM_SUCCESS) << "the last byte is a byte";
  EXPECT_EQ(arnm_byte_buffer_available(&buffer), 0u);

  EXPECT_EQ(arnm_byte_buffer_push(&buffer, 'e'), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(buffer.last_index, 4u);
  EXPECT_EQ(Content(&buffer), "abcd");

  // and a copy of one byte agrees with it, which is the invariant the two share
  EXPECT_EQ(arnm_byte_buffer_copy(&buffer, "e", 1), ARNM_ERROR_RESOURCE_EXHAUSTED);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: push refuses what it cannot use, exactly where copy does
TEST(ByteBuffer, PushRefusesABufferItCannotWriteTo) {
  EXPECT_EQ(arnm_byte_buffer_push(nullptr, 'x'), ARNM_ERROR_NULL_POINTER);

  arnm_byte_buffer empty = {};
  EXPECT_EQ(arnm_byte_buffer_push(&empty, 'x'), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(empty.last_index, 0u);
}

// ---------------------------------------------------------------------------
// the unsafe pair
// ---------------------------------------------------------------------------

// promise: the unsafe calls write what the safe ones write, byte for byte and mark for mark.
// They are the same append with the questions asked somewhere else, not a different one.
TEST(ByteBuffer, TheUnsafePairWritesExactlyWhatTheSafePairWrites) {
  arnm_byte_buffer safe;
  arnm_byte_buffer unsafe;
  ASSERT_EQ(arnm_byte_buffer_init(&safe, 256, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_init(&unsafe, 256, nullptr), ARNM_SUCCESS);

  const uint8_t record[5] = {0x00, 0xff, 0x41, 0x7f, 0x80};
  for (int round = 0; round < 8; ++round) {
    ASSERT_EQ(arnm_byte_buffer_copy(&safe, record, sizeof(record)), ARNM_SUCCESS);
    ASSERT_EQ(arnm_byte_buffer_push(&safe, static_cast<uint8_t>(round)), ARNM_SUCCESS);

    // the shape the unsafe pair is written for: one question for the whole group
    ASSERT_GE(arnm_byte_buffer_available(&unsafe), sizeof(record) + 1u);
    unsafe_arnm_byte_buffer_copy(&unsafe, record, sizeof(record));
    unsafe_arnm_byte_buffer_push(&unsafe, static_cast<uint8_t>(round));
  }

  EXPECT_EQ(safe.last_index, unsafe.last_index);
  EXPECT_EQ(Content(&safe), Content(&unsafe));

  // and the last byte either of them may write is the last byte of the block, not one past it
  arnm_byte_buffer edge;
  ASSERT_EQ(arnm_byte_buffer_init(&edge, 4, nullptr), ARNM_SUCCESS);
  unsafe_arnm_byte_buffer_copy(&edge, "abc", 3);
  unsafe_arnm_byte_buffer_push(&edge, 'd');
  EXPECT_EQ(Content(&edge), "abcd");
  EXPECT_EQ(arnm_byte_buffer_available(&edge), 0u);

  ASSERT_EQ(arnm_byte_buffer_free(&safe, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_free(&unsafe, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_free(&edge, nullptr), ARNM_SUCCESS);
}

#ifndef NDEBUG
// What the unsafe pair asserts is exactly what the safe pair refuses, so these walk that list.
// They are the only tests here that cannot run with NDEBUG defined -- a release build takes the
// checks out, which is the whole point of them -- so the whole block is compiled out there
// rather than left to fail.

TEST(ByteBufferDeathTest, UnsafeCopyChecksWhatTheSafeCopyRefuses) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 8, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "abcde"), ARNM_SUCCESS);

  EXPECT_DEATH(unsafe_arnm_byte_buffer_copy(&buffer, "XXXX", 4), "room for")
      << "three bytes are free and four were offered";
  EXPECT_DEATH(unsafe_arnm_byte_buffer_copy(&buffer, "X", 0), "size is 0");
  EXPECT_DEATH(unsafe_arnm_byte_buffer_copy(&buffer, nullptr, 1), "src is NULL");
  EXPECT_DEATH(unsafe_arnm_byte_buffer_copy(nullptr, "X", 1), "buffer is NULL");

  arnm_byte_buffer empty = {};
  EXPECT_DEATH(unsafe_arnm_byte_buffer_copy(&empty, "X", 1), "holds no block");

  // the parent process is untouched by any of that, and the buffer still works
  EXPECT_EQ(Content(&buffer), "abcde");
  ASSERT_EQ(Append(&buffer, "fgh"), ARNM_SUCCESS);
  EXPECT_EQ(Content(&buffer), "abcdefgh");
  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

TEST(ByteBufferDeathTest, UnsafePushChecksWhatTheSafePushRefuses) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 4, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "abcd"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_byte_buffer_available(&buffer), 0u);

  EXPECT_DEATH(unsafe_arnm_byte_buffer_push(&buffer, 'e'), "the buffer is full");
  EXPECT_DEATH(unsafe_arnm_byte_buffer_push(nullptr, 'e'), "buffer is NULL");

  arnm_byte_buffer empty = {};
  EXPECT_DEATH(unsafe_arnm_byte_buffer_push(&empty, 'e'), "holds no block");

  EXPECT_EQ(Content(&buffer), "abcd");
  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}
#endif // NDEBUG

// ---------------------------------------------------------------------------
// reading it back out
// ---------------------------------------------------------------------------

// promise: access answers the content length and not the allocated one, and the pointer is the
// start of the block -- the pair a write() call is handed
TEST(ByteBuffer, AccessAnswersTheContentAndNotTheRoom) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 128, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(Append(&buffer, "packed"), ARNM_SUCCESS);

  const uint8_t *data = nullptr;
  uint32_t size = 0;
  ASSERT_EQ(arnm_byte_buffer_access(&buffer, &data, &size), ARNM_SUCCESS);
  EXPECT_EQ(data, buffer.data);
  EXPECT_EQ(size, 6u);
  EXPECT_NE(size, buffer.size);

  EXPECT_EQ(arnm_byte_buffer_access(&buffer, nullptr, &size), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_byte_buffer_access(&buffer, &data, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_byte_buffer_access(nullptr, &data, &size), ARNM_ERROR_NULL_POINTER);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: an initialized but empty buffer is a success with a length of 0, so a caller that
// flushes on a timer needs no special case for a tick in which nothing happened
TEST(ByteBuffer, AnEmptyBufferIsSomethingToWriteZeroBytesOf) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 16, nullptr), ARNM_SUCCESS);

  const uint8_t *data = nullptr;
  uint32_t size = 7;
  EXPECT_EQ(arnm_byte_buffer_access(&buffer, &data, &size), ARNM_SUCCESS);
  EXPECT_EQ(data, buffer.data);
  EXPECT_EQ(size, 0u);

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}

// promise: clear drops the content and keeps the block, which is what makes a fill / write out /
// clear cycle cost exactly one allocation in total
TEST(ByteBuffer, ClearKeepsTheRoomAndTheAddress) {
  arnm_byte_buffer buffer;
  ASSERT_EQ(arnm_byte_buffer_init(&buffer, 16, nullptr), ARNM_SUCCESS);
  const uint8_t *const block = buffer.data;

  for (int round = 0; round < 3; ++round) {
    ASSERT_EQ(Append(&buffer, "round"), ARNM_SUCCESS) << "round " << round;
    EXPECT_EQ(Content(&buffer), "round");
    arnm_byte_buffer_clear(&buffer);
    EXPECT_EQ(buffer.last_index, 0u);
    EXPECT_EQ(arnm_byte_buffer_available(&buffer), 16u);
    EXPECT_EQ(buffer.data, block) << "the block itself never moves";
  }

  ASSERT_EQ(arnm_byte_buffer_free(&buffer, nullptr), ARNM_SUCCESS);
}
