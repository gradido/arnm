#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// The writer is opaque from here on purpose: these tests only ever see what a consumer sees.
// Two things are checked over and over. That a struct written field by field comes out as the
// text it should, byte for byte -- and that arnm_json_writer_size() knew that length before a
// byte of it existed.

namespace {

/** Arena large enough for every document below, the working room of a write included. */
constexpr uint32_t kArenaCapacity = 256 * 1024;

/** A writer over an arena, torn down in the right order at the end of a scope. */
class ArenaWriter {
public:
  explicit ArenaWriter(arnm_json_write_flags flags = ARNM_JSON_WRITE_DEFAULT) {
    EXPECT_EQ(arnm_init_arena(&arena_, kArenaCapacity), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_writer_init(&writer_, &arena_, flags), ARNM_SUCCESS);
  }
  ~ArenaWriter() {
    arnm_json_writer_release(&writer_);
    arnm_release(&arena_);
  }
  ArenaWriter(const ArenaWriter &) = delete;
  ArenaWriter &operator=(const ArenaWriter &) = delete;

  arnm_json_writer *writer() {
    return &writer_;
  }
  arnm *arena() {
    return &arena_;
  }

private:
  arnm arena_{};
  arnm_json_writer writer_{};
};

/** Where an arena's index stands, as an offset from wherever it started. */
uintptr_t ArenaMark(arnm *arena) {
  uint8_t *probe = nullptr;
  EXPECT_EQ(arnm_alloc(&probe, 1, arena), ARNM_SUCCESS);
  const uintptr_t mark = reinterpret_cast<uintptr_t>(probe);
  EXPECT_EQ(arnm_free(probe, 1, arena), ARNM_SUCCESS);
  return mark;
}

/**
 * Write, hand back the text, and check the promise the size call made about it.
 *
 * Every test that writes goes through here, so the measurement is not something a few tests
 * remember to check -- it is checked on every document any of them ever builds.
 */
std::string Write(arnm_json_writer *writer, arnm *allocator, bool size_is_exact = true) {
  const uint32_t promised = arnm_json_writer_size(writer);

  arnm_memory_block block{};
  uint32_t length = 0;
  const arnm_result result = arnm_json_writer_write(writer, allocator, &block, &length);
  EXPECT_EQ(result, ARNM_SUCCESS);
  if (ARNM_SUCCESS != result) { return {}; }

  EXPECT_EQ(std::strlen(reinterpret_cast<const char *>(block.data)), length)
      << "JSON never holds a NUL byte, so the terminator and the length have to agree";
  EXPECT_GE(promised, length + 1u) << "a measurement that comes out short is a buffer overrun";
  if (size_is_exact) {
    EXPECT_EQ(promised, length + 1u) << "nothing here should have made the answer an estimate";
  }

  std::string text(reinterpret_cast<const char *>(block.data), length);
  EXPECT_EQ(arnm_memory_block_free(&block, allocator), ARNM_SUCCESS);
  return text;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(JsonWriter, AZeroedWriterIsNotInitialized) {
  arnm_json_writer writer;
  std::memset(&writer, 0, sizeof(writer));

  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_writer_begin_object(&writer), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_writer_release(&writer), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_writer_size(&writer), 0u);
  EXPECT_EQ(arnm_json_writer_depth(&writer), 0u);
  EXPECT_STREQ(arnm_json_writer_error_field(&writer), "");

  arnm_memory_block block{};
  EXPECT_EQ(arnm_json_writer_write(&writer, nullptr, &block, nullptr), ARNM_ERROR_NOT_INITIALIZED);
}

TEST(JsonWriter, InitWritesEveryFieldAndAllocatesNothing) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  // deliberately dirty storage: init reads none of it
  arnm_json_writer writer;
  std::memset(&writer, 0xAB, sizeof(writer));
  ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT), ARNM_SUCCESS);

  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_depth(&writer), 0u) << "no document until the first field";
  EXPECT_EQ(arnm_json_writer_size(&writer), 0u);
  EXPECT_EQ(ArenaMark(&arena), before) << "init must not draw from the allocator";

  EXPECT_EQ(arnm_json_writer_release(&writer), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonWriter, AnUnknownFlagBitIsRefusedBeforeAnythingIsWritten) {
  arnm_json_writer writer;
  std::memset(&writer, 0, sizeof(writer));
  EXPECT_EQ(arnm_json_writer_init(&writer, nullptr, 1u << 20), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_writer_create(nullptr, 1u << 20), nullptr);
  EXPECT_EQ(
      arnm_json_writer_init(nullptr, nullptr, ARNM_JSON_WRITE_DEFAULT), ARNM_ERROR_NULL_POINTER
  );
}

TEST(JsonWriter, ACreatedWriterGoesHomeThroughDestroy) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  arnm_json_writer *writer = arnm_json_writer_create(&arena, ARNM_JSON_WRITE_DEFAULT);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(ArenaMark(&arena) - before, ARNM_ALIGN8(sizeof(arnm_json_writer)));

  arnm_json_writer_add_uint64(writer, "n", 1);
  EXPECT_EQ(arnm_json_writer_status(writer), ARNM_SUCCESS);

  // the document sits above the state, so what an arena cannot take back is what it says
  const arnm_result destroyed = arnm_json_writer_destroy(writer, &arena);
  EXPECT_TRUE(ARNM_SUCCESS == destroyed || ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == destroyed);
  arnm_reset(&arena);
  EXPECT_EQ(ArenaMark(&arena), before) << "a reset is what returns the rest";
  arnm_release(&arena);
}

TEST(JsonWriter, NullReachesEveryCallWithoutHarm) {
  EXPECT_EQ(arnm_json_writer_release(nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_destroy(nullptr, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_begin_object(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_writer_begin_array(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_writer_status(nullptr), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_STREQ(arnm_json_writer_error_field(nullptr), "");
  EXPECT_EQ(arnm_json_writer_clear_error(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_writer_size(nullptr), 0u);
  EXPECT_EQ(arnm_json_writer_depth(nullptr), 0u);

  arnm_json_writer_add_null(nullptr, "a");
  arnm_json_writer_add_bool(nullptr, "a", true);
  arnm_json_writer_add_int64(nullptr, "a", 1);
  arnm_json_writer_add_uint64(nullptr, "a", 1);
  arnm_json_writer_add_double(nullptr, "a", 1.0);
  arnm_json_writer_add_string(nullptr, "a", "b");
  arnm_json_writer_add_string_length(nullptr, "a", "b", 1);
  arnm_json_writer_add_string_copy(nullptr, "a", "b");
  arnm_json_writer_add_string_copy_length(nullptr, "a", "b", 1);
  arnm_json_writer_open_object(nullptr, "a");
  arnm_json_writer_open_array(nullptr, "a");
  arnm_json_writer_close(nullptr);

  arnm_memory_block block{};
  EXPECT_EQ(arnm_json_writer_write(nullptr, nullptr, &block, nullptr), ARNM_ERROR_NULL_POINTER);
}

// ---------------------------------------------------------------------------
// writing a struct, one line per member
// ---------------------------------------------------------------------------

TEST(JsonWriter, AStructGoesOutInOneRunAndIsAskedAboutOnce) {
  struct {
    const char *name;
    uint64_t port;
    int64_t offset;
    bool debug;
    const char *note;
  } config{"arnm", 8443, -7, true, nullptr};

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", config.name);
  arnm_json_writer_add_uint64(owner.writer(), "port", config.port);
  arnm_json_writer_add_int64(owner.writer(), "offset", config.offset);
  arnm_json_writer_add_bool(owner.writer(), "debug", config.debug);
  arnm_json_writer_add_string(owner.writer(), "note", config.note);

  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "");
  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"name\":\"arnm\",\"port\":8443,\"offset\":-7,\"debug\":true,\"note\":null}"
  ) << "a NULL string is the literal null, which is what an absent optional member means";
}

TEST(JsonWriter, AnEmptyDocumentIsStillADocument) {
  ArenaWriter owner;
  ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_size(owner.writer()), 3u) << "{} and the terminator";
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{}");

  ASSERT_EQ(arnm_json_writer_begin_array(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "[]");
}

TEST(JsonWriter, AnArrayRootTakesItsElementsWithoutNames) {
  ArenaWriter owner;
  ASSERT_EQ(arnm_json_writer_begin_array(owner.writer()), ARNM_SUCCESS);
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 1);
  arnm_json_writer_add_string(owner.writer(), nullptr, "two");
  arnm_json_writer_add_bool(owner.writer(), nullptr, false);
  arnm_json_writer_add_null(owner.writer(), nullptr);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "[1,\"two\",false,null]");
}

TEST(JsonWriter, NestingCostsAnOpenAndACloseAndNothingElse) {
  const char *names[] = {"alpha", "beta"};

  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), "id", 3);

  arnm_json_writer_open_object(owner.writer(), "address");
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 2u);
  arnm_json_writer_add_string(owner.writer(), "city", "Bern");
  arnm_json_writer_close(owner.writer());

  arnm_json_writer_open_array(owner.writer(), "peers");
  for (uint32_t index = 0; index < 2; ++index) {
    arnm_json_writer_open_object(owner.writer(), nullptr);
    arnm_json_writer_add_string(owner.writer(), "name", names[index]);
    arnm_json_writer_add_uint64(owner.writer(), "port", index + 1u);
    arnm_json_writer_close(owner.writer());
  }
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 1u);
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"id\":3,\"address\":{\"city\":\"Bern\"},"
      "\"peers\":[{\"name\":\"alpha\",\"port\":1},{\"name\":\"beta\",\"port\":2}]}"
  );
}

TEST(JsonWriter, AnEmptyNestedContainerStaysOnOneLine) {
  ArenaWriter owner(ARNM_JSON_WRITE_PRETTY);
  arnm_json_writer_open_object(owner.writer(), "empty_object");
  arnm_json_writer_close(owner.writer());
  arnm_json_writer_open_array(owner.writer(), "empty_array");
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\n    \"empty_object\": {},\n    \"empty_array\": []\n}"
  );
}

TEST(JsonWriter, PrettyPutsOneValueOnEachLine) {
  ArenaWriter four(ARNM_JSON_WRITE_PRETTY);
  arnm_json_writer_add_string(four.writer(), "name", "arnm");
  arnm_json_writer_open_object(four.writer(), "nested");
  arnm_json_writer_add_uint64(four.writer(), "n", 1);
  arnm_json_writer_close(four.writer());
  EXPECT_EQ(
      Write(four.writer(), four.arena()),
      "{\n    \"name\": \"arnm\",\n    \"nested\": {\n        \"n\": 1\n    }\n}"
  );

  ArenaWriter two(ARNM_JSON_WRITE_PRETTY_TWO_SPACES);
  arnm_json_writer_add_string(two.writer(), "name", "arnm");
  arnm_json_writer_open_array(two.writer(), "list");
  arnm_json_writer_add_uint64(two.writer(), nullptr, 1);
  arnm_json_writer_add_uint64(two.writer(), nullptr, 2);
  arnm_json_writer_close(two.writer());
  EXPECT_EQ(
      Write(two.writer(), two.arena()),
      "{\n  \"name\": \"arnm\",\n  \"list\": [\n    1,\n    2\n  ]\n}"
  );
}

TEST(JsonWriter, ANewlineAtTheEndIsMeasuredToo) {
  ArenaWriter owner(ARNM_JSON_WRITE_NEWLINE_AT_END);
  arnm_json_writer_add_uint64(owner.writer(), "n", 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"n\":1}\n");
}

// ---------------------------------------------------------------------------
// strings: borrowed, or copied
// ---------------------------------------------------------------------------

TEST(JsonWriter, AStringIsBorrowedWhereItLies) {
  char buffer[] = "first";

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "value", buffer);
  // nothing was copied, so changing the source before the write changes what is written -- the
  // plainest proof there is that the pointer is all the writer kept
  std::memcpy(buffer, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"SECON\"}");
}

TEST(JsonWriter, ACopiedStringStandsOnItsOwn) {
  char buffer[] = "first";

  ArenaWriter owner;
  arnm_json_writer_add_string_copy(owner.writer(), "value", buffer);
  std::memcpy(buffer, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"first\"}");
}

TEST(JsonWriter, AStringMayHoldAnEmbeddedNul) {
  const char value[] = "be\0fore";

  ArenaWriter owner;
  arnm_json_writer_add_string_length(owner.writer(), "borrowed", value, 7);
  arnm_json_writer_add_string_copy_length(owner.writer(), "copied", value, 7);

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"borrowed\":\"be\\u0000fore\",\"copied\":\"be\\u0000fore\"}"
  );
}

TEST(JsonWriter, EveryEscapeIsMeasuredBeforeItIsWritten) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "quote", "a\"b");
  arnm_json_writer_add_string(owner.writer(), "slash", "a\\b");
  arnm_json_writer_add_string(owner.writer(), "short", "a\nb\tc\rd");
  arnm_json_writer_add_string(owner.writer(), "control", "a\x01\x1f b");
  arnm_json_writer_add_string(owner.writer(), "path", "a/b");

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"quote\":\"a\\\"b\",\"slash\":\"a\\\\b\",\"short\":\"a\\nb\\tc\\rd\","
      "\"control\":\"a\\u0001\\u001F b\",\"path\":\"a/b\"}"
  );
}

TEST(JsonWriter, ASlashIsEscapedOnlyWhenAskedFor) {
  ArenaWriter owner(ARNM_JSON_WRITE_ESCAPE_SLASHES);
  arnm_json_writer_add_string(owner.writer(), "path", "/usr/bin");
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"path\":\"\\/usr\\/bin\"}");
}

TEST(JsonWriter, UnicodeIsCopiedThroughAndMeasuredExactly) {
  // two bytes, three bytes and four bytes of UTF-8, copied rather than escaped: every byte of
  // the input is one byte of the output, which is a length the measurement knows exactly
  const char *value = "\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "text", value);
  const std::string written = Write(owner.writer(), owner.arena());
  EXPECT_EQ(written, std::string("{\"text\":\"") + value + "\"}");
}

TEST(JsonWriter, EscapedUnicodeIsMeasuredGenerously) {
  const char *value = "\xC3\xA4\xF0\x9F\x98\x80";

  ArenaWriter owner(ARNM_JSON_WRITE_ESCAPE_UNICODE);
  arnm_json_writer_add_string(owner.writer(), "text", value);
  // six bytes charged for every byte outside ASCII: exact for one escaped alone, and more than
  // enough for a character whose bytes become a single escape
  EXPECT_EQ(
      Write(owner.writer(), owner.arena(), /*size_is_exact=*/false),
      "{\"text\":\"\\u00E4\\uD83D\\uDE00\"}"
  );
}

// ---------------------------------------------------------------------------
// the first error, kept
// ---------------------------------------------------------------------------

TEST(JsonWriter, TheFirstRefusalIsTheOneThatStays) {
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), "good", 1);

  // an element without a name, inside an object that needs one
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 2);
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "[]");

  // everything after it does nothing at all, and changes nothing about the verdict
  arnm_json_writer_add_string(owner.writer(), "later", "value");
  arnm_json_writer_open_object(owner.writer(), "deeper");
  arnm_json_writer_close(owner.writer());
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "[]");

  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_writer_write(owner.writer(), owner.arena(), &block, nullptr),
      ARNM_ERROR_INVALID_PARAM
  ) << "a writer carrying an error refuses to write, so the one result stands in for the check";
  EXPECT_EQ(block.data, nullptr) << "a refusal leaves every output untouched";

  // cleared, the writing counts again -- and what was refused stayed out
  ASSERT_EQ(arnm_json_writer_clear_error(owner.writer()), ARNM_SUCCESS);
  arnm_json_writer_add_uint64(owner.writer(), "after", 3);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"good\":1,\"after\":3}");
}

TEST(JsonWriter, ANameInsideAnArrayIsRefusedByTheContainer) {
  ArenaWriter owner;
  arnm_json_writer_open_array(owner.writer(), "list");
  arnm_json_writer_add_uint64(owner.writer(), "named", 1);

  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "named");
}

TEST(JsonWriter, OneCloseTooManyIsRecordedRatherThanSwallowed) {
  ArenaWriter owner;
  arnm_json_writer_open_object(owner.writer(), "inner");
  arnm_json_writer_close(owner.writer());
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_STATE)
      << "a silent extra close would move the next field somewhere nobody expects";
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 1u);
}

TEST(JsonWriter, OpeningPastTheLastLevelIsRefused) {
  ArenaWriter owner;
  for (uint32_t level = 1; level < ARNM_JSON_WRITER_MAX_DEPTH; ++level) {
    arnm_json_writer_open_object(owner.writer(), "down");
    EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS) << "at level " << level;
  }
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), ARNM_JSON_WRITER_MAX_DEPTH);

  arnm_json_writer_open_object(owner.writer(), "one_too_deep");
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "one_too_deep");
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), ARNM_JSON_WRITER_MAX_DEPTH);
}

TEST(JsonWriter, ALongFieldNameIsKeptAsFarAsItFits) {
  const std::string key(80, 'k');
  ArenaWriter owner;
  arnm_json_writer_open_array(owner.writer(), "list");
  arnm_json_writer_add_uint64(owner.writer(), key.c_str(), 1);

  const std::string recorded = arnm_json_writer_error_field(owner.writer());
  EXPECT_EQ(recorded.size(), ARNM_JSON_WRITER_FIELD_NAME_SIZE - 1u);
  EXPECT_EQ(recorded, key.substr(0, recorded.size()));
}

TEST(JsonWriter, AnArenaWithNoRoomIsRecordedAsOutOfMemory) {
  uint8_t storage[64] = {0};
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_json_writer writer{};
  ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT), ARNM_SUCCESS);
  for (uint32_t index = 0; index < 64; ++index) {
    arnm_json_writer_add_uint64(&writer, "n", index);
  }

  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_ERROR_OUT_OF_MEMORY);
  arnm_json_writer_release(&writer);
  arnm_release(&arena);
}

TEST(JsonWriter, ANonFiniteNumberNeedsAFlagOrTheWholeWriteIsRefused) {
  const double infinity = 1e308 * 10.0;

  ArenaWriter strict;
  arnm_json_writer_add_double(strict.writer(), "n", infinity);
  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_writer_write(strict.writer(), strict.arena(), &block, nullptr),
      ARNM_ERROR_ENCODE_FAILED
  );
  EXPECT_EQ(block.data, nullptr);

  ArenaWriter allowed(ARNM_JSON_WRITE_ALLOW_INF_AND_NAN);
  arnm_json_writer_add_double(allowed.writer(), "n", infinity);
  EXPECT_EQ(Write(allowed.writer(), allowed.arena(), /*size_is_exact=*/false), "{\"n\":Infinity}");

  ArenaWriter as_null(ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL);
  arnm_json_writer_add_double(as_null.writer(), "n", infinity);
  EXPECT_EQ(Write(as_null.writer(), as_null.arena(), /*size_is_exact=*/false), "{\"n\":null}");
}

// ---------------------------------------------------------------------------
// measuring the output before it exists
// ---------------------------------------------------------------------------

TEST(JsonWriter, TheSizeIsKnownBeforeAByteOfTextExists) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", "arnm");
  arnm_json_writer_add_uint64(owner.writer(), "port", 8443);

  // {"name":"arnm","port":8443} is 27 bytes, and the terminator is the 28th
  EXPECT_EQ(arnm_json_writer_size(owner.writer()), 28u);
  EXPECT_EQ(Write(owner.writer(), owner.arena()).size(), 27u);
}

TEST(JsonWriter, TheSizeGrowsWithEveryFieldAndNeverWalksTheDocument) {
  ArenaWriter owner;
  ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);

  uint32_t previous = arnm_json_writer_size(owner.writer());
  EXPECT_EQ(previous, 3u);
  for (uint32_t index = 0; index < 32; ++index) {
    arnm_json_writer_add_uint64(owner.writer(), "key", index);
    const uint32_t now = arnm_json_writer_size(owner.writer());
    EXPECT_GT(now, previous) << "at field " << index;
    previous = now;
  }
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(Write(owner.writer(), owner.arena()).size() + 1u, previous);
}

TEST(JsonWriter, EveryShapeAndEveryLayoutIsMeasuredExactly) {
  // the same document under every layout the flags allow, checked through Write() -- which
  // compares the promise against the text on each one
  const arnm_json_write_flags layouts[] = {
      ARNM_JSON_WRITE_DEFAULT,
      ARNM_JSON_WRITE_PRETTY,
      ARNM_JSON_WRITE_PRETTY_TWO_SPACES,
      ARNM_JSON_WRITE_NEWLINE_AT_END,
      ARNM_JSON_WRITE_PRETTY | ARNM_JSON_WRITE_NEWLINE_AT_END,
      ARNM_JSON_WRITE_ESCAPE_SLASHES,
      ARNM_JSON_WRITE_PRETTY | ARNM_JSON_WRITE_ESCAPE_SLASHES,
  };

  for (arnm_json_write_flags flags : layouts) {
    ArenaWriter owner(flags);
    arnm_json_writer_add_string(owner.writer(), "name", "a name with \"quotes\" and a \\ and /");
    arnm_json_writer_add_string(owner.writer(), "lines", "one\ntwo\tthree\x01");
    arnm_json_writer_add_uint64(owner.writer(), "big", UINT64_MAX);
    arnm_json_writer_add_int64(owner.writer(), "small", INT64_MIN);
    arnm_json_writer_add_bool(owner.writer(), "yes", true);
    arnm_json_writer_add_bool(owner.writer(), "no", false);
    arnm_json_writer_add_null(owner.writer(), "nothing");
    arnm_json_writer_add_string(owner.writer(), "unicode", "\xC3\xA4\xE2\x82\xAC");

    arnm_json_writer_open_object(owner.writer(), "empty");
    arnm_json_writer_close(owner.writer());

    arnm_json_writer_open_array(owner.writer(), "list");
    for (uint32_t index = 0; index < 3; ++index) {
      arnm_json_writer_open_object(owner.writer(), nullptr);
      arnm_json_writer_add_uint64(owner.writer(), "index", index);
      arnm_json_writer_open_array(owner.writer(), "inner");
      arnm_json_writer_add_string(owner.writer(), nullptr, "deep");
      arnm_json_writer_close(owner.writer());
      arnm_json_writer_close(owner.writer());
    }
    arnm_json_writer_close(owner.writer());

    ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS) << "flags " << flags;
    EXPECT_FALSE(Write(owner.writer(), owner.arena()).empty()) << "flags " << flags;
  }
}

TEST(JsonWriter, ARealNumberIsChargedItsLongestForm) {
  ArenaWriter owner;
  arnm_json_writer_add_double(owner.writer(), "ratio", 0.5);

  // {"ratio":} is 10 bytes plus the terminator, and the number is charged its ceiling
  EXPECT_EQ(arnm_json_writer_size(owner.writer()), 11u + ARNM_JSON_WRITER_MAX_NUMBER_TEXT);
  EXPECT_EQ(Write(owner.writer(), owner.arena(), /*size_is_exact=*/false), "{\"ratio\":0.5}");
}

TEST(JsonWriter, TheLongestRealNumberStillFitsItsCharge) {
  // the ceiling is a claim about doubles, so it is checked against the ones that reach it
  const double extremes[] = {
      -1.7976931348623157e308,
      1.7976931348623157e308,
      -2.2250738585072014e-308,
      5e-324,
      -0.0,
      1.0 / 3.0,
  };

  for (double value : extremes) {
    ArenaWriter owner;
    arnm_json_writer_begin_array(owner.writer());
    arnm_json_writer_add_double(owner.writer(), nullptr, value);
    ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);

    const std::string text = Write(owner.writer(), owner.arena(), /*size_is_exact=*/false);
    ASSERT_GE(text.size(), 2u);
    EXPECT_LE(text.size() - 2u, ARNM_JSON_WRITER_MAX_NUMBER_TEXT)
        << "rendered as " << text << ", which is longer than the ceiling promises";
  }
}

TEST(JsonWriter, TheTextIsShrunkToWhatItActuallyNeeded) {
  ArenaWriter owner;
  for (uint32_t index = 0; index < 16; ++index) {
    arnm_json_writer_add_string(owner.writer(), "key", "a value of an ordinary length");
  }

  const uintptr_t before = ArenaMark(owner.arena());
  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), owner.arena(), &block, &length), ARNM_SUCCESS);

  // the serializer asks for more than the text needs, and the slack goes home before the call
  // returns -- so what the arena is holding is the text and nothing else
  EXPECT_EQ(block.size, length + 1u);
  EXPECT_EQ(ArenaMark(owner.arena()) - before, ARNM_ALIGN8(length + 1u));

  EXPECT_EQ(arnm_memory_block_free(&block, owner.arena()), ARNM_SUCCESS);
  EXPECT_EQ(ArenaMark(owner.arena()), before) << "and it all comes back";
}

TEST(JsonWriter, AnArenaSizedByTheMeasurementHoldsTheText) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", "arnm");
  arnm_json_writer_add_uint64(owner.writer(), "port", 8443);
  arnm_json_writer_open_array(owner.writer(), "list");
  for (uint32_t index = 0; index < 8; ++index) {
    arnm_json_writer_add_uint64(owner.writer(), nullptr, index * 1000u);
  }
  arnm_json_writer_close(owner.writer());

  const uint32_t promised = arnm_json_writer_size(owner.writer());

  // an arena of exactly the promised size: the working room the serializer needs comes from
  // the writer's own allocator, so this one carries the text and nothing else
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, promised), ARNM_SUCCESS);

  arnm_memory_block block{};
  uint32_t length = 0;
  const arnm_result result = arnm_json_writer_write(owner.writer(), &output, &block, &length);
  ASSERT_EQ(result, ARNM_SUCCESS);
  EXPECT_EQ(length + 1u, promised) << "sized to the byte";

  EXPECT_EQ(arnm_memory_block_free(&block, &output), ARNM_SUCCESS);
  arnm_release(&output);
}

TEST(JsonWriter, TheHostCanCarryTheTextJustAsWell) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", "arnm");

  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), nullptr, &block, &length), ARNM_SUCCESS);
  EXPECT_STREQ(reinterpret_cast<const char *>(block.data), "{\"name\":\"arnm\"}");
  EXPECT_EQ(arnm_memory_block_free(&block, nullptr), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// reuse, and the way back
// ---------------------------------------------------------------------------

TEST(JsonWriter, OneWriterServesOnePayloadAfterAnother) {
  ArenaWriter owner;
  for (uint32_t round = 0; round < 3; ++round) {
    ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);
    arnm_json_writer_add_uint64(owner.writer(), "round", round);
    EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"round\":" + std::to_string(round) + "}");
  }
}

TEST(JsonWriter, ABeginClearsWhatTheLastDocumentLeftBehind) {
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 1);
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);

  ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "");
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 1u);

  arnm_json_writer_add_uint64(owner.writer(), "n", 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"n\":1}");
}

TEST(JsonWriter, WhatWasWrittenReadsBackAsWhatWentIn) {
  // the two halves of the module, back to back: what the writer put down is what the reader
  // finds, field for field
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", "arnm");
  arnm_json_writer_add_uint64(owner.writer(), "port", 8443);
  arnm_json_writer_add_int64(owner.writer(), "offset", -7);
  arnm_json_writer_add_bool(owner.writer(), "debug", true);
  arnm_json_writer_add_double(owner.writer(), "ratio", 0.25);
  arnm_json_writer_open_array(owner.writer(), "tags");
  arnm_json_writer_add_string(owner.writer(), nullptr, "one");
  arnm_json_writer_add_string(owner.writer(), nullptr, "two");
  arnm_json_writer_close(owner.writer());

  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), owner.arena(), &block, &length), ARNM_SUCCESS);

  arnm reading{};
  ASSERT_EQ(arnm_init_arena(&reading, kArenaCapacity), ARNM_SUCCESS);
  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, &reading, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);
  ASSERT_EQ(
      arnm_json_reader_parse(&reader, reinterpret_cast<const char *>(block.data), length),
      ARNM_SUCCESS
  );

  EXPECT_STREQ(arnm_json_reader_get_string(&reader, "name"), "arnm");
  EXPECT_EQ(arnm_json_reader_get_uint64(&reader, "port"), 8443u);
  EXPECT_EQ(arnm_json_reader_get_int64(&reader, "offset"), -7);
  EXPECT_TRUE(arnm_json_reader_get_bool(&reader, "debug"));
  EXPECT_DOUBLE_EQ(arnm_json_reader_get_double(&reader, "ratio"), 0.25);

  arnm_json_value *tags = arnm_json_reader_enter(&reader, "tags");
  ASSERT_EQ(arnm_json_reader_count(&reader), 2u);
  arnm_json_value *first = arnm_json_reader_enter_at(&reader, 0);
  EXPECT_STREQ(arnm_json_reader_get_string(&reader, nullptr), "one");
  arnm_json_reader_leave(&reader, first);
  arnm_json_reader_leave(&reader, tags);
  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_SUCCESS);

  arnm_json_reader_release(&reader);
  arnm_release(&reading);
  EXPECT_EQ(arnm_memory_block_free(&block, owner.arena()), ARNM_SUCCESS);
}
