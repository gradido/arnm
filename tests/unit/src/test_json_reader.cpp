#include "arnm/arena.h"
#include "arnm/converter.h"
#include "arnm/json_reader.h"
#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// The reader is opaque from here on purpose: these tests only ever see what a consumer sees.
// What the whole file is really checking is that the model holds -- a reader carries a document
// and nothing else, a parse hands back a root, and the two walks are the only way down from it.

namespace {

/** Room for every document below, including the widest table and the longest string. */
constexpr uint32_t kArenaCapacity = 1024 * 1024;

/** A reader over an arena, torn down in the right order at the end of a scope. */
class ArenaReader {
public:
  ArenaReader() {
    EXPECT_EQ(arnm_init_arena(&arena_, kArenaCapacity), ARNM_SUCCESS);
    base_ = Mark();
    EXPECT_EQ(arnm_json_reader_init(&reader_, &arena_), ARNM_SUCCESS);
  }
  ~ArenaReader() {
    (void)arnm_json_reader_release(&reader_);
    arnm_release(&arena_);
  }
  ArenaReader(const ArenaReader &) = delete;
  ArenaReader &operator=(const ArenaReader &) = delete;

  arnm_json_reader *reader() {
    return &reader_;
  }
  arnm *arena() {
    return &arena_;
  }

  /**
   * Where the arena's index stands, as an address.
   *
   * Measured through the public interface alone: what a one byte allocation is handed is the
   * index itself, and giving it straight back leaves the arena as it was found.
   */
  uintptr_t Mark() {
    uint8_t *probe = nullptr;
    EXPECT_EQ(arnm_alloc(&probe, 1, &arena_), ARNM_SUCCESS);
    EXPECT_EQ(arnm_free(probe, 1, &arena_), ARNM_SUCCESS);
    return reinterpret_cast<uintptr_t>(probe);
  }

  /** Bytes the arena has handed out and not taken back since this reader was made. */
  uint32_t used() {
    return static_cast<uint32_t>(Mark() - base_);
  }

private:
  arnm arena_{};
  arnm_json_reader reader_{};
  uintptr_t base_ = 0;
};

/** Parse a literal, the copying way, and hand back the root. */
arnm_json_value *Parse(ArenaReader &owner, const std::string &json, arnm_result *out = nullptr) {
  arnm_json_value *root = nullptr;
  const arnm_result result = arnm_json_reader_parse(
      owner.reader(), json.c_str(), static_cast<uint32_t>(json.size()), false, &root
  );
  if (out) { *out = result; }
  return (ARNM_SUCCESS == result) ? root : nullptr;
}

/** A writable buffer carrying `json` plus the padding the insitu path writes through. */
std::vector<char> InsituBuffer(const std::string &json) {
  std::vector<char> buffer(json.size() + ARNM_JSON_READER_INSITU_PADDING, '\0');
  std::memcpy(buffer.data(), json.data(), json.size());
  return buffer;
}

/** The string a borrowed block stands for, so a test can compare it to a literal. */
std::string Borrowed(const arnm_memory_block &block) {
  return std::string(reinterpret_cast<const char *>(block.data), block.size);
}

} // namespace

// ---------------------------------------------------------------------------
// starting and ending a session
// ---------------------------------------------------------------------------

// promise: a reader is ready the moment it is initialized, and uninitialized storage is a valid
// input -- every field is written and none is read
TEST(JsonReader, InitPreparesUninitializedStorage) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);

  arnm_json_reader reader;
  std::memset(&reader, 0xa5, sizeof(reader));
  ASSERT_EQ(arnm_json_reader_init(&reader, &arena), ARNM_SUCCESS);

  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_SUCCESS) << "nothing has been refused yet";
  EXPECT_FALSE(arnm_json_reader_has_document(&reader));
  EXPECT_EQ(arnm_json_reader_value_count(&reader), 0u);
  EXPECT_EQ(arnm_json_reader_bytes_read(&reader), 0u);
  EXPECT_STREQ(arnm_json_reader_error_message(&reader), "no error");

  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonReader, InitRefusesNoStorage) {
  EXPECT_EQ(arnm_json_reader_init(nullptr, nullptr), ARNM_ERROR_NULL_POINTER);
}

// promise: a reader that was never initialized is refused rather than walked, and a zeroed one
// counts as never initialized
TEST(JsonReader, AnUninitializedReaderIsRefusedEverywhere) {
  arnm_json_reader reader;
  std::memset(&reader, 0, sizeof(reader));

  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_destroy(&reader, nullptr), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_FALSE(arnm_json_reader_has_document(&reader));
  EXPECT_EQ(arnm_json_reader_value_count(&reader), 0u);

  arnm_json_value *root = nullptr;
  EXPECT_EQ(arnm_json_reader_parse(&reader, "{}", 2, false, &root), ARNM_ERROR_NOT_INITIALIZED);

  // and NULL is accepted by the two that end a session, because ending nothing is not an error
  EXPECT_EQ(arnm_json_reader_release(nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_destroy(nullptr, nullptr), ARNM_SUCCESS);
}

// promise: create carves the reader out of the same allocator its documents come from, and
// destroy gives both back
TEST(JsonReader, CreateAndDestroyReturnEveryByte) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);

  uint8_t *probe = nullptr;
  ASSERT_EQ(arnm_alloc(&probe, 1, &arena), ARNM_SUCCESS);
  const uintptr_t base = reinterpret_cast<uintptr_t>(probe);
  ASSERT_EQ(arnm_free(probe, 1, &arena), ARNM_SUCCESS);

  arnm_json_reader *reader = arnm_json_reader_create(&arena);
  ASSERT_NE(reader, nullptr);

  // in place, because a copying parse leaves its string pool buried under the reader's own bytes
  // and destroy would then report the arena warning instead -- see the test further down
  char buffer[16] = "{\"a\":1}";
  arnm_json_value *root = nullptr;
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(reader, buffer, 7, sizeof(buffer), false, &root), ARNM_SUCCESS
  );
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(arnm_json_reader_has_document(reader));

  EXPECT_EQ(arnm_json_reader_destroy(reader, &arena), ARNM_SUCCESS);

  ASSERT_EQ(arnm_alloc(&probe, 1, &arena), ARNM_SUCCESS);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(probe), base) << "the arena is where it started";
  ASSERT_EQ(arnm_free(probe, 1, &arena), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonReader, CreateAnswersNullWhenTheArenaHasNoRoom) {
  arnm arena{};
  uint8_t storage[64];
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_create(&arena), nullptr)
      << "a reader needs more than this arena has, and nothing is left behind on that path";
  arnm_release(&arena);
}

// promise: releasing empties the reader without ending it -- parsing again needs no second init
TEST(JsonReader, AReleasedReaderParsesAgain) {
  ArenaReader owner;
  ASSERT_NE(Parse(owner, "{\"a\":1}"), nullptr);
  EXPECT_TRUE(arnm_json_reader_has_document(owner.reader()));

  // the copying parse took a string pool below the value buffer, so the arena cannot take the
  // whole document back -- the release happened, the memory did not come home
  EXPECT_EQ(arnm_json_reader_release(owner.reader()), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()));

  arnm_json_value *root = Parse(owner, "[1,2,3]");
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(arnm_json_reader_has_document(owner.reader()));
  EXPECT_EQ(arnm_json_reader_value_count(owner.reader()), 4u) << "the array and its three";
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

// promise: a parse hands back the root, and the reader can be asked what it read
TEST(JsonReader, AParseHandsBackTheRootAndDescribesTheDocument) {
  ArenaReader owner;
  const std::string json = "{\"a\":[1,2]}";
  arnm_json_value *root = Parse(owner, json);
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  EXPECT_TRUE(arnm_json_reader_has_document(owner.reader()));
  EXPECT_EQ(arnm_json_reader_bytes_read(owner.reader()), json.size());
  EXPECT_EQ(arnm_json_reader_value_count(owner.reader()), 5u)
      << "the object, the key, the array, and the two numbers -- a key is a node as well";
}

// promise: the root is optional -- a caller that only wants to know the bytes parse may say so
TEST(JsonReader, TheRootMayBeDeclined) {
  ArenaReader owner;
  EXPECT_EQ(arnm_json_reader_parse(owner.reader(), "{\"a\":1}", 7, false, nullptr), ARNM_SUCCESS);
  EXPECT_TRUE(arnm_json_reader_has_document(owner.reader()));
}

TEST(JsonReader, AParseRefusesWhatItCannotCarry) {
  ArenaReader owner;
  arnm_json_value *root = reinterpret_cast<arnm_json_value *>(0x1);

  EXPECT_EQ(arnm_json_reader_parse(nullptr, "{}", 2, false, &root), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(
      arnm_json_reader_parse(owner.reader(), nullptr, 2, false, &root), ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_json_reader_parse(owner.reader(), "{}", 0, false, &root), ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      arnm_json_reader_parse(
          owner.reader(), "{}", ARNM_JSON_READER_MAX_INPUT_SIZE + 1u, false, &root
      ),
      ARNM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(root, reinterpret_cast<arnm_json_value *>(0x1)) << "a refused parse writes no root";
}

// promise: a refused parse leaves the reader empty rather than still answering for the document
// before it, and says where it went wrong
TEST(JsonReader, ARefusedParseEmptiesTheReaderAndSaysWhy) {
  ArenaReader owner;
  ASSERT_NE(Parse(owner, "{\"a\":1}"), nullptr);
  ASSERT_TRUE(arnm_json_reader_has_document(owner.reader()));

  arnm_result result = ARNM_SUCCESS;
  EXPECT_EQ(Parse(owner, "{\"a\":", &result), nullptr);
  EXPECT_EQ(result, ARNM_ERROR_DECODE_FAILED);

  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()))
      << "answering the previous document after a failed parse is the state nobody checks for";
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_DECODE_FAILED);
  EXPECT_STRNE(arnm_json_reader_error_message(owner.reader()), "no error");
  EXPECT_EQ(arnm_json_reader_value_count(owner.reader()), 0u);

  // and a parse that holds clears the refusal behind it
  ASSERT_NE(Parse(owner, "{\"b\":2}"), nullptr);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_error_message(owner.reader()), "no error");
  EXPECT_EQ(arnm_json_reader_error_position(owner.reader()), 0u);
}

TEST(JsonReader, TheErrorPositionPointsIntoTheInput) {
  ArenaReader owner;
  arnm_result result = ARNM_SUCCESS;
  EXPECT_EQ(Parse(owner, "[1,2,oops]", &result), nullptr);
  EXPECT_EQ(result, ARNM_ERROR_DECODE_FAILED);
  EXPECT_EQ(arnm_json_reader_error_position(owner.reader()), 5u)
      << "the offset of the first byte that could not be read";
}

// promise: strict RFC 8259, and not askable otherwise -- the extensions are gone from the parser
// rather than defaulted off, so there is no switch for them here
TEST(JsonReader, StrictAboutTheGrammarAndNotAskableOtherwise) {
  ArenaReader owner;
  arnm_result result = ARNM_SUCCESS;

  const char *const refused[] = {"[1,2,3,]",   "[1] // tail",     "[NaN]",
                                 "[Infinity]", "\xEF\xBB\xBF[1]", "{} trailing"};
  for (const char *json : refused) {
    EXPECT_EQ(Parse(owner, json, &result), nullptr) << json;
    EXPECT_EQ(result, ARNM_ERROR_DECODE_FAILED) << json;
  }
}

// promise: stop_when_done ends the document at its last byte, and bytes_read says where that was
TEST(JsonReader, StopWhenDoneEndsTheDocumentAtItsLastByte) {
  ArenaReader owner;
  const std::string json = "{\"a\":1} and then something else entirely";

  arnm_json_value *root = nullptr;
  EXPECT_EQ(
      arnm_json_reader_parse(
          owner.reader(), json.c_str(), static_cast<uint32_t>(json.size()), false, &root
      ),
      ARNM_ERROR_DECODE_FAILED
  ) << "without it, what follows the document is part of the input and is not JSON";

  ASSERT_EQ(
      arnm_json_reader_parse(
          owner.reader(), json.c_str(), static_cast<uint32_t>(json.size()), true, &root
      ),
      ARNM_SUCCESS
  );
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(arnm_json_reader_bytes_read(owner.reader()), 7u)
      << "where the first document stopped, and therefore where the next one starts";
}

// ---------------------------------------------------------------------------
// parsing in place
// ---------------------------------------------------------------------------

// promise: the in place parse reads the caller's buffer and spends it, and the strings it hands
// back point into that same buffer
TEST(JsonReader, AnInsituParseReadsTheCallersBufferAndSpendsIt) {
  ArenaReader owner;
  const std::string json = "{\"text\":\"here\",\"n\":7}";
  std::vector<char> buffer = InsituBuffer(json);
  const std::string before(buffer.data(), json.size());

  arnm_json_value *root = nullptr;
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(buffer.size()), false, &root
      ),
      ARNM_SUCCESS
  );
  ASSERT_NE(root, nullptr);

  arnm_memory_block text{};
  int64_t number = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_STRING("text", &text), ARNM_JSON_FIELD_INT64("n", &number)
  };
  ASSERT_EQ(arnm_json_read_object(root, fields, 2, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(Borrowed(text), "here");
  EXPECT_EQ(number, 7);

  const char *at = reinterpret_cast<const char *>(text.data);
  EXPECT_GE(at, buffer.data()) << "the string is the caller's own bytes, not a copy";
  EXPECT_LT(at, buffer.data() + buffer.size());
  EXPECT_NE(std::string(buffer.data(), json.size()), before)
      << "what the buffer held is gone by the time the parse returns";
}

TEST(JsonReader, AnInsituParseNeedsRoomForItsPadding) {
  ArenaReader owner;
  const std::string json = "{\"a\":1}";
  std::vector<char> buffer = InsituBuffer(json);
  arnm_json_value *root = nullptr;

  EXPECT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(json.size()), false, &root
      ),
      ARNM_ERROR_INVALID_PARAM
  ) << "exactly the JSON leaves nowhere for the padding to go";

  EXPECT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(json.size()) + ARNM_JSON_READER_INSITU_PADDING - 1u, false, &root
      ),
      ARNM_ERROR_INVALID_PARAM
  ) << "one byte short is still short";

  EXPECT_EQ(
      arnm_json_reader_parse_insitu(owner.reader(), nullptr, 4, 64, false, &root),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      arnm_json_reader_parse_insitu(owner.reader(), buffer.data(), 0, 64, false, &root),
      ARNM_ERROR_INVALID_PARAM
  );
}

// promise: an in place parse has no string pool, so an arena gets every byte back on release --
// which is the reason to reach for it, and the clock does not show it
TEST(JsonReader, AnInsituParseGivesTheArenaEverythingBack) {
  ArenaReader owner;
  const std::string json = "{\"text\":\"a string long enough to be worth pooling\",\"n\":7}";

  const uint32_t before = owner.used();

  std::vector<char> buffer = InsituBuffer(json);
  arnm_json_value *root = nullptr;
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(buffer.size()), false, &root
      ),
      ARNM_SUCCESS
  );
  EXPECT_GT(owner.used(), before) << "the document itself is still an allocation";
  ASSERT_EQ(arnm_json_reader_release(owner.reader()), ARNM_SUCCESS);
  EXPECT_EQ(owner.used(), before) << "and it sat at the tail, so the index came all the way home";

  // the copying parse takes the pool first and the values above it, so releasing leaves the pool
  ASSERT_NE(Parse(owner, json), nullptr);
  EXPECT_EQ(arnm_json_reader_release(owner.reader()), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED)
      << "and the arena says so rather than reporting a clean release";
  EXPECT_GT(owner.used(), before) << "the string pool stays buried until the arena resets";
}

// ---------------------------------------------------------------------------
// reading an object in one walk
// ---------------------------------------------------------------------------

namespace {

/** Every target one walk can fill, in the shape a consumer would hold them. */
struct Shape {
  bool flag = false;
  uint64_t id = 0;
  int64_t balance = 0;
  uint32_t port = 0;
  int32_t offset = 0;
  double ratio = 0.0;
  arnm_memory_block name{};
  uint8_t digest_bytes[4] = {0, 0, 0, 0};
  arnm_memory_block digest = ARNM_JSON_BLOCK_OF(digest_bytes);
  uint8_t uuid_bytes[ARNM_UUID_BINARY_SIZE] = {0};
  arnm_memory_block uuid = ARNM_JSON_BLOCK_OF(uuid_bytes);
  arnm_json_value *nested = nullptr;
};

const char kShapeDocument[] =
    "{\"flag\":true,\"id\":18446744073709551615,\"balance\":-9,\"port\":8443,\"offset\":-12345,"
    "\"ratio\":0.25,\"name\":\"here\",\"digest\":\"deadbeef\","
    "\"uuid\":\"019e2c31-a303-75c0-941e-f35c59e4f978\",\"nested\":{\"deep\":1}}";

} // namespace

TEST(JsonReader, AWalkFillsEveryTargetItsTableNames) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, kShapeDocument);
  ASSERT_NE(root, nullptr);

  Shape shape;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_BOOL("flag", &shape.flag),
      ARNM_JSON_FIELD_UINT64("id", &shape.id),
      ARNM_JSON_FIELD_INT64("balance", &shape.balance),
      ARNM_JSON_FIELD_UINT32("port", &shape.port),
      ARNM_JSON_FIELD_INT32("offset", &shape.offset),
      ARNM_JSON_FIELD_DOUBLE("ratio", &shape.ratio),
      ARNM_JSON_FIELD_STRING("name", &shape.name),
      ARNM_JSON_FIELD_HEX_FIXED("digest", &shape.digest),
      ARNM_JSON_FIELD_UUID("uuid", &shape.uuid),
      ARNM_JSON_FIELD_VALUE("nested", &shape.nested)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 10, &found), ARNM_SUCCESS);

  EXPECT_EQ(found, 0x3ffull) << "every one of the ten was there";
  EXPECT_TRUE(shape.flag);
  EXPECT_EQ(shape.id, UINT64_MAX);
  EXPECT_EQ(shape.balance, -9);
  EXPECT_EQ(shape.port, 8443u);
  EXPECT_EQ(shape.offset, -12345);
  EXPECT_DOUBLE_EQ(shape.ratio, 0.25);
  EXPECT_EQ(Borrowed(shape.name), "here");
  EXPECT_EQ(shape.digest_bytes[0], 0xdeu);
  EXPECT_EQ(shape.digest_bytes[3], 0xefu);
  EXPECT_EQ(shape.uuid_bytes[0], 0x01u);
  EXPECT_EQ(shape.uuid_bytes[15], 0x78u);
  ASSERT_NE(shape.nested, nullptr) << "a nested member is handed over, not walked";
}

// promise: the nested value really is a way further down -- the model's whole traversal is a
// table naming a VALUE and then another table on it
TEST(JsonReader, AValueFieldIsTheWayIntoTheNextShape) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"inner\":{\"n\":42,\"s\":\"deep\"}}");
  ASSERT_NE(root, nullptr);

  arnm_json_value *inner = nullptr;
  arnm_json_field outer[] = {ARNM_JSON_FIELD_VALUE("inner", &inner)};
  ASSERT_EQ(arnm_json_read_object(root, outer, 1, nullptr), ARNM_SUCCESS);
  ASSERT_NE(inner, nullptr);

  int64_t number = 0;
  arnm_memory_block text{};
  arnm_json_field within[] = {
      ARNM_JSON_FIELD_INT64("n", &number), ARNM_JSON_FIELD_STRING("s", &text)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(inner, within, 2, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3ull);
  EXPECT_EQ(number, 42);
  EXPECT_EQ(Borrowed(text), "deep");
}

TEST(JsonReader, AWalkReadsTheSameDocumentInAnyOrder) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"third\":3,\"first\":1,\"second\":2}");
  ASSERT_NE(root, nullptr);

  int64_t first = 0, second = 0, third = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("first", &first), ARNM_JSON_FIELD_INT64("second", &second),
      ARNM_JSON_FIELD_INT64("third", &third)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 3, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x7ull);
  EXPECT_EQ(first, 1);
  EXPECT_EQ(second, 2);
  EXPECT_EQ(third, 3);
}

// promise: an entry is skipped once filled, so the first of a pair is what is kept -- at either
// position relative to where the scan currently stands
TEST(JsonReader, AWalkKeepsTheFirstOfADuplicateWhereverItSits) {
  ArenaReader owner;
  {
    arnm_json_value *root = Parse(owner, "{\"a\":1,\"a\":2,\"b\":3}");
    ASSERT_NE(root, nullptr);
    int64_t a = 0, b = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b)};
    ASSERT_EQ(arnm_json_read_object(root, fields, 2, nullptr), ARNM_SUCCESS);
    EXPECT_EQ(a, 1) << "the duplicate below the lowest open entry is passed over";
    EXPECT_EQ(b, 3);
  }
  {
    arnm_json_value *root = Parse(owner, "{\"b\":1,\"b\":2}");
    ASSERT_NE(root, nullptr);
    int64_t a = -1, b = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b)};
    uint64_t found = 0;
    ASSERT_EQ(arnm_json_read_object(root, fields, 2, &found), ARNM_SUCCESS);
    EXPECT_EQ(found, 0x2ull);
    EXPECT_EQ(b, 1) << "the one the scan still passes over is skipped by its filled bit";
    EXPECT_EQ(a, -1);
  }
}

// promise: once every entry is filled the rest of the document is not looked at
TEST(JsonReader, AWalkStopsLookingOnceEveryEntryIsFilled) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"a\":1,\"a\":\"not a number\"}");
  ASSERT_NE(root, nullptr);

  int64_t a = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  EXPECT_EQ(arnm_json_read_object(root, fields, 1, nullptr), ARNM_SUCCESS)
      << "the second a was never read, so its type was never asked";
  EXPECT_EQ(a, 1);
}

TEST(JsonReader, AWalkSkipsWhatItDoesNotKnowAndNamesWhatWasThere) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"other\":1,\"b\":2,\"more\":3}");
  ASSERT_NE(root, nullptr);

  int64_t a = -1, b = -1, c = -1;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b), ARNM_JSON_FIELD_INT64("c", &c)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 3, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x2ull) << "only the middle one was carried";
  EXPECT_EQ(a, -1) << "an absent member leaves its target alone";
  EXPECT_EQ(b, 2);
  EXPECT_EQ(c, -1);
}

TEST(JsonReader, AWalkStopsAtTheMemberItCannotReadAndSaysHowFarItCame) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"a\":1,\"b\":\"two\",\"c\":3}");
  ASSERT_NE(root, nullptr);

  int64_t a = 0, b = 0, c = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b), ARNM_JSON_FIELD_INT64("c", &c)
  };
  uint64_t found = 0;
  EXPECT_EQ(arnm_json_read_object(root, fields, 3, &found), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(found, 0x1ull) << "the first was read, the second refused, the third never reached";
  EXPECT_EQ(a, 1);
  EXPECT_EQ(c, 0);
}

// promise: both ends of both narrow integer types, which is where a range check is easiest to
// get backwards
TEST(JsonReader, AWalkCarriesEveryNumberItsTargetCanHold) {
  ArenaReader owner;
  arnm_json_value *root = Parse(
      owner, "{\"low\":-2147483648,\"high\":2147483647,\"small\":-1,\"wide\":-9223372036854775808,"
             "\"huge\":18446744073709551615,\"cap\":4294967295}"
  );
  ASSERT_NE(root, nullptr);

  int32_t low = 0, high = 0, small = 0;
  int64_t wide = 0;
  uint64_t huge = 0;
  uint32_t cap = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT32("low", &low),     ARNM_JSON_FIELD_INT32("high", &high),
      ARNM_JSON_FIELD_INT32("small", &small), ARNM_JSON_FIELD_INT64("wide", &wide),
      ARNM_JSON_FIELD_UINT64("huge", &huge),  ARNM_JSON_FIELD_UINT32("cap", &cap)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 6, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3full);
  EXPECT_EQ(low, INT32_MIN);
  EXPECT_EQ(high, INT32_MAX);
  EXPECT_EQ(small, -1);
  EXPECT_EQ(wide, INT64_MIN);
  EXPECT_EQ(huge, UINT64_MAX);
  EXPECT_EQ(cap, UINT32_MAX);
}

TEST(JsonReader, AWalkRefusesANumberThatWillNotFitItsTarget) {
  ArenaReader owner;

  arnm_json_value *root = Parse(owner, "{\"n\":2147483648}");
  ASSERT_NE(root, nullptr);
  int32_t narrow = 0;
  arnm_json_field over_i32[] = {ARNM_JSON_FIELD_INT32("n", &narrow)};
  EXPECT_EQ(arnm_json_read_object(root, over_i32, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(narrow, 0) << "a refused member leaves its target alone";

  root = Parse(owner, "{\"n\":4294967296}");
  ASSERT_NE(root, nullptr);
  uint32_t wide_u32 = 0;
  arnm_json_field over_u32[] = {ARNM_JSON_FIELD_UINT32("n", &wide_u32)};
  EXPECT_EQ(arnm_json_read_object(root, over_u32, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  root = Parse(owner, "{\"n\":-1}");
  ASSERT_NE(root, nullptr);
  uint32_t unsigned_narrow = 0;
  uint64_t unsigned_wide = 0xdeadbeefull;
  arnm_json_field negative_u32[] = {ARNM_JSON_FIELD_UINT32("n", &unsigned_narrow)};
  arnm_json_field negative_u64[] = {ARNM_JSON_FIELD_UINT64("n", &unsigned_wide)};
  EXPECT_EQ(arnm_json_read_object(root, negative_u32, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(arnm_json_read_object(root, negative_u64, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(unsigned_wide, 0xdeadbeefull);

  // 2^63 is one past what an int64_t holds, and carrying it across would wrap it negative
  root = Parse(owner, "{\"n\":9223372036854775808}");
  ASSERT_NE(root, nullptr);
  int64_t past_int64 = 0;
  arnm_json_field over_i64[] = {ARNM_JSON_FIELD_INT64("n", &past_int64)};
  EXPECT_EQ(arnm_json_read_object(root, over_i64, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(past_int64, 0);
}

TEST(JsonReader, AWalkRefusesAStringThatIsNotTheShapeItsEntryNames) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"short\":\"01\",\"nothex\":\"zzzzzzzz\",\"empty\":\"\"}");
  ASSERT_NE(root, nullptr);
  uint8_t four[4] = {0, 0, 0, 0};

  arnm_memory_block too_short = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field short_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("short", &too_short)};
  EXPECT_EQ(arnm_json_read_object(root, short_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED)
      << "four bytes want eight characters";

  arnm_memory_block bad = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field not_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("nothex", &bad)};
  EXPECT_EQ(arnm_json_read_object(root, not_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  arnm_memory_block empty_target = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field empty_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("empty", &empty_target)};
  EXPECT_EQ(arnm_json_read_object(root, empty_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  uint8_t uuid[ARNM_UUID_BINARY_SIZE] = {0};
  arnm_memory_block uuid_block = ARNM_JSON_BLOCK_OF(uuid);
  arnm_json_field not_a_uuid[] = {ARNM_JSON_FIELD_UUID("short", &uuid_block)};
  EXPECT_EQ(arnm_json_read_object(root, not_a_uuid, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  // a uuid decodes into exactly ARNM_UUID_BINARY_SIZE bytes, so a block of any other size is the
  // caller's mistake rather than the document's
  root = Parse(owner, "{\"u\":\"019e2c31-a303-75c0-941e-f35c59e4f978\"}");
  ASSERT_NE(root, nullptr);
  uint8_t too_few[8] = {0};
  arnm_memory_block wrong_size = ARNM_JSON_BLOCK_OF(too_few);
  arnm_json_field bad_target[] = {ARNM_JSON_FIELD_UUID("u", &wrong_size)};
  EXPECT_EQ(arnm_json_read_object(root, bad_target, 1, nullptr), ARNM_ERROR_DECODE_FAILED);
}

TEST(JsonReader, AWalkRefusesAMemberOfAnotherJsonType) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"n\":true,\"s\":5,\"b\":\"yes\",\"d\":null}");
  ASSERT_NE(root, nullptr);

  int64_t number = 0;
  arnm_json_field wrong_number[] = {ARNM_JSON_FIELD_INT64("n", &number)};
  EXPECT_EQ(arnm_json_read_object(root, wrong_number, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);

  arnm_memory_block text{};
  arnm_json_field wrong_string[] = {ARNM_JSON_FIELD_STRING("s", &text)};
  EXPECT_EQ(arnm_json_read_object(root, wrong_string, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);

  bool flag = false;
  arnm_json_field wrong_bool[] = {ARNM_JSON_FIELD_BOOL("b", &flag)};
  EXPECT_EQ(arnm_json_read_object(root, wrong_bool, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);

  double real = 0.0;
  arnm_json_field wrong_double[] = {ARNM_JSON_FIELD_DOUBLE("d", &real)};
  EXPECT_EQ(arnm_json_read_object(root, wrong_double, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);
}

// promise: there is no placeholder tag -- an entry nobody set is a mistake and is named as one
TEST(JsonReader, AWalkRefusesAnEntryThatCarriesNoTypeOrNoTarget) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"a\":1}");
  ASSERT_NE(root, nullptr);
  int64_t a = 0;

  arnm_json_field no_type[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  no_type[0].type = ARNM_JSON_FIELD_TYPE_NONE;
  uint64_t found = 0;
  EXPECT_EQ(arnm_json_read_object(root, no_type, 1, &found), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(found, 0ull);
  EXPECT_EQ(a, 0);

  arnm_json_field no_target[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  no_target[0].target = nullptr;
  EXPECT_EQ(arnm_json_read_object(root, no_target, 1, nullptr), ARNM_ERROR_NULL_POINTER);
}

TEST(JsonReader, AWalkRefusesWhatIsNoObjectAndWhatIsNoTable) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "[1,2]");
  ASSERT_NE(root, nullptr);

  int64_t value = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &value)};
  uint64_t found = 0xffull;

  EXPECT_EQ(arnm_json_read_object(root, fields, 1, &found), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(found, 0ull) << "the mask is cleared before anything else is asked";
  EXPECT_EQ(arnm_json_read_object(nullptr, fields, 1, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_object(root, nullptr, 1, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_object(root, fields, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(
      arnm_json_read_object(root, fields, ARNM_JSON_FIELDS_MAX + 1u, nullptr),
      ARNM_ERROR_INVALID_PARAM
  );
}

// promise: the mask is a bit per entry, so the topmost entry of a full table is what says whether
// the shift that sets it still fits the word
TEST(JsonReader, AWalkFillsTheWidestTableItAccepts) {
  ArenaReader owner;
  std::string json = "{";
  for (uint32_t index = 0; index < ARNM_JSON_FIELDS_MAX; ++index) {
    if (index) { json += ","; }
    json += "\"k" + std::to_string(index) + "\":" + std::to_string(index);
  }
  json += "}";
  arnm_json_value *root = Parse(owner, json);
  ASSERT_NE(root, nullptr);

  std::vector<std::string> keys;
  std::vector<int64_t> values(ARNM_JSON_FIELDS_MAX, -1);
  std::vector<arnm_json_field> fields;
  keys.reserve(ARNM_JSON_FIELDS_MAX);
  for (uint32_t index = 0; index < ARNM_JSON_FIELDS_MAX; ++index) {
    keys.push_back("k" + std::to_string(index));
    arnm_json_field field{};
    field.key = keys.back().c_str();
    field.key_length = static_cast<uint32_t>(keys.back().size());
    field.type = ARNM_JSON_FIELD_TYPE_INT64;
    field.target = &values[index];
    fields.push_back(field);
  }

  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields.data(), ARNM_JSON_FIELDS_MAX, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, UINT64_MAX) << "every bit of the mask, the topmost one included";
  for (uint32_t index = 0; index < ARNM_JSON_FIELDS_MAX; ++index) {
    EXPECT_EQ(values[index], static_cast<int64_t>(index)) << "entry " << index;
  }
}

// promise: a key is compared over its length, so a prefix of a longer key is not a match
TEST(JsonReader, AWalkComparesKeysOverTheirLengthAndNotToATerminator) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"abc\":1,\"ab\":2}");
  ASSERT_NE(root, nullptr);

  int64_t two = 0, three = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("ab", &two), ARNM_JSON_FIELD_INT64("abc", &three)
  };
  uint64_t found = 0;
  ASSERT_EQ(arnm_json_read_object(root, fields, 2, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3ull);
  EXPECT_EQ(two, 2);
  EXPECT_EQ(three, 1);
}

// ---------------------------------------------------------------------------
// reading an array
// ---------------------------------------------------------------------------

TEST(JsonReader, AnArrayComesBackInOrder) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "[{\"n\":1},{\"n\":2},{\"n\":3}]");
  ASSERT_NE(root, nullptr);

  arnm_json_value *elements[4] = {nullptr, nullptr, nullptr, nullptr};
  uint32_t size = 0;
  ASSERT_EQ(arnm_json_read_array(root, elements, 4, &size), ARNM_SUCCESS);
  ASSERT_EQ(size, 3u);

  for (uint32_t index = 0; index < size; ++index) {
    int64_t number = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("n", &number)};
    ASSERT_EQ(arnm_json_read_object(elements[index], fields, 1, nullptr), ARNM_SUCCESS)
        << "element " << index;
    EXPECT_EQ(number, static_cast<int64_t>(index) + 1);
  }
  EXPECT_EQ(elements[3], nullptr) << "nothing beyond the array's own length is touched";
}

// promise: an array that exactly fills the buffer fits -- the bound is the length, not one less
TEST(JsonReader, AnArrayThatExactlyFillsTheBufferIsAccepted) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "[1,2,3,4]");
  ASSERT_NE(root, nullptr);

  arnm_json_value *exact[4] = {nullptr, nullptr, nullptr, nullptr};
  uint32_t size = 0;
  EXPECT_EQ(arnm_json_read_array(root, exact, 4, &size), ARNM_SUCCESS);
  EXPECT_EQ(size, 4u);
  for (arnm_json_value *element : exact) { EXPECT_NE(element, nullptr); }
}

// promise: all or nothing -- an array longer than the buffer is refused, not truncated
TEST(JsonReader, AnArrayLongerThanTheBufferIsRefusedWhole) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "[1,2,3,4,5]");
  ASSERT_NE(root, nullptr);

  arnm_json_value *slots[4] = {nullptr, nullptr, nullptr, nullptr};
  uint32_t size = 0xffu;
  EXPECT_EQ(arnm_json_read_array(root, slots, 4, &size), ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL);
  EXPECT_EQ(size, 0u) << "the size is cleared before anything else is asked";
  for (arnm_json_value *element : slots) {
    EXPECT_EQ(element, nullptr) << "nothing is written on the path that refuses";
  }
}

TEST(JsonReader, AnEmptyArrayIsNotARefusal) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "[]");
  ASSERT_NE(root, nullptr);

  arnm_json_value *slots[2] = {nullptr, nullptr};
  uint32_t size = 0xffu;
  EXPECT_EQ(arnm_json_read_array(root, slots, 2, &size), ARNM_SUCCESS);
  EXPECT_EQ(size, 0u);
  EXPECT_EQ(slots[0], nullptr);
}

TEST(JsonReader, AnArrayReadRefusesWhatIsNoArray) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"a\":1}");
  ASSERT_NE(root, nullptr);

  arnm_json_value *slots[2] = {nullptr, nullptr};
  EXPECT_EQ(arnm_json_read_array(root, slots, 2, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(arnm_json_read_array(nullptr, slots, 2, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_array(root, nullptr, 2, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_array(root, slots, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
}

// promise: an array of arrays is read the same way, one level at a time -- which is the whole
// traversal model, and the reason nothing here nests on the caller's behalf
TEST(JsonReader, AnArrayOfArraysIsReadOneLevelAtATime) {
  ArenaReader owner;
  arnm_json_value *root = Parse(owner, "{\"rows\":[[1,2],[3],[4,5,6]]}");
  ASSERT_NE(root, nullptr);

  arnm_json_value *rows = nullptr;
  arnm_json_field outer[] = {ARNM_JSON_FIELD_VALUE("rows", &rows)};
  ASSERT_EQ(arnm_json_read_object(root, outer, 1, nullptr), ARNM_SUCCESS);
  ASSERT_NE(rows, nullptr);

  arnm_json_value *row[4] = {nullptr, nullptr, nullptr, nullptr};
  uint32_t row_count = 0;
  ASSERT_EQ(arnm_json_read_array(rows, row, 4, &row_count), ARNM_SUCCESS);
  ASSERT_EQ(row_count, 3u);

  const uint32_t expected_widths[] = {2u, 1u, 3u};
  for (uint32_t index = 0; index < row_count; ++index) {
    arnm_json_value *cell[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t width = 0;
    ASSERT_EQ(arnm_json_read_array(row[index], cell, 4, &width), ARNM_SUCCESS) << index;
    EXPECT_EQ(width, expected_widths[index]) << "row " << index;
  }
}
