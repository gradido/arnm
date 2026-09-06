#include "arnm/arena.h"
#include "arnm/converter.h"
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
// What is checked over and over is that a struct written field by field comes out as the text
// it should, byte for byte, under every layout the flags allow.
//
// The sizing is deliberately *not* checked that way any more. arnm_json_writer_buffer_size_min()
// replaced a running length that was exact with a guess made from the element count, so there
// is no promise left to hold it to -- the tests below pin what it still claims (it grows, it
// follows the layout, it is free to ask) and one of them pins that it is not a bound.

#define LONG_CONTENT_STRING                                                                        \
  "A well-written JSON document is like a well-written program: every brace has a purpose, every " \
  "comma has a place, and nothing should escape without being properly encoded."
#define LONG_CONTENT_STRING_QUOTED                                                                 \
  "\"A well-written JSON document is like a well-written program: every brace has a purpose, "     \
  "every comma has a place, and nothing should escape without being properly encoded.\""

namespace {

/** Arena large enough for every document below, the working room of a write included. */
constexpr uint32_t kArenaCapacity = 256 * 1024;

/** A writer over an arena, torn down in the right order at the end of a scope. */
class ArenaWriter {
public:
  explicit ArenaWriter(
      arnm_json_write_flags flags = ARNM_JSON_WRITE_DEFAULT,
      const arnm_json_writer_hint *hint = nullptr
  ) {
    EXPECT_EQ(arnm_init_arena(&arena_, kArenaCapacity), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_writer_init(&writer_, &arena_, flags, hint), ARNM_SUCCESS);
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
 * Write and hand back the text, checking what still holds for every document.
 *
 * Every test that writes goes through here, so the terminator and the reported length are
 * agreed on once rather than in the tests that remember to ask.
 */
std::string Write(arnm_json_writer *writer, arnm *allocator) {
  arnm_memory_block block{};
  uint32_t length = 0;
  const arnm_result result = arnm_json_writer_write(writer, allocator, &block, &length);
  EXPECT_EQ(result, ARNM_SUCCESS);
  if (ARNM_SUCCESS != result) { return {}; }

  EXPECT_EQ(std::strlen(reinterpret_cast<const char *>(block.data)), length)
      << "JSON never holds a NUL byte, so the terminator and the length have to agree";

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
  EXPECT_EQ(arnm_json_writer_buffer_size_min(&writer), 0u);
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
  ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT, NULL), ARNM_SUCCESS);

  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_depth(&writer), 0u) << "no document until the first field";
  EXPECT_EQ(arnm_json_writer_buffer_size_min(&writer), 0u);
  EXPECT_EQ(ArenaMark(&arena), before) << "init must not draw from the allocator";

  EXPECT_EQ(arnm_json_writer_release(&writer), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonWriter, AnUnknownFlagBitIsRefusedBeforeAnythingIsWritten) {
  arnm_json_writer writer;
  std::memset(&writer, 0, sizeof(writer));
  EXPECT_EQ(arnm_json_writer_init(&writer, nullptr, 1u << 20, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_ERROR_NOT_INITIALIZED);

  // bits 4 and 6 carried ALLOW_INF_AND_NAN and ALLOW_INVALID_UNICODE, which this build cannot
  // honour. They were left empty rather than closed up, so a caller still passing one is told
  // so here instead of landing on whatever a renumbering would have moved into their place.
  for (unsigned bit : {4u, 6u}) {
    EXPECT_EQ(arnm_json_writer_init(&writer, nullptr, 1u << bit, nullptr), ARNM_ERROR_INVALID_PARAM)
        << "bit " << bit;
  }
  // and the neighbours that survived still mean exactly what they did
  EXPECT_EQ(ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL, 1u << 5);
  EXPECT_EQ(ARNM_JSON_WRITE_NEWLINE_AT_END, 1u << 7);
  EXPECT_EQ(arnm_json_writer_create(nullptr, 1u << 20, nullptr), nullptr);
  EXPECT_EQ(
      arnm_json_writer_init(nullptr, nullptr, ARNM_JSON_WRITE_DEFAULT, nullptr),
      ARNM_ERROR_NULL_POINTER
  );
}

TEST(JsonWriter, ACreatedWriterGoesHomeThroughDestroy) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  arnm_json_writer *writer = arnm_json_writer_create(&arena, ARNM_JSON_WRITE_DEFAULT, nullptr);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(ArenaMark(&arena) - before, ARNM_ALIGN8(sizeof(arnm_json_writer)));

  arnm_json_writer_add_uint64(writer, ARNM_JSON_WRITER_KEY("n"), 1);
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
  EXPECT_EQ(arnm_json_writer_buffer_size_min(nullptr), 0u);
  EXPECT_EQ(arnm_json_writer_depth(nullptr), 0u);

  arnm_json_writer_add_null(nullptr, ARNM_JSON_WRITER_KEY("a"));
  arnm_json_writer_add_bool(nullptr, ARNM_JSON_WRITER_KEY("a"), true);
  arnm_json_writer_add_int64(nullptr, ARNM_JSON_WRITER_KEY("a"), 1);
  arnm_json_writer_add_uint64(nullptr, ARNM_JSON_WRITER_KEY("a"), 1);
  arnm_json_writer_add_double(nullptr, ARNM_JSON_WRITER_KEY("a"), 1.0);
  arnm_json_writer_add_string(nullptr, ARNM_JSON_WRITER_KEY("a"), "b", 1);
  arnm_json_writer_add_string_flags(
      nullptr, ARNM_JSON_WRITER_KEY("a"), "b", 1, ARNM_JSON_WRITER_STRING_COPY
  );
  arnm_json_writer_open_object(nullptr, ARNM_JSON_WRITER_KEY("a"));
  arnm_json_writer_open_array(nullptr, ARNM_JSON_WRITER_KEY("a"));
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
  arnm_json_writer_add_string(
      owner.writer(), ARNM_JSON_WRITER_KEY("name"), config.name, strlen(config.name)
  );
  arnm_json_writer_add_uint64(owner.writer(), ARNM_JSON_WRITER_KEY("port"), config.port);
  arnm_json_writer_add_int64(owner.writer(), ARNM_JSON_WRITER_KEY("offset"), config.offset);
  arnm_json_writer_add_bool(owner.writer(), ARNM_JSON_WRITER_KEY("debug"), config.debug);
  arnm_json_writer_add_string(
      owner.writer(), ARNM_JSON_WRITER_KEY("note"), config.note, strlen(config.note)
  );

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
  EXPECT_EQ(arnm_json_writer_buffer_size_min(owner.writer()), 96u)
      << "the root counts as one element, and the floor is what the rest of this is";
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{}");

  ASSERT_EQ(arnm_json_writer_begin_array(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_buffer_size_min(owner.writer()), 96u) << "and the same for an array";
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "[]");
}

TEST(JsonWriter, AnArrayRootTakesItsElementsWithoutNames) {
  ArenaWriter owner;
  ASSERT_EQ(arnm_json_writer_begin_array(owner.writer()), ARNM_SUCCESS);
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 0, false, 1);
  arnm_json_writer_add_string(owner.writer(), nullptr, 0, false, "two", 3);
  arnm_json_writer_add_bool(owner.writer(), nullptr, 0, false, false);
  arnm_json_writer_add_null(owner.writer(), nullptr, 0, false);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "[1,\"two\",false,null]");
}

TEST(JsonWriter, NestingCostsAnOpenAndACloseAndNothingElse) {
  const char *names[] = {"alpha", "beta"};

  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), ARNM_JSON_WRITER_KEY("id"), 3);

  arnm_json_writer_open_object(owner.writer(), ARNM_JSON_WRITER_KEY("address"));
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 2u);
  arnm_json_writer_add_string(owner.writer(), ARNM_JSON_WRITER_KEY("city"), "Bern", 4);
  arnm_json_writer_close(owner.writer());

  arnm_json_writer_open_array(owner.writer(), ARNM_JSON_WRITER_KEY("peers"));
  for (uint32_t index = 0; index < 2; ++index) {
    arnm_json_writer_open_object(owner.writer(), nullptr, 0, false);
    arnm_json_writer_add_string(
        owner.writer(), ARNM_JSON_WRITER_KEY("name"), names[index], strlen(names[index])
    );
    arnm_json_writer_add_uint64(owner.writer(), ARNM_JSON_WRITER_KEY("port"), index + 1u);
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
  arnm_json_writer_open_object(owner.writer(), ARNM_JSON_WRITER_KEY("empty_object"));
  arnm_json_writer_close(owner.writer());
  arnm_json_writer_open_array(owner.writer(), ARNM_JSON_WRITER_KEY("empty_array"));
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\n    \"empty_object\": {},\n    \"empty_array\": []\n}"
  );
}

TEST(JsonWriter, PrettyPutsOneValueOnEachLine) {
  ArenaWriter four(ARNM_JSON_WRITE_PRETTY);
  arnm_json_writer_add_string(four.writer(), ARNM_JSON_WRITER_KEY("name"), "arnm", 4);
  arnm_json_writer_open_object(four.writer(), ARNM_JSON_WRITER_KEY("nested"));
  arnm_json_writer_add_uint64(four.writer(), ARNM_JSON_WRITER_KEY("n"), 1);
  arnm_json_writer_close(four.writer());
  EXPECT_EQ(
      Write(four.writer(), four.arena()),
      "{\n    \"name\": \"arnm\",\n    \"nested\": {\n        \"n\": 1\n    }\n}"
  );

  ArenaWriter two(ARNM_JSON_WRITE_PRETTY_TWO_SPACES);
  arnm_json_writer_add_string(two.writer(), ARNM_JSON_WRITER_KEY("name"), "arnm", 4);
  arnm_json_writer_open_array(two.writer(), ARNM_JSON_WRITER_KEY("list"));
  arnm_json_writer_add_uint64(two.writer(), nullptr, 0, false, 1);
  arnm_json_writer_add_uint64(two.writer(), nullptr, 0, false, 2);
  arnm_json_writer_close(two.writer());
  EXPECT_EQ(
      Write(two.writer(), two.arena()),
      "{\n  \"name\": \"arnm\",\n  \"list\": [\n    1,\n    2\n  ]\n}"
  );
}

TEST(JsonWriter, ANewlineAtTheEndComesAheadOfTheTerminator) {
  ArenaWriter owner(ARNM_JSON_WRITE_NEWLINE_AT_END);
  arnm_json_writer_add_uint64(owner.writer(), ARNM_JSON_WRITER_KEY("n"), 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"n\":1}\n");
}

TEST(JsonWriter, AValueLongerThanTheEstimateIsStillWrittenWhole) {
  // one field of prose is already past what the element count guessed, so the serializer grows
  // the buffer it was handed mid-write. What comes back has to be the whole text regardless.
  ArenaWriter owner;
  arnm_json_writer_add_string(
      owner.writer(), ARNM_JSON_WRITER_KEY("n"), LONG_CONTENT_STRING,
      sizeof(LONG_CONTENT_STRING) - 1u
  );

  const std::string expected = std::string("{\"n\":\"") + LONG_CONTENT_STRING + "\"}";
  EXPECT_EQ(Write(owner.writer(), owner.arena()), expected);
}

TEST(JsonWriter, AValueLongerThanTheEstimateIsStillWrittenWholeRaw) {
  // the same through the raw adder, which grows the same buffer without the escaping pass
  ArenaWriter owner;
  arnm_json_writer_add_string_flags(
      owner.writer(), ARNM_JSON_WRITER_KEY("n"), LONG_CONTENT_STRING_QUOTED,
      sizeof(LONG_CONTENT_STRING_QUOTED) - 1u, ARNM_JSON_WRITER_STRING_RAW
  );

  const std::string expected = std::string("{\"n\":\"") + LONG_CONTENT_STRING + "\"}";
  EXPECT_EQ(Write(owner.writer(), owner.arena()), expected);
}

// ---------------------------------------------------------------------------
// keys carry their own length
// ---------------------------------------------------------------------------

TEST(JsonWriter, AKeyIsReadForExactlyItsLengthAndNotToATerminator) {
  // the whole reason the length is a parameter: the writer never walks the key. A name that
  // sits inside a larger buffer is written from where it starts for as far as it was told,
  // and the bytes behind it are none of its business.
  const char names[] = "hostportdebug";

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), names + 0, 4, "arnm");
  arnm_json_writer_add_uint64(owner.writer(), names + 4, 4, 8443);
  arnm_json_writer_add_bool(owner.writer(), names + 8, 5, true);

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()), "{\"host\":\"arnm\",\"port\":8443,\"debug\":true}"
  );
}

TEST(JsonWriter, AKeyNeedsNoTerminatorAtAll) {
  // no NUL anywhere in this buffer; a writer that reached for one would run into the next test
  const char key[3] = {'k', 'e', 'y'};

  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), key, sizeof(key), 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"key\":1}");
}

TEST(JsonWriter, AShorterLengthTruncatesTheKeyRatherThanBeingCaught) {
  // the length is taken at its word. Nothing checks it against the key, so a wrong one is a
  // wrong name in the document and not a refusal -- which is what the header warns about.
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), "port", 2, 8443);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"po\":8443}");
}

TEST(JsonWriter, AnEmptyKeyIsAName) {
  // "" is a legal member name in JSON, and it is not the same thing as NULL -- one names a
  // member, the other says the container has no names at all
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), "", 0, 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"\":1}");
}

TEST(JsonWriter, AKeyIsBorrowedLikeEveryOtherString) {
  char key[] = "first";

  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), key, 5, 1);
  // nothing was copied, so the name is read at the write and not at the add
  std::memcpy(key, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"SECON\":1}")
      << "a copied value does not make its key a copy too";
}

// ---------------------------------------------------------------------------
// strings: borrowed, or copied
// ---------------------------------------------------------------------------

TEST(JsonWriter, AStringIsBorrowedWhereItLies) {
  char buffer[] = "first";

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "value", 5, buffer);
  // nothing was copied, so changing the source before the write changes what is written -- the
  // plainest proof there is that the pointer is all the writer kept
  std::memcpy(buffer, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"SECON\"}");
}

TEST(JsonWriter, ACopiedStringStandsOnItsOwn) {
  char buffer[] = "first";

  ArenaWriter owner;
  arnm_json_writer_add_string_copy(owner.writer(), "value", 5, buffer);
  std::memcpy(buffer, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"first\"}");
}

TEST(JsonWriter, AStringMayHoldAnEmbeddedNul) {
  const char value[] = "be\0fore";

  ArenaWriter owner;
  arnm_json_writer_add_string_length(owner.writer(), "borrowed", 8, value, 7);
  arnm_json_writer_add_string_copy_length(owner.writer(), "copied", 6, value, 7);

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"borrowed\":\"be\\u0000fore\",\"copied\":\"be\\u0000fore\"}"
  );
}

// ---------------------------------------------------------------------------
// raw: bytes that are already JSON
// ---------------------------------------------------------------------------

TEST(JsonWriter, RawBytesGoIntoTheTextExactlyAsTheyStand) {
  // nothing is quoted and nothing is escaped, so a fragment arrives carrying whatever it needs
  // to be a JSON value on its own -- an object, an array, a number, or a string with its quotes
  ArenaWriter owner;
  arnm_json_writer_add_string_raw(owner.writer(), "object", 6, "{\"a\":1}", 7);
  arnm_json_writer_add_string_raw(owner.writer(), "array", 5, "[1,2]", 5);
  arnm_json_writer_add_string_raw(owner.writer(), "number", 6, "1.50000", 7);
  arnm_json_writer_add_string_raw(owner.writer(), "text", 4, "\"arnm\"", 6);

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"object\":{\"a\":1},\"array\":[1,2],\"number\":1.50000,\"text\":\"arnm\"}"
  ) << "a number written by hand keeps the precision no flag in this header could ask for";
}

TEST(JsonWriter, RawTakesItsPlaceInArraysAndUnderPretty) {
  ArenaWriter owner(ARNM_JSON_WRITE_PRETTY_TWO_SPACES);
  arnm_json_writer_open_array(owner.writer(), "cached", 6);
  arnm_json_writer_add_string_raw(owner.writer(), nullptr, 0, "{\"a\":1}", 7);
  arnm_json_writer_add_string_raw(owner.writer(), nullptr, 0, "2", 1);
  arnm_json_writer_close(owner.writer());

  // the fragment is laid down whole: the layout puts it on its own line but does not reach
  // inside it, so the object it holds stays minified
  EXPECT_EQ(
      Write(owner.writer(), owner.arena()), "{\n  \"cached\": [\n    {\"a\":1},\n    2\n  ]\n}"
  );
}

TEST(JsonWriter, RawBorrowsItsBytesLikeEveryOtherString) {
  char fragment[] = "\"first\"";

  ArenaWriter owner;
  arnm_json_writer_add_string_raw(owner.writer(), "value", 5, fragment, 7);
  std::memcpy(fragment + 1, "SECON", 5);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"SECON\"}")
      << "there is no copying form of the raw adder, so the fragment has to stand still";
}

TEST(JsonWriter, RawOfNothingIsTheLiteralNull) {
  // the same reading a NULL gets everywhere else in this header: an optional member that is
  // not there, and not an empty fragment
  ArenaWriter owner;
  arnm_json_writer_add_string_raw(owner.writer(), "value", 5, nullptr, 0);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":null}");
}

TEST(JsonWriter, RawIsNotCheckedAndWillWriteADocumentThatIsNotJson) {
  // pinned rather than left implied. The header warns that nothing here can tell JSON from
  // anything else, and this is what that costs: the writer succeeds, the text is garbage, and
  // the reader on the far side is the first thing that notices.
  ArenaWriter owner;
  arnm_json_writer_add_string_raw(owner.writer(), "text", 4, "arnm", 4);
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);

  const std::string written = Write(owner.writer(), owner.arena());
  EXPECT_EQ(written, "{\"text\":arnm}") << "unquoted, because nothing added the quotes";

  arnm reading{};
  ASSERT_EQ(arnm_init_arena(&reading, kArenaCapacity), ARNM_SUCCESS);
  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, &reading), ARNM_SUCCESS);
  arnm_json_value *root = nullptr;
  EXPECT_NE(
      arnm_json_reader_parse(&reader, written.c_str(), written.size(), false, &root), ARNM_SUCCESS
  ) << "the writer let it through; the reader is where it stops";
  arnm_json_reader_release(&reader);
  arnm_release(&reading);
}

TEST(JsonWriter, EveryEscapeIsWrittenTheWayJsonSpellsIt) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "quote", 5, "a\"b");
  arnm_json_writer_add_string(owner.writer(), "slash", 5, "a\\b");
  arnm_json_writer_add_string(owner.writer(), "short", 5, "a\nb\tc\rd");
  arnm_json_writer_add_string(owner.writer(), "control", 7, "a\x01\x1f b");
  arnm_json_writer_add_string(owner.writer(), "path", 4, "a/b");

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\"quote\":\"a\\\"b\",\"slash\":\"a\\\\b\",\"short\":\"a\\nb\\tc\\rd\","
      "\"control\":\"a\\u0001\\u001F b\",\"path\":\"a/b\"}"
  );
}

TEST(JsonWriter, ASlashIsEscapedOnlyWhenAskedFor) {
  ArenaWriter owner(ARNM_JSON_WRITE_ESCAPE_SLASHES);
  arnm_json_writer_add_string(owner.writer(), "path", 4, "/usr/bin");
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"path\":\"\\/usr\\/bin\"}");
}

TEST(JsonWriter, UnicodeIsCopiedThroughRatherThanEscaped) {
  // two bytes, three bytes and four bytes of UTF-8, copied rather than escaped: every byte of
  // the input is one byte of the output
  const char *value = "\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "text", 4, value);
  const std::string written = Write(owner.writer(), owner.arena());
  EXPECT_EQ(written, std::string("{\"text\":\"") + value + "\"}");
}

TEST(JsonWriter, EscapedUnicodeBecomesTheSurrogatePairsThatSpellIt) {
  const char *value = "\xC3\xA4\xF0\x9F\x98\x80";

  ArenaWriter owner(ARNM_JSON_WRITE_ESCAPE_UNICODE);
  arnm_json_writer_add_string(owner.writer(), "text", 4, value);
  // one escape per code point, and a code point past the basic plane spells itself as the
  // surrogate pair JSON has for it -- two escapes for the four bytes that went in
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"text\":\"\\u00E4\\uD83D\\uDE00\"}");
}

// ---------------------------------------------------------------------------
// hex: formatted where it is written
// ---------------------------------------------------------------------------

TEST(JsonWriter, HexIsTwoLowercaseCharactersPerByteInOrder) {
  uint8_t bytes[] = {0x00, 0x0f, 0x10, 0xa5, 0xff};

  ArenaWriter owner;
  arnm_json_writer_add_hex(owner.writer(), "value", 5, bytes, sizeof(bytes));
  // the bytes are read where they are added and never again, so changing them afterwards
  // changes nothing about what comes out
  std::memset(bytes, 0, sizeof(bytes));

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"value\":\"000f10a5ff\"}");
}

TEST(JsonWriter, HexMatchesTheConverterThatWritesItElsewhere) {
  std::vector<uint8_t> bytes(64);
  for (size_t index = 0; index < bytes.size(); ++index) { bytes[index] = (uint8_t)(index * 7u); }

  std::string expected(bytes.size() * 2u + 1u, '\0');
  const arnm_memory_block block{bytes.data(), (uint32_t)bytes.size()};
  ASSERT_EQ(arnm_binary_to_hex(expected.data(), &block), ARNM_SUCCESS);
  expected.resize(bytes.size() * 2u);

  ArenaWriter owner;
  arnm_json_writer_add_hex(owner.writer(), "h", 1, bytes.data(), (uint32_t)bytes.size());

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"h\":\"" + expected + "\"}");
}

TEST(JsonWriter, NoBytesIsTheEmptyStringAndNotNull) {
  const uint8_t byte = 0x42;

  ArenaWriter owner;
  arnm_json_writer_add_hex(owner.writer(), "empty", 5, &byte, 0);
  arnm_json_writer_add_hex(owner.writer(), "absent", 6, nullptr, 4);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"empty\":\"\",\"absent\":\"\"}");
}

TEST(JsonWriter, HexTakesItsPlaceInArraysAndUnderPretty) {
  const uint8_t bytes[] = {0xde, 0xad};

  ArenaWriter owner(ARNM_JSON_WRITE_PRETTY_TWO_SPACES);
  arnm_json_writer_open_array(owner.writer(), "list", 4);
  arnm_json_writer_add_hex(owner.writer(), nullptr, 0, bytes, sizeof(bytes));
  arnm_json_writer_add_hex(owner.writer(), nullptr, 0, bytes, sizeof(bytes));
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()), "{\n  \"list\": [\n    \"dead\",\n    \"dead\"\n  ]\n}"
  );
}

TEST(JsonWriter, HexDoesNotAskTheSerializerForSixTimesItsLength) {
  // The serializer reserves six bytes per character of a string, for the case where every one
  // of them escapes to \uXXXX. Hex escapes to nothing and goes in as a raw value, which is
  // reserved for at one byte per character -- the whole point of this call existing.
  //
  // An arena that fits the second and not the first is the plainest way to hold that apart:
  // 1 KiB of bytes is 2048 characters, which the string path asks about 12 KiB of working
  // buffer for and this path about 2 KiB.
  std::vector<uint8_t> bytes(1024);
  for (size_t index = 0; index < bytes.size(); ++index) { bytes[index] = (uint8_t)index; }

  constexpr uint32_t kTightCapacity = 8 * 1024;

  {
    alignas(8) uint8_t storage[kTightCapacity] = {0};
    arnm arena{};
    ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
    arnm_json_writer writer{};
    ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT, NULL), ARNM_SUCCESS);

    arnm_json_writer_add_hex(&writer, "payload", 7, bytes.data(), (uint32_t)bytes.size());
    arnm_memory_block block{};
    uint32_t length = 0;
    EXPECT_EQ(arnm_json_writer_write(&writer, &arena, &block, &length), ARNM_SUCCESS);
    EXPECT_EQ(length, bytes.size() * 2u + 14u) << R"({"payload":"..."})" << " around the hex";

    arnm_json_writer_release(&writer);
    arnm_release(&arena);
  }

  {
    std::string hex(bytes.size() * 2u + 1u, '\0');
    const arnm_memory_block source{bytes.data(), (uint32_t)bytes.size()};
    ASSERT_EQ(arnm_binary_to_hex(hex.data(), &source), ARNM_SUCCESS);

    alignas(8) uint8_t storage[kTightCapacity] = {0};
    arnm arena{};
    ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
    arnm_json_writer writer{};
    ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT, NULL), ARNM_SUCCESS);

    arnm_json_writer_add_string_copy(&writer, "payload", 7, hex.c_str());
    arnm_memory_block block{};
    // the same document, the same arena, refused for the room the string path asks for
    EXPECT_EQ(arnm_json_writer_write(&writer, &arena, &block, nullptr), ARNM_ERROR_OUT_OF_MEMORY);

    arnm_json_writer_release(&writer);
    arnm_release(&arena);
  }
}

// ---------------------------------------------------------------------------
// base64: a payload rather than a value to read
// ---------------------------------------------------------------------------

TEST(JsonWriter, Base64IsTheStandardAlphabetWithPadding) {
  uint8_t bytes[] = {'f', 'o', 'o', 'b'};

  ArenaWriter owner;
  arnm_json_writer_add_base64(owner.writer(), "payload", 7, bytes, sizeof(bytes));
  std::memset(bytes, 0, sizeof(bytes));

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"payload\":\"Zm9vYg==\"}");
}

TEST(JsonWriter, Base64MatchesTheConverterThatWritesItElsewhere) {
  std::vector<uint8_t> bytes(200);
  for (size_t i = 0; i < bytes.size(); ++i) { bytes[i] = (uint8_t)(i * 11u); }

  std::string expected(ARNM_BASE64_STRING_LENGTH(bytes.size()) + 1u, '\0');
  const arnm_memory_block block{bytes.data(), (uint32_t)bytes.size()};
  ASSERT_EQ(arnm_binary_to_base64(expected.data(), &block), ARNM_SUCCESS);
  expected.resize(ARNM_BASE64_STRING_LENGTH(bytes.size()));

  ArenaWriter owner;
  arnm_json_writer_add_base64(owner.writer(), "p", 1, bytes.data(), (uint32_t)bytes.size());

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"p\":\"" + expected + "\"}");
}

TEST(JsonWriter, Base64OfNoBytesIsTheEmptyString) {
  const uint8_t byte = 0x42;

  ArenaWriter owner;
  arnm_json_writer_add_base64(owner.writer(), "empty", 5, &byte, 0);
  arnm_json_writer_add_base64(owner.writer(), "absent", 6, nullptr, 4);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"empty\":\"\",\"absent\":\"\"}");
}

TEST(JsonWriter, Base64CostsAThirdLessThanTheSameBytesAsHex) {
  std::vector<uint8_t> bytes(600);
  for (size_t i = 0; i < bytes.size(); ++i) { bytes[i] = (uint8_t)i; }

  ArenaWriter as_hex;
  arnm_json_writer_add_hex(as_hex.writer(), "p", 1, bytes.data(), (uint32_t)bytes.size());
  const size_t hex_length = Write(as_hex.writer(), as_hex.arena()).size();

  ArenaWriter as_base64;
  arnm_json_writer_add_base64(as_base64.writer(), "p", 1, bytes.data(), (uint32_t)bytes.size());
  const size_t base64_length = Write(as_base64.writer(), as_base64.arena()).size();

  // 1200 characters against 800, the envelope the same in both
  EXPECT_EQ(hex_length - base64_length, 400u);
}

// ---------------------------------------------------------------------------
// uuids: the canonical form, formatted where it is written
// ---------------------------------------------------------------------------

TEST(JsonWriter, AUuidTakesTheCanonicalDashedForm) {
  uint8_t uuid[ARNM_UUID_BINARY_SIZE];
  for (uint8_t index = 0; index < ARNM_UUID_BINARY_SIZE; ++index) {
    uuid[index] = (uint8_t)(0x10u + index * 0x11u);
  }

  std::string expected(ARNM_UUID_STRING_LENGTH + 1u, '\0');
  arnm_uuid_to_string(expected.data(), uuid);
  expected.resize(ARNM_UUID_STRING_LENGTH);

  ArenaWriter owner;
  arnm_json_writer_add_uuid(owner.writer(), "id", 2, uuid);
  // read where it is added and never again
  std::memset(uuid, 0, sizeof(uuid));

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"id\":\"" + expected + "\"}");
}

TEST(JsonWriter, AnAbsentUuidIsNullAndNotAnEmptyString) {
  // unlike a block of no bytes, which add_hex writes as "": a uuid is sixteen bytes or it is
  // not there, and there is no size here that could tell those apart
  ArenaWriter owner;
  arnm_json_writer_add_uuid(owner.writer(), "id", 2, nullptr);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"id\":null}");
}

TEST(JsonWriter, UuidsTakeTheirPlaceInArraysAndUnderPretty) {
  const uint8_t uuid[ARNM_UUID_BINARY_SIZE] = {0};

  ArenaWriter owner(ARNM_JSON_WRITE_PRETTY_TWO_SPACES);
  arnm_json_writer_open_array(owner.writer(), "ids", 3);
  arnm_json_writer_add_uuid(owner.writer(), nullptr, 0, uuid);
  arnm_json_writer_add_uuid(owner.writer(), nullptr, 0, nullptr);
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(
      Write(owner.writer(), owner.arena()),
      "{\n  \"ids\": [\n    \"00000000-0000-0000-0000-000000000000\",\n    null\n  ]\n}"
  );
}

// ---------------------------------------------------------------------------
// the pool hint
// ---------------------------------------------------------------------------

namespace {

/** Build one small document and answer what the arena had to give up for it. */
uint32_t DocumentCost(const arnm_json_writer_hint *hint) {
  alignas(8) static uint8_t storage[8 * 1024];
  arnm arena{};
  EXPECT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
  const uint32_t before = arnm_arena_remaining(&arena);

  arnm_json_writer writer{};
  EXPECT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT, hint), ARNM_SUCCESS);
  arnm_json_writer_begin_object(&writer);
  for (uint32_t index = 0; index < 12; ++index) {
    arnm_json_writer_add_string(&writer, "k", 1, "0123456789abcdef");
  }
  // read while the document stands: the write itself would add the text and its working buffer
  const uint32_t cost = before - arnm_arena_remaining(&arena);
  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_SUCCESS);

  arnm_json_writer_release(&writer);
  arnm_release(&arena);
  return cost;
}

} // namespace

TEST(JsonWriter, AHintOpensThePoolsOnceInsteadOfDoubling) {
  // 12 borrowed strings under one key each: 25 values, and not a byte of copied string
  const arnm_json_writer_hint hint{25, 0};

  const uint32_t hinted = DocumentCost(&hint);
  const uint32_t grown = DocumentCost(nullptr);

  // without the hint the value pool opens 16 slots, then 32, then 64, and keeps all three
  EXPECT_LT(hinted, grown) << "the hint should replace the chunk series with one chunk";
}

TEST(JsonWriter, AHintThatIsWrongChangesNothingAboutTheDocument) {
  const uint8_t bytes[] = {0xab, 0xcd};
  const arnm_json_writer_hint far_too_small{1, 1};
  const arnm_json_writer_hint far_too_large{4096, 8192};

  for (const arnm_json_writer_hint *hint : {&far_too_small, &far_too_large}) {
    ArenaWriter owner(ARNM_JSON_WRITE_DEFAULT, hint);
    arnm_json_writer_add_string_copy(owner.writer(), "text", 4, "value");
    arnm_json_writer_add_hex(owner.writer(), "bytes", 5, bytes, sizeof(bytes));
    arnm_json_writer_add_int64(owner.writer(), "n", 1, -7);

    EXPECT_EQ(
        Write(owner.writer(), owner.arena()), "{\"text\":\"value\",\"bytes\":\"abcd\",\"n\":-7}"
    );
  }
}

TEST(JsonWriter, AHintOfZerosIsTheSameAsNone) {
  const arnm_json_writer_hint none{0, 0};
  EXPECT_EQ(DocumentCost(&none), DocumentCost(nullptr));
}

TEST(JsonWriter, AHintYyjsonCannotServeLeavesTheDefaultGrowth) {
  // past what a chunk can be counted in; refused where it is set and never reaches the write
  const arnm_json_writer_hint absurd{UINT32_MAX, UINT32_MAX};

  ArenaWriter owner(ARNM_JSON_WRITE_DEFAULT, &absurd);
  arnm_json_writer_add_int64(owner.writer(), "n", 1, 1);

  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"n\":1}");
}

// ---------------------------------------------------------------------------
// the first error, kept
// ---------------------------------------------------------------------------

TEST(JsonWriter, TheFirstRefusalIsTheOneThatStays) {
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), "good", 4, 1);

  // an element without a name, inside an object that needs one
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 0, 2);
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "Missing key for object container")
      << "a field with no key belongs to no name, and the sentinel is the name it is filed under";

  // everything after it does nothing at all, and changes nothing about the verdict
  arnm_json_writer_add_string(owner.writer(), "later", 5, "value");
  arnm_json_writer_open_object(owner.writer(), "deeper", 6);
  arnm_json_writer_close(owner.writer());
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "Missing key for object container");

  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_writer_write(owner.writer(), owner.arena(), &block, nullptr),
      ARNM_ERROR_INVALID_PARAM
  ) << "a writer carrying an error refuses to write, so the one result stands in for the check";
  EXPECT_EQ(block.data, nullptr) << "a refusal leaves every output untouched";

  // cleared, the writing counts again -- and what was refused stayed out
  ASSERT_EQ(arnm_json_writer_clear_error(owner.writer()), ARNM_SUCCESS);
  arnm_json_writer_add_uint64(owner.writer(), "after", 5, 3);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"good\":1,\"after\":3}");
}

TEST(JsonWriter, ANameInsideAnArrayIsRefusedByTheContainer) {
  ArenaWriter owner;
  arnm_json_writer_open_array(owner.writer(), "list", 4);
  arnm_json_writer_add_uint64(owner.writer(), "named", 5, 1);

  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "Not null key for array container");
}

TEST(JsonWriter, ARefusalWithNoKeyIsFiledUnderTheArraySentinel) {
  // A field added to an array has no name to record a refusal under, so the writer supplies
  // one. It is the sentinel and not the empty string, because the empty string is what a
  // refusal belonging to no field at all wears -- and the two say different things.
  ArenaWriter deep;
  arnm_json_writer_open_array(deep.writer(), "list", 4);
  for (uint32_t level = 2; level < ARNM_JSON_WRITER_MAX_DEPTH; ++level) {
    arnm_json_writer_open_array(deep.writer(), nullptr, 0);
  }
  arnm_json_writer_open_array(deep.writer(), nullptr, 0);
  EXPECT_EQ(arnm_json_writer_status(deep.writer()), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_STREQ(arnm_json_writer_error_field(deep.writer()), "[]");

  // one close too many belongs to no field, and reads as the empty string
  ArenaWriter closed;
  arnm_json_writer_open_object(closed.writer(), "inner", 5);
  arnm_json_writer_close(closed.writer());
  arnm_json_writer_close(closed.writer());
  EXPECT_EQ(arnm_json_writer_status(closed.writer()), ARNM_ERROR_INVALID_STATE);
  EXPECT_STREQ(arnm_json_writer_error_field(closed.writer()), "");
}

TEST(JsonWriter, OneCloseTooManyIsRecordedRatherThanSwallowed) {
  ArenaWriter owner;
  arnm_json_writer_open_object(owner.writer(), "inner", 5);
  arnm_json_writer_close(owner.writer());
  arnm_json_writer_close(owner.writer());

  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_STATE)
      << "a silent extra close would move the next field somewhere nobody expects";
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 1u);
}

TEST(JsonWriter, OpeningPastTheLastLevelIsRefused) {
  ArenaWriter owner;
  for (uint32_t level = 1; level < ARNM_JSON_WRITER_MAX_DEPTH; ++level) {
    arnm_json_writer_open_object(owner.writer(), "down", 4);
    EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS) << "at level " << level;
  }
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), ARNM_JSON_WRITER_MAX_DEPTH);

  arnm_json_writer_open_object(owner.writer(), "one_too_deep", 13);
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "one_too_deep");
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), ARNM_JSON_WRITER_MAX_DEPTH);
}

TEST(JsonWriter, AnArenaWithNoRoomIsRecordedAsOutOfMemory) {
  uint8_t storage[64] = {0};
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_json_writer writer{};
  ASSERT_EQ(arnm_json_writer_init(&writer, &arena, ARNM_JSON_WRITE_DEFAULT, NULL), ARNM_SUCCESS);
  for (uint32_t index = 0; index < 64; ++index) {
    arnm_json_writer_add_uint64(&writer, "n", 1, index);
  }

  EXPECT_EQ(arnm_json_writer_status(&writer), ARNM_ERROR_OUT_OF_MEMORY);
  arnm_json_writer_release(&writer);
  arnm_release(&arena);
}

TEST(JsonWriter, ANonFiniteNumberIsRefusedOrWrittenAsNull) {
  // yyjson is built here with YYJSON_DISABLE_NON_STANDARD, which takes out the code behind
  // YYJSON_WRITE_ALLOW_INF_AND_NAN -- there is no longer a way to spell `Infinity` in the
  // output, so the two answers left are a refusal and a null.
  const double infinity = 1e308 * 10.0;

  ArenaWriter strict;
  arnm_json_writer_add_double(strict.writer(), "n", 1, infinity);
  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_writer_write(strict.writer(), strict.arena(), &block, nullptr),
      ARNM_ERROR_ENCODE_FAILED
  );
  EXPECT_EQ(block.data, nullptr) << "a refused write hands back nothing to release";

  // standard JSON has a spelling for this one, so it survives the build that removed the other
  ArenaWriter as_null(ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL);
  arnm_json_writer_add_double(as_null.writer(), "n", 1, infinity);
  EXPECT_EQ(Write(as_null.writer(), as_null.arena()), "{\"n\":null}");

  ArenaWriter nan_as_null(ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL);
  arnm_json_writer_add_double(nan_as_null.writer(), "n", 1, infinity - infinity);
  EXPECT_EQ(Write(nan_as_null.writer(), nan_as_null.arena()), "{\"n\":null}");
}

// ---------------------------------------------------------------------------
// measuring the output before it exists
// ---------------------------------------------------------------------------

TEST(JsonWriter, TheEstimateIsThereBeforeAByteOfTextExists) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", 4, "arnm");
  arnm_json_writer_add_uint64(owner.writer(), "port", 4, 8443);

  // free to ask, and answered from a count the adders already kept -- the document is never
  // walked and no string is ever measured
  EXPECT_EQ(arnm_json_writer_buffer_size_min(owner.writer()), 160u);
  EXPECT_EQ(Write(owner.writer(), owner.arena()).size(), 27u);
}

TEST(JsonWriter, TheEstimateGrowsWithEveryFieldAndNeverWalksTheDocument) {
  ArenaWriter owner;
  ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);

  uint32_t previous = arnm_json_writer_buffer_size_min(owner.writer());
  EXPECT_EQ(previous, 96u);
  for (uint32_t index = 0; index < 32; ++index) {
    arnm_json_writer_add_uint64(owner.writer(), "key", 3, index);
    const uint32_t now = arnm_json_writer_buffer_size_min(owner.writer());
    EXPECT_GT(now, previous) << "at field " << index;
    previous = now;
  }
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  // a document of short keys and short integers is where the flat charge per element is
  // generous, so here the guess comes out above the text -- which is one direction of two
  EXPECT_GT(previous, Write(owner.writer(), owner.arena()).size() + 1u);
}

TEST(JsonWriter, EveryShapeAndEveryLayoutComesOutWhole) {
  // the same document under every layout the flags allow: every one of them writes, and every
  // one of them comes back terminated at the length it reported
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
    arnm_json_writer_add_string(owner.writer(), "name", 4, "a name with \"quotes\" and a \\ and /");
    arnm_json_writer_add_string(owner.writer(), "lines", 5, "one\ntwo\tthree\x01");
    arnm_json_writer_add_uint64(owner.writer(), "big", 3, UINT64_MAX);
    arnm_json_writer_add_int64(owner.writer(), "small", 5, INT64_MIN);
    arnm_json_writer_add_bool(owner.writer(), "yes", 3, true);
    arnm_json_writer_add_bool(owner.writer(), "no", 2, false);
    arnm_json_writer_add_null(owner.writer(), "nothing", 7);
    arnm_json_writer_add_string(owner.writer(), "unicode", 7, "\xC3\xA4\xE2\x82\xAC");

    arnm_json_writer_open_object(owner.writer(), "empty", 5);
    arnm_json_writer_close(owner.writer());

    arnm_json_writer_open_array(owner.writer(), "list", 4);
    for (uint32_t index = 0; index < 3; ++index) {
      arnm_json_writer_open_object(owner.writer(), nullptr, 0);
      arnm_json_writer_add_uint64(owner.writer(), "index", 5, index);
      arnm_json_writer_open_array(owner.writer(), "inner", 5);
      arnm_json_writer_add_string(owner.writer(), nullptr, 0, "deep");
      arnm_json_writer_close(owner.writer());
      arnm_json_writer_close(owner.writer());
    }
    arnm_json_writer_close(owner.writer());

    ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS) << "flags " << flags;
    EXPECT_FALSE(Write(owner.writer(), owner.arena()).empty()) << "flags " << flags;
  }
}

namespace {

/**
 * The doubles that reach the longest text, in both notations the serializer chooses between.
 *
 * The exponent forms are the obvious ones and they are not the long ones. A number whose
 * decimal point falls just left of its first significant digit is written out in full instead,
 * and seventeen significant digits behind `-0.00000` is one byte longer than the largest
 * `double` ever written with an exponent -- which is the byte the ceiling exists for.
 */
const double kLongestReals[] = {
    -1.7976931348623157e308,  /* 23 -- the largest magnitude, in exponent form */
    1.7976931348623157e308,   /* 22 -- the same without the sign */
    -2.2250738585072014e-308, /* 24 -- the smallest normal, in exponent form */
    5e-324,                   /*  6 -- the smallest subnormal */
    -0.0,                     /*  4 -- the sign that survives a zero */
    1.0 / 3.0,                /* 18 -- decimal point inside the digits */
    0.0000018498776203445192, /* 24 -- fixed point, without a sign */
    -0.000012345678901234567, /* 24 -- one zero fewer behind the point */
    -0.00012345678901234567,  /* 23 */
    -1.2345678901234567e-7,   /* 22 -- just past where the exponent form takes over */
};

/**
 * The two that reach the ceiling: sign, `0.` and five zeros, then seventeen significant digits.
 *
 * Kept apart from the list above because a document mixing them with shorter numbers proves
 * nothing -- the slack the short ones leave pays for the long ones, and a ceiling one byte too
 * low still comes out ahead. A document of nothing but these has no slack to hide in.
 */
const double kCeilingReals[] = {
    -0.0000018498776203445192,
    -0.0000012345678901234567,
};

} // namespace

TEST(JsonWriter, TheLongestRealNumberStillFitsItsCharge) {
  // the ceiling is a claim about doubles, so it is checked against the ones that reach it
  std::vector<double> reaching(std::begin(kLongestReals), std::end(kLongestReals));
  reaching.insert(reaching.end(), std::begin(kCeilingReals), std::end(kCeilingReals));

  for (double value : reaching) {
    ArenaWriter owner;
    arnm_json_writer_begin_array(owner.writer());
    arnm_json_writer_add_double(owner.writer(), nullptr, 0, value);
    ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);

    // nothing copies against this ceiling any more, but it is still the number a caller sizes
    // a buffer of its own by, so a double that renders longer than it would be a lie
    const std::string text = Write(owner.writer(), owner.arena());
    ASSERT_GE(text.size(), 2u);
    EXPECT_LE(text.size() - 2u, ARNM_JSON_WRITER_MAX_NUMBER_TEXT)
        << "rendered as " << text << ", which is longer than the ceiling promises";
  }
}

TEST(JsonWriter, TheCeilingIsReachedByADoubleAndNotMerelyGuessedAt) {
  // A ceiling nobody reaches is a ceiling nobody has measured, and it would hide the next byte
  // the serializer grows by. One of these has to render exactly as long as the charge.
  size_t longest = 0;
  std::string longest_text;
  for (double value : kCeilingReals) {
    ArenaWriter owner;
    arnm_json_writer_begin_array(owner.writer());
    arnm_json_writer_add_double(owner.writer(), nullptr, 0, value);
    ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);

    const std::string text = Write(owner.writer(), owner.arena());
    ASSERT_GE(text.size(), 2u);
    if (text.size() - 2u > longest) {
      longest = text.size() - 2u;
      longest_text = text;
    }
  }
  EXPECT_EQ(longest, ARNM_JSON_WRITER_MAX_NUMBER_TEXT)
      << "the longest of them rendered as " << longest_text;
}

TEST(JsonWriter, ADocumentOfLongRealsIsWrittenWholeWhateverTheEstimateSaid) {
  // A run of doubles that each render near the ceiling is where the old exact measurement was
  // at its tightest. Nothing reserves against the estimate any more, so what is checked here is
  // the part that matters: the text comes out whole and terminated at the length it reports.
  ArenaWriter owner;
  for (int round = 0; round < 8; ++round) {
    for (double value : kCeilingReals) {
      arnm_json_writer_add_double(owner.writer(), "value", 5, value);
    }
  }
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);

  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), owner.arena(), &block, &length), ARNM_SUCCESS);
  EXPECT_EQ(std::strlen(reinterpret_cast<const char *>(block.data)), length);
  EXPECT_GE(block.size, length + 1u) << "the block has to hold the text and its terminator";
  EXPECT_EQ(arnm_memory_block_free(&block, owner.arena()), ARNM_SUCCESS);
}

TEST(JsonWriter, TheEstimateIsAGuessAndNotABound) {
  // The one property the old arnm_json_writer_size() had and this call deliberately does not.
  // Pinned rather than left implied: a caller that sizes a fixed buffer by this number and
  // copies into it is writing past the end, and the header says so for a reason.
  ArenaWriter owner;
  arnm_json_writer_add_string_length(
      owner.writer(), "n", 1, LONG_CONTENT_STRING, sizeof(LONG_CONTENT_STRING) - 1u
  );

  const uint32_t estimate = arnm_json_writer_buffer_size_min(owner.writer());
  const std::string text = Write(owner.writer(), owner.arena());
  EXPECT_GT(text.size() + 1u, estimate)
      << "one string of ordinary prose already runs past the guess, which is the point";
}

TEST(JsonWriter, TheEstimateFollowsTheLayoutItWasAskedFor) {
  // A pretty document carries an indent and a newline per element that a minified one does not,
  // and the estimate charges for them. Both pretty spellings count, which is worth its own
  // check: the flags the writer keeps are the serializer's and not this header's, and the two
  // do not put PRETTY_TWO_SPACES on the same bit.
  const arnm_json_write_flags layouts[] = {
      ARNM_JSON_WRITE_PRETTY,
      ARNM_JSON_WRITE_PRETTY_TWO_SPACES,
  };

  ArenaWriter minified(ARNM_JSON_WRITE_DEFAULT);
  arnm_json_writer_add_uint64(minified.writer(), "a", 1, 1);
  arnm_json_writer_add_uint64(minified.writer(), "b", 1, 2);
  const uint32_t flat = arnm_json_writer_buffer_size_min(minified.writer());

  for (arnm_json_write_flags flags : layouts) {
    ArenaWriter owner(flags);
    arnm_json_writer_add_uint64(owner.writer(), "a", 1, 1);
    arnm_json_writer_add_uint64(owner.writer(), "b", 1, 2);
    EXPECT_GT(arnm_json_writer_buffer_size_min(owner.writer()), flat) << "flags " << flags;
  }
}

TEST(JsonWriter, TheTextIsShrunkToWhatItActuallyNeeded) {
  ArenaWriter owner;
  for (uint32_t index = 0; index < 16; ++index) {
    arnm_json_writer_add_string(owner.writer(), "key", 3, "a value of an ordinary length");
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

TEST(JsonWriter, TheTextComesFromTheAllocatorItWasAskedOfAndNotTheWritersOwn) {
  // The document is built in one arena and the text is rendered into another, which is the
  // split the header describes. The serializer now grows the output buffer in place rather
  // than copying out of a scratch one, so the arena the text lands in is the only one that
  // moves for it.
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, kArenaCapacity), ARNM_SUCCESS);

  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", 4, "arnm");

  const uintptr_t document_before = ArenaMark(owner.arena());
  const uintptr_t output_before = ArenaMark(&output);

  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), &output, &block, &length), ARNM_SUCCESS);
  EXPECT_STREQ(reinterpret_cast<const char *>(block.data), "{\"name\":\"arnm\"}");

  EXPECT_EQ(ArenaMark(owner.arena()), document_before)
      << "the writer's own arena carries the document and nothing of the text";
  EXPECT_EQ(ArenaMark(&output) - output_before, ARNM_ALIGN8(length + 1u))
      << "and the output arena holds the text, shrunk to what it needed";

  EXPECT_EQ(arnm_memory_block_free(&block, &output), ARNM_SUCCESS);
  EXPECT_EQ(ArenaMark(&output), output_before);
  arnm_release(&output);
}

TEST(JsonWriter, TheHostCanCarryTheTextJustAsWell) {
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", 4, "arnm");

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
    arnm_json_writer_add_uint64(owner.writer(), "round", 5, round);
    EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"round\":" + std::to_string(round) + "}");
  }
}

TEST(JsonWriter, ABeginClearsWhatTheLastDocumentLeftBehind) {
  ArenaWriter owner;
  arnm_json_writer_add_uint64(owner.writer(), nullptr, 0, 1);
  ASSERT_EQ(arnm_json_writer_status(owner.writer()), ARNM_ERROR_INVALID_PARAM);

  ASSERT_EQ(arnm_json_writer_begin_object(owner.writer()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_writer_status(owner.writer()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_writer_error_field(owner.writer()), "");
  EXPECT_EQ(arnm_json_writer_depth(owner.writer()), 1u);

  arnm_json_writer_add_uint64(owner.writer(), "n", 1, 1);
  EXPECT_EQ(Write(owner.writer(), owner.arena()), "{\"n\":1}");
}

TEST(JsonWriter, WhatWasWrittenReadsBackAsWhatWentIn) {
  // the two halves of the module, back to back: what the writer put down is what the reader
  // finds, field for field
  ArenaWriter owner;
  arnm_json_writer_add_string(owner.writer(), "name", 4, "arnm");
  arnm_json_writer_add_uint64(owner.writer(), "port", 4, 8443);
  arnm_json_writer_add_int64(owner.writer(), "offset", 6, -7);
  arnm_json_writer_add_bool(owner.writer(), "debug", 5, true);
  arnm_json_writer_add_double(owner.writer(), "ratio", 5, 0.25);
  arnm_json_writer_open_array(owner.writer(), "tags", 4);
  arnm_json_writer_add_string(owner.writer(), nullptr, 0, "one");
  arnm_json_writer_add_string(owner.writer(), nullptr, 0, "two");
  arnm_json_writer_close(owner.writer());

  arnm_memory_block block{};
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_writer_write(owner.writer(), owner.arena(), &block, &length), ARNM_SUCCESS);

  arnm reading{};
  ASSERT_EQ(arnm_init_arena(&reading, kArenaCapacity), ARNM_SUCCESS);
  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, &reading), ARNM_SUCCESS);
  arnm_json_value *root = nullptr;
  ASSERT_EQ(
      arnm_json_reader_parse(
          &reader, reinterpret_cast<const char *>(block.data), length, false, &root
      ),
      ARNM_SUCCESS
  );
  ASSERT_NE(root, nullptr);

  arnm_memory_block name{};
  uint64_t port = 0;
  int64_t offset = 0;
  bool debug = false;
  double ratio = 0.0;
  arnm_json_value *tags = nullptr;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_STRING("name", &name),    ARNM_JSON_FIELD_UINT64("port", &port),
      ARNM_JSON_FIELD_INT64("offset", &offset), ARNM_JSON_FIELD_BOOL("debug", &debug),
      ARNM_JSON_FIELD_DOUBLE("ratio", &ratio),  ARNM_JSON_FIELD_VALUE("tags", &tags)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 6, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3full) << "every member the writer put there came back";

  EXPECT_EQ(std::string(reinterpret_cast<const char *>(name.data), name.size), "arnm");
  EXPECT_EQ(port, 8443u);
  EXPECT_EQ(offset, -7);
  EXPECT_TRUE(debug);
  EXPECT_DOUBLE_EQ(ratio, 0.25);

  ASSERT_NE(tags, nullptr);
  arnm_json_value *tag[2] = {nullptr, nullptr};
  uint32_t tag_count = 0;
  ASSERT_EQ(arnm_json_read_array(tags, tag, 2, &tag_count), ARNM_SUCCESS);
  EXPECT_EQ(tag_count, 2u);
  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_SUCCESS);

  arnm_json_reader_release(&reader);
  arnm_release(&reading);
  EXPECT_EQ(arnm_memory_block_free(&block, owner.arena()), ARNM_SUCCESS);
}
