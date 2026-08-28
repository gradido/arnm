#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// The reader is opaque from here on purpose: these tests only ever see what a consumer sees.
// What they are actually checking is the seam underneath -- that yyjson's allocator hooks reach
// arnm and nothing else, that a document costs the number of allocations the header promises,
// and that every arena byte comes home when the shape allows it.

namespace {

/** Arena large enough for every document below, including the one that forces a regrow. */
constexpr uint32_t kArenaCapacity = 1024 * 1024;

/** Parse a literal through a reader, the copying way. */
arnm_result Parse(arnm_json_reader *reader, const std::string &json) {
  return arnm_json_reader_parse(reader, json.c_str(), static_cast<uint32_t>(json.size()));
}

/** Parse a literal in place, through a buffer that lives as long as the caller wants it. */
arnm_result ParseInsitu(arnm_json_reader *reader, std::vector<char> &buffer, size_t length) {
  return arnm_json_reader_parse_insitu(
      reader, buffer.data(), static_cast<uint32_t>(length), static_cast<uint32_t>(buffer.size())
  );
}

/** A writable buffer carrying `json` plus the padding the insitu path writes through. */
std::vector<char> InsituBuffer(const std::string &json) {
  std::vector<char> buffer(json.size() + ARNM_JSON_READER_INSITU_PADDING, '\0');
  std::memcpy(buffer.data(), json.data(), json.size());
  return buffer;
}

/**
 * Where an arena's index stands, as an address.
 *
 * Measured through the public interface alone: what a one byte allocation is handed is the
 * index itself, and giving it straight back leaves the arena as it was found. Only differences
 * between two of these readings mean anything -- ArenaBase() below turns them into offsets.
 */
uintptr_t ArenaMark(arnm *arena) {
  uint8_t *probe = nullptr;
  EXPECT_EQ(arnm_alloc(&probe, 1, arena), ARNM_SUCCESS);
  const uintptr_t mark = reinterpret_cast<uintptr_t>(probe);
  EXPECT_EQ(arnm_free(probe, 1, arena), ARNM_SUCCESS);
  return mark;
}

/** A reader over an arena, torn down in the right order at the end of a scope. */
class ArenaReader {
public:
  explicit ArenaReader(arnm_json_read_flags flags = ARNM_JSON_READ_DEFAULT) {
    EXPECT_EQ(arnm_init_arena(&arena_, kArenaCapacity), ARNM_SUCCESS);
    base_ = ArenaMark(&arena_);
    EXPECT_EQ(arnm_json_reader_init(&reader_, &arena_, flags), ARNM_SUCCESS);
  }
  ~ArenaReader() {
    arnm_json_reader_release(&reader_);
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
  /** Bytes the arena has handed out and not taken back. */
  uint32_t used() {
    return static_cast<uint32_t>(ArenaMark(&arena_) - base_);
  }

private:
  arnm arena_{};
  arnm_json_reader reader_{};
  uintptr_t base_ = 0;
};

const char kDocument[] =
    "{\"name\":\"arnm\",\"count\":42,\"ratio\":0.5,\"cold\":-7,\"ok\":true,\"nothing\":null,"
    "\"list\":[1,\"two\",false,null,[3],{\"deep\":true}]}";

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(JsonReader, AZeroedReaderIsNotInitialized) {
  // Failures leave every output untouched, so uninitialized storage has to be recognisable
  // rather than walked into.
  arnm_json_reader reader{};
  std::memset(&reader, 0, sizeof(reader));

  EXPECT_EQ(Parse(&reader, "{}"), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_root(&reader), nullptr);
  EXPECT_FALSE(arnm_json_reader_has_document(&reader));
  EXPECT_STREQ(arnm_json_reader_error_message(&reader), "no error");
}

TEST(JsonReader, InitWritesEveryFieldAndAllocatesNothing) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  // deliberately dirty storage: init reads none of it
  arnm_json_reader reader;
  std::memset(&reader, 0xAB, sizeof(reader));
  ASSERT_EQ(arnm_json_reader_init(&reader, &arena, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);

  EXPECT_FALSE(arnm_json_reader_has_document(&reader));
  EXPECT_EQ(arnm_json_reader_root(&reader), nullptr);
  EXPECT_EQ(arnm_json_reader_value_count(&reader), 0u);
  EXPECT_EQ(ArenaMark(&arena), before) << "init must not draw from the allocator";

  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonReader, InitRefusesANullReader) {
  EXPECT_EQ(
      arnm_json_reader_init(nullptr, nullptr, ARNM_JSON_READ_DEFAULT), ARNM_ERROR_NULL_POINTER
  );
}

TEST(JsonReader, CreateCarvesTheStateOutOfTheAllocator) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  arnm_json_reader *reader = arnm_json_reader_create(&arena, ARNM_JSON_READ_DEFAULT);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(ArenaMark(&arena) - before, ARNM_ALIGN8(sizeof(arnm_json_reader)));

  // insitu, so the whole document is the one block above the state: both are at the tail when
  // their turn comes and the index lands exactly where it started
  std::vector<char> buffer = InsituBuffer(kDocument);
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          reader, buffer.data(), static_cast<uint32_t>(std::strlen(kDocument)),
          static_cast<uint32_t>(buffer.size())
      ),
      ARNM_SUCCESS
  );
  EXPECT_TRUE(arnm_json_reader_has_document(reader));

  EXPECT_EQ(arnm_json_reader_destroy(reader, &arena), ARNM_SUCCESS);
  EXPECT_EQ(ArenaMark(&arena), before);
  arnm_release(&arena);
}

TEST(JsonReader, DestroyReportsWhatAnArenaCouldNotTakeBack) {
  // The copying parse allocates the string pool under the value buffer, so releasing gives the
  // buffer back and leaves the pool -- and the reader's own bytes below it -- buried. The
  // warning is the documented answer, and reading it as failure is the mistake it guards
  // against: everything was released, some of it simply waits for the arena's reset.
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, kArenaCapacity), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&arena);

  arnm_json_reader *reader = arnm_json_reader_create(&arena, ARNM_JSON_READ_DEFAULT);
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(Parse(reader, kDocument), ARNM_SUCCESS);

  EXPECT_EQ(arnm_json_reader_destroy(reader, &arena), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_GT(ArenaMark(&arena), before);

  arnm_reset(&arena);
  EXPECT_EQ(ArenaMark(&arena), before) << "a reset is what returns the rest";
  arnm_release(&arena);
}

TEST(JsonReader, CreateOnAFullArenaAnswersNull) {
  // A cap that cannot hold the state at all, so the refusal is the allocator's and not a
  // question of how much memory the machine happens to have.
  uint8_t storage[16] = {0};
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_create(&arena, ARNM_JSON_READ_DEFAULT), nullptr);
  arnm_release(&arena);
}

TEST(JsonReader, NullIsANoOpEverywhereItIsAccepted) {
  EXPECT_EQ(arnm_json_reader_destroy(nullptr, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_release(nullptr), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_root(nullptr), nullptr);
  EXPECT_FALSE(arnm_json_reader_has_document(nullptr));
  EXPECT_STREQ(arnm_json_reader_error_message(nullptr), "no error");
  EXPECT_EQ(arnm_json_reader_error_position(nullptr), 0u);
  EXPECT_EQ(arnm_json_reader_value_count(nullptr), 0u);
  EXPECT_EQ(arnm_json_reader_bytes_read(nullptr), 0u);
}

TEST(JsonReader, ACreatedReaderGoesHomeThroughDestroy) {
  arnm_json_reader *reader = arnm_json_reader_create(nullptr, ARNM_JSON_READ_DEFAULT);
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(Parse(reader, "[1,2,3]"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_array_size(arnm_json_reader_root(reader)), 3u);
  // the host takes the state's bytes back here; nothing may read through the handle afterwards
  EXPECT_EQ(arnm_json_reader_destroy(reader, nullptr), ARNM_SUCCESS);
}

TEST(JsonReader, ReleaseKeepsTheReaderUsable) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  // the copying parse buries its string pool, so the warning is what release answers here --
  // the document is gone either way, which is what the rest of this test is about
  ASSERT_EQ(arnm_json_reader_release(owner.reader()), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);

  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()));
  EXPECT_EQ(arnm_json_reader_root(owner.reader()), nullptr);
  EXPECT_EQ(arnm_json_reader_value_count(owner.reader()), 0u);

  // still bound to its allocator, so it parses again without being initialized again
  EXPECT_EQ(Parse(owner.reader(), "[1]"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_array_size(arnm_json_reader_root(owner.reader())), 1u);
}

// ---------------------------------------------------------------------------
// the allocator seam
// ---------------------------------------------------------------------------

TEST(JsonReader, EveryByteOfADocumentComesFromTheNamedAllocator) {
  ArenaReader owner;
  const uint32_t before = owner.used();

  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  const uint32_t after_parse = owner.used();
  EXPECT_GT(after_parse, before) << "the parse has to draw from the arena, not from the host";

  // the copying parse allocates the string pool first and the value buffer above it, so the
  // buffer goes home and the pool waits for the arena's own reset -- the warning is the
  // documented answer and not a failure
  EXPECT_EQ(arnm_json_reader_release(owner.reader()), ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_LT(owner.used(), after_parse) << "the value buffer sat at the tail and had to come back";

  arnm_reset(owner.arena());
  EXPECT_EQ(owner.used(), 0u) << "a reset is what returns the rest";
}

TEST(JsonReader, InsituLeavesTheArenaExactlyAsItWasFound) {
  ArenaReader owner;
  const uint32_t before = owner.used();

  std::vector<char> buffer = InsituBuffer(kDocument);
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(std::strlen(kDocument)),
          static_cast<uint32_t>(buffer.size())
      ),
      ARNM_SUCCESS
  );
  EXPECT_GT(owner.used(), before);

  // one allocation and it is the tail, so releasing moves the index all the way home and
  // nothing is left for a reset to collect
  EXPECT_EQ(arnm_json_reader_release(owner.reader()), ARNM_SUCCESS);
  EXPECT_EQ(owner.used(), before);
}

TEST(JsonReader, InsituCostsLessArenaThanCopying) {
  // The saving is the input copy plus its padding; measured rather than asserted from the
  // header, so a change to either path is noticed here.
  const std::string json(kDocument);

  ArenaReader copying;
  const uint32_t copy_before = copying.used();
  ASSERT_EQ(Parse(copying.reader(), json), ARNM_SUCCESS);
  const uint32_t copy_cost = copying.used() - copy_before;

  ArenaReader in_place;
  const uint32_t insitu_before = in_place.used();
  std::vector<char> buffer = InsituBuffer(json);
  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          in_place.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(buffer.size())
      ),
      ARNM_SUCCESS
  );
  const uint32_t insitu_cost = in_place.used() - insitu_before;

  EXPECT_LT(insitu_cost, copy_cost);
  EXPECT_GE(copy_cost - insitu_cost, json.size()) << "the whole input copy has to be the saving";
}

TEST(JsonReader, InsituReadsStringsOutOfTheCallersBuffer) {
  ArenaReader owner;
  const std::string json = "{\"key\":\"value with \\\"escape\\\"\"}";
  std::vector<char> buffer = InsituBuffer(json);

  ASSERT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(buffer.size())
      ),
      ARNM_SUCCESS
  );

  arnm_json_value *value = nullptr;
  ASSERT_EQ(
      arnm_json_object_get(arnm_json_reader_root(owner.reader()), "key", &value), ARNM_SUCCESS
  );
  const char *text = nullptr;
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_read_string(value, &text, &length), ARNM_SUCCESS);
  EXPECT_EQ(std::string(text, length), "value with \"escape\"");

  // the proof that nothing was copied: the string points into the caller's own bytes
  EXPECT_GE(text, buffer.data());
  EXPECT_LT(text, buffer.data() + buffer.size());
}

TEST(JsonReader, InsituRefusesABufferWithoutPadding) {
  ArenaReader owner;
  std::string json = "[1,2,3]";
  std::vector<char> buffer(json.begin(), json.end());
  buffer.resize(json.size() + ARNM_JSON_READER_INSITU_PADDING - 1);

  EXPECT_EQ(
      arnm_json_reader_parse_insitu(
          owner.reader(), buffer.data(), static_cast<uint32_t>(json.size()),
          static_cast<uint32_t>(buffer.size())
      ),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()));
}

TEST(JsonReader, ADocumentTooLargeForTheArenaIsRefusedNotBorrowed) {
  // A tiny arena, so the parse has to fail inside arnm rather than reach the host behind its
  // back. This is the check that the allocator hooks have no fallback path.
  uint8_t storage[256] = {0};
  arnm arena{};
  ASSERT_EQ(arnm_init_arena_borrow(&arena, storage, sizeof(storage)), ARNM_SUCCESS);

  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, &arena, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);

  std::string big = "[";
  for (int i = 0; i < 512; ++i) { big += (i ? ",1" : "1"); }
  big += "]";

  EXPECT_EQ(Parse(&reader, big), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_FALSE(arnm_json_reader_has_document(&reader));
  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_SUCCESS);
  arnm_release(&arena);
}

TEST(JsonReader, AGrowingValueBufferStaysWithTheAllocator) {
  // Enough elements that yyjson's first estimate is too small and it has to resize -- the
  // realloc hook is the one path the smaller documents never take.
  ArenaReader owner;
  std::string many = "[";
  for (int i = 0; i < 4000; ++i) { many += (i ? ",1" : "1"); }
  many += "]";

  ASSERT_EQ(Parse(owner.reader(), many), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_array_size(arnm_json_reader_root(owner.reader())), 4000u);

  uint32_t seen = 0;
  arnm_json_array_iter iter{};
  ASSERT_EQ(arnm_json_array_iter_init(arnm_json_reader_root(owner.reader()), &iter), ARNM_SUCCESS);
  arnm_json_value *element = nullptr;
  while (arnm_json_array_iter_next(&iter, &element)) {
    int32_t value = 0;
    ASSERT_EQ(arnm_json_read_int32(element, &value), ARNM_SUCCESS);
    EXPECT_EQ(value, 1);
    ++seen;
  }
  EXPECT_EQ(seen, 4000u);
}

TEST(JsonReader, TheHostAllocatorServesAReaderJustAsWell) {
  // NULL is the host everywhere else in arnm, and it has to be here too -- including the
  // release path, where the block header is what carries the size back to free().
  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, nullptr, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);
  ASSERT_EQ(Parse(&reader, kDocument), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_object_size(arnm_json_reader_root(&reader)), 7u);
  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_SUCCESS);
}

TEST(JsonReader, EveryHandedOutBlockIsEightByteAligned) {
  // The block header is eight bytes wide precisely so the payload keeps the alignment arnm
  // promises. A value read out of the document is the visible end of that chain.
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  const arnm_json_value *root = arnm_json_reader_root(owner.reader());
  EXPECT_EQ(reinterpret_cast<uintptr_t>(root) % 8, 0u);
}

// ---------------------------------------------------------------------------
// parsing and its refusals
// ---------------------------------------------------------------------------

TEST(JsonReader, ParseRefusesTheArgumentsItCannotUse) {
  ArenaReader owner;
  EXPECT_EQ(arnm_json_reader_parse(nullptr, "{}", 2), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_reader_parse(owner.reader(), nullptr, 2), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_reader_parse(owner.reader(), "{}", 0), ARNM_ERROR_INVALID_PARAM);
}

TEST(JsonReader, AnUnknownFlagBitIsRefusedRatherThanPassedOn) {
  // The flags are translated one by one, which is what lets a bit we do not define be caught
  // at init instead of arriving in yyjson as a feature nobody asked for. It is caught before a
  // byte of the reader is written, so what was refused stays uninitialized.
  arnm_json_reader reader;
  std::memset(&reader, 0, sizeof(reader));
  EXPECT_EQ(arnm_json_reader_init(&reader, nullptr, 1u << 20), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_create(nullptr, 1u << 20), nullptr);
}

TEST(JsonReader, MalformedInputAnswersDecodeFailedAndSaysWhere) {
  ArenaReader owner;
  EXPECT_EQ(Parse(owner.reader(), "{\"a\":}"), ARNM_ERROR_DECODE_FAILED);
  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()));
  EXPECT_STRNE(arnm_json_reader_error_message(owner.reader()), "no error");
  EXPECT_GT(arnm_json_reader_error_position(owner.reader()), 0u);

  // a good parse clears what the bad one left behind
  ASSERT_EQ(Parse(owner.reader(), "{}"), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_error_message(owner.reader()), "no error");
  EXPECT_EQ(arnm_json_reader_error_position(owner.reader()), 0u);
}

TEST(JsonReader, AFailedParseDropsTheDocumentItReplaced) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  ASSERT_TRUE(arnm_json_reader_has_document(owner.reader()));

  EXPECT_EQ(Parse(owner.reader(), "not json"), ARNM_ERROR_DECODE_FAILED);
  EXPECT_FALSE(arnm_json_reader_has_document(owner.reader()))
      << "answering the previous document after a failed parse is the state nobody checks for";
  EXPECT_EQ(arnm_json_reader_root(owner.reader()), nullptr);
}

TEST(JsonReader, StrictAboutTheGrammarAndNotAskableOtherwise) {
  // yyjson is built here with YYJSON_DISABLE_NON_STANDARD, so the extensions are gone from the
  // parser rather than defaulted off and there is no flag left that could ask for them back.
  // What the reader refuses, it refuses whatever it was initialised with.
  ArenaReader strict;
  EXPECT_EQ(Parse(strict.reader(), "[1,2,3,]"), ARNM_ERROR_DECODE_FAILED) << "trailing comma";
  EXPECT_EQ(Parse(strict.reader(), "[1] // tail"), ARNM_ERROR_DECODE_FAILED) << "comment";
  EXPECT_EQ(Parse(strict.reader(), "[NaN]"), ARNM_ERROR_DECODE_FAILED) << "nan";
  EXPECT_EQ(Parse(strict.reader(), "[Infinity]"), ARNM_ERROR_DECODE_FAILED) << "infinity";
  EXPECT_EQ(Parse(strict.reader(), "\xEF\xBB\xBF[1]"), ARNM_ERROR_DECODE_FAILED)
      << "byte order mark";
  EXPECT_EQ(Parse(strict.reader(), "{} trailing"), ARNM_ERROR_DECODE_FAILED);

  ArenaReader stops(ARNM_JSON_READ_STOP_WHEN_DONE);
  EXPECT_EQ(Parse(stops.reader(), "[1,2,3,]"), ARNM_ERROR_DECODE_FAILED)
      << "the one surviving flag is about where a document ends, not about its grammar";
}

// promise: the one flag left still does what it says, and it belongs to the reader rather than
// to a single parse
TEST(JsonReader, StopWhenDoneEndsTheDocumentAtItsLastByte) {
  ArenaReader strict;
  EXPECT_EQ(Parse(strict.reader(), "{} trailing"), ARNM_ERROR_DECODE_FAILED);

  ArenaReader stops(ARNM_JSON_READ_STOP_WHEN_DONE);
  EXPECT_EQ(Parse(stops.reader(), "{} trailing"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_bytes_read(stops.reader()), 2u);
  EXPECT_EQ(Parse(stops.reader(), "[1] and more"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_bytes_read(stops.reader()), 3u);
}

// promise: a bit the header does not define is refused at init rather than carried into a parse
// that would ignore it -- which is the whole reason the removed flags are not still sitting here
TEST(JsonReader, InitRefusesAFlagThisReaderDoesNotHave) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 4096), ARNM_SUCCESS);
  arnm_json_reader reader{};
  for (unsigned bit = 1; bit < 8u; ++bit) {
    EXPECT_EQ(
        arnm_json_reader_init(&reader, &arena, static_cast<arnm_json_read_flags>(1u << bit)),
        ARNM_ERROR_INVALID_PARAM
    ) << "bit "
      << bit << " was one of the flags this build cannot honour";
  }
  arnm_release(&arena);
}

TEST(JsonReader, ValueCountAndBytesReadDescribeTheDocument) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[1,2,3]"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_bytes_read(owner.reader()), 7u);
  EXPECT_EQ(arnm_json_reader_value_count(owner.reader()), 4u) << "the array plus its elements";
}

// ---------------------------------------------------------------------------
// read functions, one per JSON standard type
// ---------------------------------------------------------------------------

namespace {

/** The value at `key` of the shared document, which every read test starts from. */
arnm_json_value *Field(arnm_json_reader *reader, const char *key) {
  arnm_json_value *value = nullptr;
  EXPECT_EQ(arnm_json_object_get(arnm_json_reader_root(reader), key, &value), ARNM_SUCCESS)
      << "key " << key;
  return value;
}

} // namespace

TEST(JsonReader, EveryStandardTypeIsNamedCorrectly) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_reader *r = owner.reader();

  EXPECT_EQ(arnm_json_value_type(arnm_json_reader_root(r)), ARNM_JSON_TYPE_OBJECT);
  EXPECT_EQ(arnm_json_value_type(Field(r, "name")), ARNM_JSON_TYPE_STRING);
  EXPECT_EQ(arnm_json_value_type(Field(r, "count")), ARNM_JSON_TYPE_NUMBER);
  EXPECT_EQ(arnm_json_value_type(Field(r, "ok")), ARNM_JSON_TYPE_BOOL);
  EXPECT_EQ(arnm_json_value_type(Field(r, "nothing")), ARNM_JSON_TYPE_NULL);
  EXPECT_EQ(arnm_json_value_type(Field(r, "list")), ARNM_JSON_TYPE_ARRAY);

  // NONE is the absence of a value, and null is a value that is empty -- not the same answer
  EXPECT_EQ(arnm_json_value_type(nullptr), ARNM_JSON_TYPE_NONE);
}

TEST(JsonReader, ANumberSaysWhichCTypeCarriesItWhole) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_reader *r = owner.reader();

  EXPECT_EQ(arnm_json_value_number_type(Field(r, "count")), ARNM_JSON_NUMBER_TYPE_UINT);
  EXPECT_EQ(arnm_json_value_number_type(Field(r, "cold")), ARNM_JSON_NUMBER_TYPE_SINT);
  EXPECT_EQ(arnm_json_value_number_type(Field(r, "ratio")), ARNM_JSON_NUMBER_TYPE_REAL);
  EXPECT_EQ(arnm_json_value_number_type(Field(r, "name")), ARNM_JSON_NUMBER_TYPE_NONE);
  EXPECT_EQ(arnm_json_value_number_type(nullptr), ARNM_JSON_NUMBER_TYPE_NONE);
}

TEST(JsonReader, ReadsTheValueOfEveryStandardType) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_reader *r = owner.reader();

  // `null` has no read of its own: there was never anything for one to hand back
  EXPECT_EQ(arnm_json_value_type(Field(r, "nothing")), ARNM_JSON_TYPE_NULL);

  bool flag = false;
  EXPECT_EQ(arnm_json_read_bool(Field(r, "ok"), &flag), ARNM_SUCCESS);
  EXPECT_TRUE(flag);

  uint64_t counted = 0;
  EXPECT_EQ(arnm_json_read_uint64(Field(r, "count"), &counted), ARNM_SUCCESS);
  EXPECT_EQ(counted, 42u);

  int64_t cold = 0;
  EXPECT_EQ(arnm_json_read_int64(Field(r, "cold"), &cold), ARNM_SUCCESS);
  EXPECT_EQ(cold, -7);

  double ratio = 0.0;
  EXPECT_EQ(arnm_json_read_double(Field(r, "ratio"), &ratio), ARNM_SUCCESS);
  EXPECT_DOUBLE_EQ(ratio, 0.5);

  const char *name = nullptr;
  uint32_t name_length = 0;
  EXPECT_EQ(arnm_json_read_string(Field(r, "name"), &name, &name_length), ARNM_SUCCESS);
  EXPECT_EQ(std::string(name, name_length), "arnm");
  EXPECT_EQ(name[name_length], '\0') << "a parsed string is terminated as well as measured";
}

TEST(JsonReader, AReadOfTheWrongTypeIsRefusedAndLeavesTheOutputAlone) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_reader *r = owner.reader();

  bool flag = true;
  EXPECT_EQ(arnm_json_read_bool(Field(r, "count"), &flag), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_TRUE(flag) << "a refused read writes nothing";

  int64_t number = 99;
  EXPECT_EQ(arnm_json_read_int64(Field(r, "name"), &number), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(number, 99);

  const char *text = reinterpret_cast<const char *>(0x1);
  EXPECT_EQ(arnm_json_read_string(Field(r, "count"), &text, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(text, reinterpret_cast<const char *>(0x1));

  EXPECT_NE(arnm_json_value_type(Field(r, "ok")), ARNM_JSON_TYPE_NULL);
}

TEST(JsonReader, EveryReadRefusesANullArgument) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_value *value = Field(owner.reader(), "count");

  bool flag = false;
  int64_t wide = 0;
  uint64_t unsigned_wide = 0;
  int32_t narrow = 0;
  uint32_t unsigned_narrow = 0;
  double real = 0.0;
  const char *text = nullptr;

  EXPECT_EQ(arnm_json_read_bool(nullptr, &flag), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_bool(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_int64(nullptr, &wide), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uint64(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uint64(nullptr, &unsigned_wide), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_int32(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_int32(nullptr, &narrow), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uint32(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uint32(nullptr, &unsigned_narrow), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_double(nullptr, &real), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_double(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_string(nullptr, &text, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_string(value, nullptr, nullptr), ARNM_ERROR_NULL_POINTER);
}

TEST(JsonReader, AStringMayHoldAnEmbeddedNul) {
  // strlen is the measurement that lies here, which is why the length comes back separately.
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[\"a\\u0000b\"]"), ARNM_SUCCESS);

  arnm_json_value *element = nullptr;
  ASSERT_EQ(arnm_json_array_get(arnm_json_reader_root(owner.reader()), 0, &element), ARNM_SUCCESS);
  const char *text = nullptr;
  uint32_t length = 0;
  ASSERT_EQ(arnm_json_read_string(element, &text, &length), ARNM_SUCCESS);
  EXPECT_EQ(length, 3u);
  EXPECT_EQ(std::string(text, length), std::string("a\0b", 3));
  EXPECT_EQ(std::strlen(text), 1u);
}

// ---------------------------------------------------------------------------
// numbers at their edges
// ---------------------------------------------------------------------------

TEST(JsonReader, IntegersAtTheirLimitsArriveExactly) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(owner.reader(), "[18446744073709551615,-9223372036854775808,9223372036854775807]"),
      ARNM_SUCCESS
  );
  arnm_json_value *root = arnm_json_reader_root(owner.reader());

  arnm_json_value *element = nullptr;
  uint64_t unsigned_max = 0;
  ASSERT_EQ(arnm_json_array_get(root, 0, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_uint64(element, &unsigned_max), ARNM_SUCCESS);
  EXPECT_EQ(unsigned_max, UINT64_MAX);
  // it does not fit an int64_t, and saying so beats handing back a wrapped one
  int64_t narrowed = 0;
  EXPECT_EQ(arnm_json_read_int64(element, &narrowed), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  int64_t signed_min = 0;
  ASSERT_EQ(arnm_json_array_get(root, 1, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int64(element, &signed_min), ARNM_SUCCESS);
  EXPECT_EQ(signed_min, INT64_MIN);
  uint64_t as_unsigned = 0;
  EXPECT_EQ(arnm_json_read_uint64(element, &as_unsigned), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  int64_t signed_max = 0;
  ASSERT_EQ(arnm_json_array_get(root, 2, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int64(element, &signed_max), ARNM_SUCCESS);
  EXPECT_EQ(signed_max, INT64_MAX);
}

TEST(JsonReader, ANarrowReadRefusesWhatDoesNotFit) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(owner.reader(), "[2147483647,2147483648,-2147483648,-2147483649,4294967296]"),
      ARNM_SUCCESS
  );
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  arnm_json_value *element = nullptr;
  int32_t narrow = 0;
  uint32_t unsigned_narrow = 0;

  ASSERT_EQ(arnm_json_array_get(root, 0, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int32(element, &narrow), ARNM_SUCCESS);
  EXPECT_EQ(narrow, INT32_MAX);

  ASSERT_EQ(arnm_json_array_get(root, 1, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int32(element, &narrow), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(narrow, INT32_MAX) << "the refused read left the previous value standing";

  ASSERT_EQ(arnm_json_array_get(root, 2, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int32(element, &narrow), ARNM_SUCCESS);
  EXPECT_EQ(narrow, INT32_MIN);

  ASSERT_EQ(arnm_json_array_get(root, 3, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int32(element, &narrow), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  ASSERT_EQ(arnm_json_array_get(root, 4, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_uint32(element, &unsigned_narrow), ARNM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(JsonReader, ARealIsReadAsAnIntegerOnlyWhenItIsOne) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[3.0,3.5,-4.0,1e300,-1.0]"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  arnm_json_value *element = nullptr;
  int64_t whole = 0;
  uint64_t unsigned_whole = 0;

  ASSERT_EQ(arnm_json_array_get(root, 0, &element), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_value_number_type(element), ARNM_JSON_NUMBER_TYPE_REAL);
  EXPECT_EQ(arnm_json_read_int64(element, &whole), ARNM_SUCCESS);
  EXPECT_EQ(whole, 3);

  ASSERT_EQ(arnm_json_array_get(root, 1, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int64(element, &whole), ARNM_ERROR_ARITHMETIC_OVERFLOW)
      << "a fraction is not truncated behind the caller's back";

  ASSERT_EQ(arnm_json_array_get(root, 2, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int64(element, &whole), ARNM_SUCCESS);
  EXPECT_EQ(whole, -4);
  EXPECT_EQ(arnm_json_read_uint64(element, &unsigned_whole), ARNM_ERROR_ARITHMETIC_OVERFLOW);

  ASSERT_EQ(arnm_json_array_get(root, 3, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_int64(element, &whole), ARNM_ERROR_ARITHMETIC_OVERFLOW)
      << "far outside int64_t, and the cast would have been undefined";

  ASSERT_EQ(arnm_json_array_get(root, 4, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_uint64(element, &unsigned_whole), ARNM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(JsonReader, EveryNumberConvertsToADouble) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[7,-7,7.25]"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  arnm_json_value *element = nullptr;
  double real = 0.0;

  ASSERT_EQ(arnm_json_array_get(root, 0, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_double(element, &real), ARNM_SUCCESS);
  EXPECT_DOUBLE_EQ(real, 7.0);

  ASSERT_EQ(arnm_json_array_get(root, 1, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_double(element, &real), ARNM_SUCCESS);
  EXPECT_DOUBLE_EQ(real, -7.0);

  ASSERT_EQ(arnm_json_array_get(root, 2, &element), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_read_double(element, &real), ARNM_SUCCESS);
  EXPECT_DOUBLE_EQ(real, 7.25);
}

// ---------------------------------------------------------------------------
// arrays
// ---------------------------------------------------------------------------

TEST(JsonReader, AnArrayIsIndexedAndWalkedToTheSameEnd) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[10,20,30]"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  ASSERT_EQ(arnm_json_array_size(root), 3u);

  std::vector<int32_t> indexed;
  for (uint32_t i = 0; i < arnm_json_array_size(root); ++i) {
    arnm_json_value *element = nullptr;
    ASSERT_EQ(arnm_json_array_get(root, i, &element), ARNM_SUCCESS);
    int32_t value = 0;
    ASSERT_EQ(arnm_json_read_int32(element, &value), ARNM_SUCCESS);
    indexed.push_back(value);
  }

  std::vector<int32_t> walked;
  arnm_json_array_iter iter{};
  ASSERT_EQ(arnm_json_array_iter_init(root, &iter), ARNM_SUCCESS);
  arnm_json_value *element = nullptr;
  while (arnm_json_array_iter_next(&iter, &element)) {
    int32_t value = 0;
    ASSERT_EQ(arnm_json_read_int32(element, &value), ARNM_SUCCESS);
    walked.push_back(value);
  }

  EXPECT_EQ(indexed, walked);
  EXPECT_EQ(walked, (std::vector<int32_t>{10, 20, 30}));
  // a spent iterator stays spent
  EXPECT_FALSE(arnm_json_array_iter_next(&iter, &element));
}

TEST(JsonReader, AnArrayRefusesWhatIsNotThere) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_value *list = Field(owner.reader(), "list");
  arnm_json_value *element = nullptr;

  EXPECT_EQ(arnm_json_array_get(list, 6, &element), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  EXPECT_EQ(
      arnm_json_array_get(Field(owner.reader(), "count"), 0, &element), ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(arnm_json_array_get(nullptr, 0, &element), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_array_get(list, 0, nullptr), ARNM_ERROR_NULL_POINTER);

  arnm_json_array_iter iter{};
  EXPECT_EQ(
      arnm_json_array_iter_init(Field(owner.reader(), "name"), &iter), ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(arnm_json_array_iter_init(list, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_FALSE(arnm_json_array_iter_next(nullptr, &element));

  // a size query answers its empty value rather than refusing
  EXPECT_EQ(arnm_json_array_size(nullptr), 0u);
  EXPECT_EQ(arnm_json_array_size(Field(owner.reader(), "count")), 0u);
}

TEST(JsonReader, AnEmptyArrayEndsAtTheFirstStep) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[]"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  EXPECT_EQ(arnm_json_array_size(root), 0u);

  arnm_json_array_iter iter{};
  ASSERT_EQ(arnm_json_array_iter_init(root, &iter), ARNM_SUCCESS);
  arnm_json_value *element = nullptr;
  EXPECT_FALSE(arnm_json_array_iter_next(&iter, &element));
}

TEST(JsonReader, NestingIsWalkedToTheBottom) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_value *list = Field(owner.reader(), "list");
  ASSERT_EQ(arnm_json_array_size(list), 6u);

  arnm_json_value *inner = nullptr;
  ASSERT_EQ(arnm_json_array_get(list, 4, &inner), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_value_type(inner), ARNM_JSON_TYPE_ARRAY);
  arnm_json_value *three = nullptr;
  ASSERT_EQ(arnm_json_array_get(inner, 0, &three), ARNM_SUCCESS);
  int32_t value = 0;
  EXPECT_EQ(arnm_json_read_int32(three, &value), ARNM_SUCCESS);
  EXPECT_EQ(value, 3);

  arnm_json_value *object = nullptr;
  ASSERT_EQ(arnm_json_array_get(list, 5, &object), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_value_type(object), ARNM_JSON_TYPE_OBJECT);
  arnm_json_value *deep = nullptr;
  ASSERT_EQ(arnm_json_object_get(object, "deep", &deep), ARNM_SUCCESS);
  bool flag = false;
  EXPECT_EQ(arnm_json_read_bool(deep, &flag), ARNM_SUCCESS);
  EXPECT_TRUE(flag);
}

// ---------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------

TEST(JsonReader, AnObjectIsLookedUpAndWalkedInWrittenOrder) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"b\":2,\"c\":3}"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  EXPECT_EQ(arnm_json_object_size(root), 3u);

  std::vector<std::string> keys;
  std::vector<int32_t> values;
  arnm_json_object_iter iter{};
  ASSERT_EQ(arnm_json_object_iter_init(root, &iter), ARNM_SUCCESS);
  const char *key = nullptr;
  uint32_t key_length = 0;
  arnm_json_value *value = nullptr;
  while (arnm_json_object_iter_next(&iter, &key, &key_length, &value)) {
    keys.push_back(std::string(key, key_length));
    int32_t number = 0;
    ASSERT_EQ(arnm_json_read_int32(value, &number), ARNM_SUCCESS);
    values.push_back(number);
  }

  EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(values, (std::vector<int32_t>{1, 2, 3}));
  EXPECT_FALSE(arnm_json_object_iter_next(&iter, &key, &key_length, &value));
}

TEST(JsonReader, AnObjectWalkSkipsWhatItIsNotAsked) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"b\":2}"), ARNM_SUCCESS);
  arnm_json_object_iter iter{};
  ASSERT_EQ(arnm_json_object_iter_init(arnm_json_reader_root(owner.reader()), &iter), ARNM_SUCCESS);

  uint32_t seen = 0;
  while (arnm_json_object_iter_next(&iter, nullptr, nullptr, nullptr)) { ++seen; }
  EXPECT_EQ(seen, 2u);
}

TEST(JsonReader, AnObjectRefusesWhatIsNotThere) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kDocument), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  arnm_json_value *value = nullptr;

  EXPECT_EQ(arnm_json_object_get(root, "absent", &value), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(
      arnm_json_object_get(Field(owner.reader(), "list"), "a", &value), ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(arnm_json_object_get(nullptr, "a", &value), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_object_get(root, nullptr, &value), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_object_get(root, "name", nullptr), ARNM_ERROR_NULL_POINTER);

  arnm_json_object_iter iter{};
  EXPECT_EQ(
      arnm_json_object_iter_init(Field(owner.reader(), "list"), &iter), ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(arnm_json_object_iter_init(root, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_FALSE(arnm_json_object_iter_next(nullptr, nullptr, nullptr, nullptr));

  EXPECT_EQ(arnm_json_object_size(nullptr), 0u);
  EXPECT_EQ(arnm_json_object_size(Field(owner.reader(), "list")), 0u);
}

TEST(JsonReader, AnEmptyObjectEndsAtTheFirstStep) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{}"), ARNM_SUCCESS);
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
  EXPECT_EQ(arnm_json_object_size(root), 0u);

  arnm_json_object_iter iter{};
  ASSERT_EQ(arnm_json_object_iter_init(root, &iter), ARNM_SUCCESS);
  EXPECT_FALSE(arnm_json_object_iter_next(&iter, nullptr, nullptr, nullptr));
}

// ---------------------------------------------------------------------------
// roots that are not containers
// ---------------------------------------------------------------------------

TEST(JsonReader, ABareValueIsADocumentToo) {
  ArenaReader owner;

  ASSERT_EQ(Parse(owner.reader(), "42"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_value_type(arnm_json_reader_root(owner.reader())), ARNM_JSON_TYPE_NUMBER);

  ASSERT_EQ(Parse(owner.reader(), "\"text\""), ARNM_SUCCESS);
  const char *text = nullptr;
  uint32_t length = 0;
  ASSERT_EQ(
      arnm_json_read_string(arnm_json_reader_root(owner.reader()), &text, &length), ARNM_SUCCESS
  );
  EXPECT_EQ(std::string(text, length), "text");

  ASSERT_EQ(Parse(owner.reader(), "null"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_value_type(arnm_json_reader_root(owner.reader())), ARNM_JSON_TYPE_NULL);

  ASSERT_EQ(Parse(owner.reader(), "true"), ARNM_SUCCESS);
  bool flag = false;
  ASSERT_EQ(arnm_json_read_bool(arnm_json_reader_root(owner.reader()), &flag), ARNM_SUCCESS);
  EXPECT_TRUE(flag);
}

// ---------------------------------------------------------------------------
// the type names
// ---------------------------------------------------------------------------

TEST(JsonReader, EveryTypeHasAName) {
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_NONE), "ARNM_JSON_TYPE_NONE");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_NULL), "ARNM_JSON_TYPE_NULL");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_BOOL), "ARNM_JSON_TYPE_BOOL");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_NUMBER), "ARNM_JSON_TYPE_NUMBER");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_STRING), "ARNM_JSON_TYPE_STRING");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_ARRAY), "ARNM_JSON_TYPE_ARRAY");
  EXPECT_STREQ(arnm_json_type_to_string(ARNM_JSON_TYPE_OBJECT), "ARNM_JSON_TYPE_OBJECT");
  EXPECT_STREQ(arnm_json_type_to_string(static_cast<arnm_json_type>(99)), "ARNM_JSON_TYPE_UNKNOWN");
}

// ---------------------------------------------------------------------------
// reading field by field: what a struct mapper does
// ---------------------------------------------------------------------------

TEST(JsonReader, AStructIsFilledInOneRunAndAskedAboutOnce) {
  // The whole point of the field level: no test between the lines, one at the end.
  ArenaReader owner;
  ASSERT_EQ(
      Parse(
          owner.reader(),
          "{\"name\":\"arnm\",\"age\":11,\"ratio\":0.25,\"cold\":-7,\"active\":true}"
      ),
      ARNM_SUCCESS
  );

  struct {
    const char *name;
    uint32_t age;
    double ratio;
    int32_t cold;
    bool active;
  } mapped{};

  mapped.name = arnm_json_reader_get_string(owner.reader(), "name");
  mapped.age = arnm_json_reader_get_uint32(owner.reader(), "age");
  mapped.ratio = arnm_json_reader_get_double(owner.reader(), "ratio");
  mapped.cold = arnm_json_reader_get_int32(owner.reader(), "cold");
  mapped.active = arnm_json_reader_get_bool(owner.reader(), "active");

  ASSERT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "");
  EXPECT_STREQ(mapped.name, "arnm");
  EXPECT_EQ(mapped.age, 11u);
  EXPECT_DOUBLE_EQ(mapped.ratio, 0.25);
  EXPECT_EQ(mapped.cold, -7);
  EXPECT_TRUE(mapped.active);

  EXPECT_EQ(arnm_json_reader_get_int64(owner.reader(), "age"), 11);
  EXPECT_EQ(arnm_json_reader_get_uint64(owner.reader(), "age"), 11u);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
}

TEST(JsonReader, TheFirstRefusalIsTheOneThatStays) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"text\",\"b\":true}"), ARNM_SUCCESS);

  // three refusals in a row, of three different kinds; the first is what is reported
  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 0u);
  EXPECT_EQ(arnm_json_reader_get_string(owner.reader(), "b"), nullptr);
  EXPECT_EQ(arnm_json_reader_get_bool(owner.reader(), "missing"), false);

  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "a");

  // and a field that would have read cleanly stays quiet too, so nothing half read reaches a
  // struct that is already known to be wrong
  EXPECT_EQ(arnm_json_reader_get_bool(owner.reader(), "b"), false);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "a");
}

TEST(JsonReader, EachKindOfRefusalIsNamedAndBlamedOnItsField) {
  const std::string document = "{\"text\":\"x\",\"big\":9999999999,\"real\":1.5}";

  {
    ArenaReader owner;
    ASSERT_EQ(Parse(owner.reader(), document), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_reader_get_string(owner.reader(), "nowhere"), nullptr);
    EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_PARAM);
    EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "nowhere");
  }
  {
    ArenaReader owner;
    ASSERT_EQ(Parse(owner.reader(), document), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "big"), 0u);
    EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_ARITHMETIC_OVERFLOW);
    EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "big");
  }
  {
    ArenaReader owner;
    ASSERT_EQ(Parse(owner.reader(), document), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_reader_get_int64(owner.reader(), "real"), 0);
    EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_ARITHMETIC_OVERFLOW);
    EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "real");
    // the same field reads whole as a double, once the verdict is cleared
    ASSERT_EQ(arnm_json_reader_clear_error(owner.reader()), ARNM_SUCCESS);
    EXPECT_DOUBLE_EQ(arnm_json_reader_get_double(owner.reader(), "real"), 1.5);
    EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
    EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "");
  }
  {
    // a document that is not an object at all: there is no field to name, and the type of the
    // current value is what is wrong
    ArenaReader owner;
    ASSERT_EQ(Parse(owner.reader(), "[1,2]"), ARNM_SUCCESS);
    EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 0u);
    EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);
    EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "a");
  }
}

TEST(JsonReader, ALongFieldNameIsKeptAsFarAsItFits) {
  const std::string key(80, 'k');
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{}"), ARNM_SUCCESS);

  EXPECT_EQ(arnm_json_reader_get_string(owner.reader(), key.c_str()), nullptr);
  const std::string recorded = arnm_json_reader_error_field(owner.reader());
  EXPECT_EQ(recorded.size(), ARNM_JSON_READER_FIELD_NAME_SIZE - 1u);
  EXPECT_EQ(recorded, key.substr(0, recorded.size()))
      << "truncated at the end, never at the front: the first bytes are what points at a member";
}

TEST(JsonReader, AParseIsWhereTheVerdictStartsOver) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_get_string(owner.reader(), "a"), nullptr);
  ASSERT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);

  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"text\"}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "");
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "a"), "text");
}

TEST(JsonReader, AFailedParseIsTheFirstErrorItself) {
  // A mapper that ignores the parse result still learns about it, at the one place it looks.
  ArenaReader owner;
  EXPECT_EQ(Parse(owner.reader(), "{\"a\":}"), ARNM_ERROR_DECODE_FAILED);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_DECODE_FAILED);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "") << "a parse belongs to no field";

  EXPECT_EQ(arnm_json_reader_get_string(owner.reader(), "a"), nullptr);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_DECODE_FAILED)
      << "the refusal that follows the parse must not overwrite it";

  // a length no parse can work with is recorded the same way
  ArenaReader empty;
  EXPECT_EQ(arnm_json_reader_parse(empty.reader(), "{}", 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_json_reader_status(empty.reader()), ARNM_ERROR_INVALID_PARAM);
}

TEST(JsonReader, ReadingWithoutADocumentIsRefusedAndNotWalkedInto) {
  ArenaReader owner;
  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 0u);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_STATE);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "a");

  // and a released document leaves the same state behind
  ArenaReader released;
  ASSERT_EQ(Parse(released.reader(), "{\"a\":1}"), ARNM_SUCCESS);
  ASSERT_NE(arnm_json_reader_release(released.reader()), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_current(released.reader()), nullptr);
  EXPECT_EQ(arnm_json_reader_get_uint32(released.reader(), "a"), 0u);
  EXPECT_EQ(arnm_json_reader_status(released.reader()), ARNM_ERROR_INVALID_STATE);
}

TEST(JsonReader, AnUninitializedReaderAnswersEmptyWithoutRecordingAnything) {
  arnm_json_reader reader;
  std::memset(&reader, 0, sizeof(reader));

  EXPECT_EQ(arnm_json_reader_status(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_STREQ(arnm_json_reader_error_field(&reader), "");
  EXPECT_EQ(arnm_json_reader_clear_error(&reader), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_set_output_allocator(&reader, nullptr), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arnm_json_reader_get_string(&reader, "a"), nullptr);
  EXPECT_EQ(arnm_json_reader_get_uint32(&reader, "a"), 0u);
  EXPECT_EQ(arnm_json_reader_get_double(&reader, "a"), 0.0);
  EXPECT_FALSE(arnm_json_reader_get_bool(&reader, "a"));
  EXPECT_EQ(arnm_json_reader_count(&reader), 0u);
  EXPECT_FALSE(arnm_json_reader_has(&reader, "a"));
  EXPECT_EQ(arnm_json_reader_type_of(&reader, "a"), ARNM_JSON_TYPE_NONE);
  EXPECT_EQ(arnm_json_reader_current(&reader), nullptr);
  EXPECT_EQ(arnm_json_reader_enter(&reader, "a"), nullptr);
  EXPECT_EQ(arnm_json_reader_enter_at(&reader, 0), nullptr);
  arnm_json_reader_leave(&reader, nullptr);
}

TEST(JsonReader, NullReachesEveryFieldLevelCallWithoutHarm) {
  EXPECT_EQ(arnm_json_reader_status(nullptr), ARNM_ERROR_NOT_INITIALIZED);
  EXPECT_STREQ(arnm_json_reader_error_field(nullptr), "");
  EXPECT_EQ(arnm_json_reader_clear_error(nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_reader_set_output_allocator(nullptr, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_reader_get_string(nullptr, "a"), nullptr);
  EXPECT_EQ(arnm_json_reader_get_string_length(nullptr, "a", nullptr), nullptr);
  EXPECT_EQ(arnm_json_reader_get_int64(nullptr, "a"), 0);
  EXPECT_EQ(arnm_json_reader_get_uint64(nullptr, "a"), 0u);
  EXPECT_EQ(arnm_json_reader_get_int32(nullptr, "a"), 0);
  EXPECT_EQ(arnm_json_reader_get_uint32(nullptr, "a"), 0u);
  EXPECT_EQ(arnm_json_reader_get_double(nullptr, "a"), 0.0);
  EXPECT_FALSE(arnm_json_reader_get_bool(nullptr, "a"));
  EXPECT_EQ(arnm_json_reader_count(nullptr), 0u);
  EXPECT_FALSE(arnm_json_reader_has(nullptr, "a"));
  EXPECT_EQ(arnm_json_reader_type_of(nullptr, "a"), ARNM_JSON_TYPE_NONE);
  EXPECT_EQ(arnm_json_reader_current(nullptr), nullptr);
  EXPECT_EQ(arnm_json_reader_enter(nullptr, "a"), nullptr);
  EXPECT_EQ(arnm_json_reader_enter_at(nullptr, 0), nullptr);
  arnm_json_reader_leave(nullptr, nullptr);
}

TEST(JsonReader, AnOptionalMemberIsAskedAboutBeforeItIsRead) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"note\":\"here\",\"empty\":null,\"n\":0}"), ARNM_SUCCESS);

  EXPECT_TRUE(arnm_json_reader_has(owner.reader(), "note"));
  EXPECT_TRUE(arnm_json_reader_has(owner.reader(), "n")) << "zero is a value like any other";
  EXPECT_FALSE(arnm_json_reader_has(owner.reader(), "empty"))
      << "for a mapper an explicit null and an absent member mean the same thing";
  EXPECT_FALSE(arnm_json_reader_has(owner.reader(), "nowhere"));
  EXPECT_TRUE(arnm_json_reader_has(owner.reader(), nullptr)) << "the current value itself";

  EXPECT_EQ(arnm_json_reader_type_of(owner.reader(), "note"), ARNM_JSON_TYPE_STRING);
  EXPECT_EQ(arnm_json_reader_type_of(owner.reader(), "empty"), ARNM_JSON_TYPE_NULL);
  EXPECT_EQ(arnm_json_reader_type_of(owner.reader(), "nowhere"), ARNM_JSON_TYPE_NONE);
  EXPECT_EQ(arnm_json_reader_type_of(owner.reader(), nullptr), ARNM_JSON_TYPE_OBJECT);

  // asking is not reading: nothing above may have been recorded
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);

  const char *note = arnm_json_reader_has(owner.reader(), "note")
                         ? arnm_json_reader_get_string(owner.reader(), "note")
                         : "";
  const char *missing = arnm_json_reader_has(owner.reader(), "gone")
                            ? arnm_json_reader_get_string(owner.reader(), "gone")
                            : "";
  EXPECT_STREQ(note, "here");
  EXPECT_STREQ(missing, "");
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
}

// ---------------------------------------------------------------------------
// moving through the document
// ---------------------------------------------------------------------------

TEST(JsonReader, NestingCostsTwoLinesAndNoBookkeeping) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(
          owner.reader(),
          "{\"id\":3,\"address\":{\"city\":\"Bern\",\"geo\":{\"lat\":46.9}},\"tail\":true}"
      ),
      ARNM_SUCCESS
  );

  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "id"), 3u);

  arnm_json_value *outer = arnm_json_reader_enter(owner.reader(), "address");
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "city"), "Bern");
  arnm_json_value *inner = arnm_json_reader_enter(owner.reader(), "geo");
  EXPECT_DOUBLE_EQ(arnm_json_reader_get_double(owner.reader(), "lat"), 46.9);
  arnm_json_reader_leave(owner.reader(), inner);
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "city"), "Bern")
      << "leaving comes back exactly where the enter started";
  arnm_json_reader_leave(owner.reader(), outer);

  EXPECT_TRUE(arnm_json_reader_get_bool(owner.reader(), "tail"));
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_current(owner.reader()), arnm_json_reader_root(owner.reader()));
}

TEST(JsonReader, AFailedEnterStillHandsTheWayBack) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"scalar\":7}"), ARNM_SUCCESS);

  arnm_json_value *saved = arnm_json_reader_enter(owner.reader(), "nowhere");
  EXPECT_EQ(saved, arnm_json_reader_root(owner.reader()))
      << "the pairing has to hold on every path, or every level would need a test of its own";
  EXPECT_EQ(arnm_json_reader_current(owner.reader()), nullptr);

  // everything below the failed step is quiet, and none of it changes the verdict
  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "whatever"), 0u);
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 0u);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_PARAM);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "nowhere");

  arnm_json_reader_leave(owner.reader(), saved);
  EXPECT_EQ(arnm_json_reader_current(owner.reader()), arnm_json_reader_root(owner.reader()));

  // the way back is there, the verdict stays until someone clears it
  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 0u);
  ASSERT_EQ(arnm_json_reader_clear_error(owner.reader()), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 1u);
}

TEST(JsonReader, EnteringSomethingThatIsNoObjectIsRefusedByType) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"scalar\":7}"), ARNM_SUCCESS);

  arnm_json_value *saved = arnm_json_reader_enter(owner.reader(), "scalar");
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "anything"), nullptr);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "anything");
  arnm_json_reader_leave(owner.reader(), saved);
}

TEST(JsonReader, AnArrayOfObjectsIsWalkedOnceFromTheFront) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(
          owner.reader(),
          "{\"items\":[{\"n\":\"a\",\"v\":1},{\"n\":\"b\",\"v\":2},{\"n\":\"c\",\"v\":3}]}"
      ),
      ARNM_SUCCESS
  );

  arnm_json_value *array = arnm_json_reader_enter(owner.reader(), "items");
  ASSERT_EQ(arnm_json_reader_count(owner.reader()), 3u);

  std::vector<std::string> names;
  std::vector<uint32_t> values;
  for (uint32_t index = 0; index < arnm_json_reader_count(owner.reader()); ++index) {
    arnm_json_value *element = arnm_json_reader_enter_at(owner.reader(), index);
    names.emplace_back(arnm_json_reader_get_string(owner.reader(), "n"));
    values.push_back(arnm_json_reader_get_uint32(owner.reader(), "v"));
    arnm_json_reader_leave(owner.reader(), element);
  }
  arnm_json_reader_leave(owner.reader(), array);

  EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(values, (std::vector<uint32_t>{1u, 2u, 3u}));
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
}

TEST(JsonReader, AnArrayIsIndexedInAnyOrderAndAlwaysAnswersTheSame) {
  // The remembered walk is a shortcut and never an answer: jumping backwards has to start the
  // chain again rather than hand out whatever it stopped at.
  ArenaReader owner;
  std::string json = "{\"a\":[";
  for (uint32_t index = 0; index < 64u; ++index) {
    json += (index > 0 ? "," : "") + std::to_string(index * 10u);
  }
  json += "]}";
  ASSERT_EQ(Parse(owner.reader(), json), ARNM_SUCCESS);

  arnm_json_value *array = arnm_json_reader_enter(owner.reader(), "a");
  ASSERT_EQ(arnm_json_reader_count(owner.reader()), 64u);

  const uint32_t order[] = {0u, 1u, 2u, 63u, 5u, 5u, 4u, 62u, 0u, 63u};
  for (uint32_t index : order) {
    arnm_json_value *element = arnm_json_reader_enter_at(owner.reader(), index);
    EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), nullptr), index * 10u)
        << "at index " << index;
    arnm_json_reader_leave(owner.reader(), element);
  }
  arnm_json_reader_leave(owner.reader(), array);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
}

TEST(JsonReader, AnIndexPastTheEndIsRecordedUnderItsPosition) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":[1,2]}"), ARNM_SUCCESS);

  arnm_json_value *array = arnm_json_reader_enter(owner.reader(), "a");
  arnm_json_value *element = arnm_json_reader_enter_at(owner.reader(), 12);
  EXPECT_EQ(arnm_json_reader_current(owner.reader()), nullptr);
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "[12]");
  arnm_json_reader_leave(owner.reader(), element);
  arnm_json_reader_leave(owner.reader(), array);
}

TEST(JsonReader, AnIndexIntoSomethingThatIsNoArrayIsRefusedByType) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1}"), ARNM_SUCCESS);

  arnm_json_value *saved = arnm_json_reader_enter_at(owner.reader(), 0);
  EXPECT_EQ(saved, arnm_json_reader_root(owner.reader()));
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "[0]");
  arnm_json_reader_leave(owner.reader(), saved);
}

TEST(JsonReader, ANullKeyReadsTheCurrentValueItself) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[\"one\",2,true,3.5]"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_count(owner.reader()), 4u);

  arnm_json_value *first = arnm_json_reader_enter_at(owner.reader(), 0);
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), nullptr), "one");
  arnm_json_reader_leave(owner.reader(), first);

  arnm_json_value *second = arnm_json_reader_enter_at(owner.reader(), 1);
  EXPECT_EQ(arnm_json_reader_get_uint64(owner.reader(), nullptr), 2u);
  arnm_json_reader_leave(owner.reader(), second);

  arnm_json_value *third = arnm_json_reader_enter_at(owner.reader(), 2);
  EXPECT_TRUE(arnm_json_reader_get_bool(owner.reader(), nullptr));
  arnm_json_reader_leave(owner.reader(), third);

  arnm_json_value *fourth = arnm_json_reader_enter_at(owner.reader(), 3);
  EXPECT_DOUBLE_EQ(arnm_json_reader_get_double(owner.reader(), nullptr), 3.5);
  arnm_json_reader_leave(owner.reader(), fourth);

  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
}

TEST(JsonReader, CountAnswersZeroWhereALoopShouldNotRun) {
  ArenaReader owner;
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 0u) << "no document";

  ASSERT_EQ(Parse(owner.reader(), "{\"a\":[1,2,3],\"o\":{\"x\":1},\"s\":\"text\"}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 3u) << "the root object holds three pairs";

  arnm_json_value *array = arnm_json_reader_enter(owner.reader(), "a");
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 3u);
  arnm_json_reader_leave(owner.reader(), array);

  arnm_json_value *object = arnm_json_reader_enter(owner.reader(), "o");
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 1u);
  arnm_json_reader_leave(owner.reader(), object);

  arnm_json_value *scalar = arnm_json_reader_enter(owner.reader(), "s");
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 0u) << "a scalar has no members";
  arnm_json_reader_leave(owner.reader(), scalar);

  EXPECT_EQ(arnm_json_reader_get_uint32(owner.reader(), "a"), 0u);
  EXPECT_EQ(arnm_json_reader_count(owner.reader()), 0u) << "an error already recorded";
}

// ---------------------------------------------------------------------------
// borrowed strings, or copied ones
// ---------------------------------------------------------------------------

TEST(JsonReader, WithoutAnOutputAllocatorAStringIsBorrowedFromTheDocument) {
  ArenaReader owner;
  const std::string json = "{\"text\":\"borrowed\"}";
  ASSERT_EQ(Parse(owner.reader(), json), ARNM_SUCCESS);

  const uint32_t before = owner.used();
  const char *text = arnm_json_reader_get_string(owner.reader(), "text");
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "borrowed");
  EXPECT_EQ(owner.used(), before) << "borrowing draws nothing from anywhere";

  // the value level borrows either way, which is what it is for
  arnm_json_value *field = nullptr;
  ASSERT_EQ(
      arnm_json_object_get(arnm_json_reader_root(owner.reader()), "text", &field), ARNM_SUCCESS
  );
  const char *same = nullptr;
  ASSERT_EQ(arnm_json_read_string(field, &same, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(same, text) << "the same bytes, not a second copy";
}

TEST(JsonReader, AnOutputAllocatorLiftsEveryStringOutOfTheDocument) {
  // insitu, so the bytes a borrowed string would point at are the caller's own buffer -- and
  // overwriting it afterwards is what tells a copy from a loan.
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, 4096), ARNM_SUCCESS);
  const uintptr_t output_before = ArenaMark(&output);

  ArenaReader owner;
  const std::string json = "{\"text\":\"lifted\",\"n\":5}";
  std::vector<char> buffer = InsituBuffer(json);

  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);
  ASSERT_EQ(ParseInsitu(owner.reader(), buffer, json.size()), ARNM_SUCCESS);

  uint32_t length = 0;
  const char *text = arnm_json_reader_get_string_length(owner.reader(), "text", &length);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "lifted");
  EXPECT_EQ(length, 6u);
  EXPECT_GT(ArenaMark(&output), output_before)
      << "the copy comes from the allocator that was named";

  // the document goes, and the buffer under it is scribbled over; the copy is untouched
  ASSERT_NE(arnm_json_reader_release(owner.reader()), ARNM_ERROR_NOT_INITIALIZED);
  std::fill(buffer.begin(), buffer.end(), 'X');
  EXPECT_STREQ(text, "lifted");

  arnm_release(&output);
}

TEST(JsonReader, BorrowingIsTakenUpAgainWhenTheOutputAllocatorIsCleared) {
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, 4096), ARNM_SUCCESS);

  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"one\",\"b\":\"two\"}"), ARNM_SUCCESS);

  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);
  const uintptr_t before = ArenaMark(&output);
  const char *copied = arnm_json_reader_get_string(owner.reader(), "a");
  const uintptr_t after_copy = ArenaMark(&output);

  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), nullptr), ARNM_SUCCESS);
  const char *borrowed = arnm_json_reader_get_string(owner.reader(), "b");

  EXPECT_STREQ(copied, "one");
  EXPECT_STREQ(borrowed, "two");
  EXPECT_GT(after_copy, before);
  EXPECT_EQ(ArenaMark(&output), after_copy) << "what borrows draws nothing";
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);

  arnm_release(&output);
}

TEST(JsonReader, AnEmptyStringIsCopiedAsOneTerminatedByte) {
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, 4096), ARNM_SUCCESS);

  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"\"}"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);

  uint32_t length = 1;
  const char *text = arnm_json_reader_get_string_length(owner.reader(), "a", &length);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(text[0], '\0');

  arnm_release(&output);
}

TEST(JsonReader, ACopiedStringKeepsAnEmbeddedNulAndItsLength) {
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, 4096), ARNM_SUCCESS);

  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"be\\u0000fore\"}"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);

  uint32_t length = 0;
  const char *text = arnm_json_reader_get_string_length(owner.reader(), "a", &length);
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(length, 7u) << "the length is what counts; strlen is the one that lies";
  EXPECT_EQ(std::string(text, length), std::string("be\0fore", 7));
  EXPECT_EQ(text[length], '\0') << "terminated on top of its own length, for a caller that scans";

  arnm_release(&output);
}

TEST(JsonReader, AnOutputAllocatorWithNoRoomIsRecordedAsOutOfMemory) {
  // A cap that cannot hold the copy, so the refusal is the allocator's and not a question of
  // how much memory the machine happens to have.
  uint8_t storage[16] = {0};
  arnm output{};
  ASSERT_EQ(arnm_init_arena_borrow(&output, storage, sizeof(storage)), ARNM_SUCCESS);

  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"far too long for sixteen bytes\"}"), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);

  uint32_t length = 99;
  EXPECT_EQ(arnm_json_reader_get_string_length(owner.reader(), "a", &length), nullptr);
  EXPECT_EQ(length, 99u) << "a refusal leaves every output untouched";
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_STREQ(arnm_json_reader_error_field(owner.reader()), "a");

  arnm_release(&output);
}

TEST(JsonReader, TheOutputAllocatorMayStandOnTheHost) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"host\"}"), ARNM_SUCCESS);

  arnm *host = nullptr;
  arnm_json_reader host_reader{};
  ASSERT_EQ(arnm_json_reader_init(&host_reader, host, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);
  ASSERT_EQ(Parse(&host_reader, "{\"a\":\"host\"}"), ARNM_SUCCESS);

  // NULL means "borrow" for the output allocator, so a copy onto the host needs an arena that
  // stands on the host -- which is what arnm_init_arena() is.
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, 256), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(&host_reader, &output), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_get_string(&host_reader, "a"), "host");
  EXPECT_EQ(arnm_json_reader_status(&host_reader), ARNM_SUCCESS);

  EXPECT_NE(arnm_json_reader_release(&host_reader), ARNM_ERROR_NOT_INITIALIZED);
  arnm_release(&output);
}

// ---------------------------------------------------------------------------
// measuring the output arena before it is built
// ---------------------------------------------------------------------------

namespace {

/** Copy every string in the subtree below the reader, the way a mapper would reach them. */
void CopyEveryString(arnm_json_reader *reader) {
  arnm_json_value *here = arnm_json_reader_current(reader);
  switch (arnm_json_value_type(here)) {
  case ARNM_JSON_TYPE_STRING:
    EXPECT_NE(arnm_json_reader_get_string(reader, nullptr), nullptr);
    break;
  case ARNM_JSON_TYPE_ARRAY:
    for (uint32_t index = 0, count = arnm_json_reader_count(reader); index < count; ++index) {
      arnm_json_value *element = arnm_json_reader_enter_at(reader, index);
      CopyEveryString(reader);
      arnm_json_reader_leave(reader, element);
    }
    break;
  case ARNM_JSON_TYPE_OBJECT: {
    arnm_json_object_iter iter{};
    ASSERT_EQ(arnm_json_object_iter_init(here, &iter), ARNM_SUCCESS);
    const char *key = nullptr;
    while (arnm_json_object_iter_next(&iter, &key, nullptr, nullptr)) {
      arnm_json_value *member = arnm_json_reader_enter(reader, key);
      CopyEveryString(reader);
      arnm_json_reader_leave(reader, member);
    }
    break;
  }
  default:
    break;
  }
}

/** A document with strings at every depth, plus members a mapper would never ask for. */
const char kMixedDocument[] =
    "{\"name\":\"arnm\",\"note\":\"a longer string that pays for its own rounding\","
    "\"port\":8443,\"flag\":true,\"exact\":\"12345678\","
    "\"address\":{\"city\":\"Bern\",\"zip\":\"3011\"},"
    "\"peers\":[{\"name\":\"alpha\",\"role\":\"primary\"},{\"name\":\"beta\",\"role\":\"backup\"}],"
    "\"tags\":[\"one\",\"two\",\"three\"]}";

} // namespace

TEST(JsonReader, TheOutputSizeIsExactlyWhatTheCopiesSpend) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kMixedDocument), ARNM_SUCCESS);

  const uint32_t reserve = arnm_json_reader_output_size(owner.reader());
  ASSERT_GT(reserve, 0u);

  // an arena of exactly that size, and nothing to spare in it
  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, reserve), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);

  CopyEveryString(owner.reader());
  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS)
      << "every copy has to fit, or the number was too small";

  uint8_t *spare = nullptr;
  EXPECT_EQ(arnm_alloc(&spare, 1, &output), ARNM_ERROR_OUT_OF_MEMORY)
      << "and not one byte may be left, or the number was too large";

  arnm_release(&output);
}

TEST(JsonReader, TheKeyedOutputSizeCoversOnlyWhatWasNamed) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kMixedDocument), ARNM_SUCCESS);

  const char *wanted[] = {"name", "city"};
  const uint32_t keyed = arnm_json_reader_output_size_for_keys(owner.reader(), wanted, 2);
  const uint32_t everything = arnm_json_reader_output_size(owner.reader());

  // "arnm", "alpha", "beta" -- the name of every element too, at whatever depth it sits -- and
  // "Bern" from the nested object
  EXPECT_EQ(
      keyed,
      ARNM_ALIGN8(4u + 1u) + ARNM_ALIGN8(5u + 1u) + ARNM_ALIGN8(4u + 1u) + ARNM_ALIGN8(4u + 1u)
  );
  EXPECT_LT(keyed, everything) << "the document carries members nobody asked for";

  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, keyed), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);

  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "name"), "arnm");
  arnm_json_value *address = arnm_json_reader_enter(owner.reader(), "address");
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "city"), "Bern");
  arnm_json_reader_leave(owner.reader(), address);

  arnm_json_value *peers = arnm_json_reader_enter(owner.reader(), "peers");
  for (uint32_t index = 0, count = arnm_json_reader_count(owner.reader()); index < count; ++index) {
    arnm_json_value *element = arnm_json_reader_enter_at(owner.reader(), index);
    EXPECT_NE(arnm_json_reader_get_string(owner.reader(), "name"), nullptr);
    arnm_json_reader_leave(owner.reader(), element);
  }
  arnm_json_reader_leave(owner.reader(), peers);

  EXPECT_EQ(arnm_json_reader_status(owner.reader()), ARNM_SUCCESS);
  uint8_t *spare = nullptr;
  EXPECT_EQ(arnm_alloc(&spare, 1, &output), ARNM_ERROR_OUT_OF_MEMORY);

  arnm_release(&output);
}

TEST(JsonReader, AKeyIsNeverCountedAsACopy) {
  ArenaReader owner;
  // the key is a string in the document and is never one in the output: a getter is handed the
  // caller's own name and gives back the value alone
  ASSERT_EQ(Parse(owner.reader(), "{\"a_very_long_member_name\":1}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_output_size(owner.reader()), 0u);

  ASSERT_EQ(Parse(owner.reader(), "{\"k\":\"v\"}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_output_size(owner.reader()), ARNM_ALIGN8(1u + 1u));
}

TEST(JsonReader, TheOutputSizeIsMeasuredFromWhereTheReaderStands) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kMixedDocument), ARNM_SUCCESS);
  const uint32_t whole = arnm_json_reader_output_size(owner.reader());

  arnm_json_value *address = arnm_json_reader_enter(owner.reader(), "address");
  const uint32_t part = arnm_json_reader_output_size(owner.reader());
  EXPECT_EQ(part, ARNM_ALIGN8(4u + 1u) + ARNM_ALIGN8(4u + 1u)) << "\"Bern\" and \"3011\"";
  EXPECT_LT(part, whole);
  arnm_json_reader_leave(owner.reader(), address);

  arnm_json_value *tags = arnm_json_reader_enter(owner.reader(), "tags");
  EXPECT_EQ(
      arnm_json_reader_output_size(owner.reader()),
      ARNM_ALIGN8(3u + 1u) + ARNM_ALIGN8(3u + 1u) + ARNM_ALIGN8(5u + 1u)
  );
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), nullptr, 0), 0u)
      << "an array element has no member name, so nothing keyed can reach it";
  arnm_json_reader_leave(owner.reader(), tags);

  EXPECT_EQ(arnm_json_reader_output_size(owner.reader()), whole)
      << "leaving puts the measurement back where it was";
}

TEST(JsonReader, MeasuringAnswersZeroWhereThereIsNothingToCarry) {
  ArenaReader empty;
  EXPECT_EQ(arnm_json_reader_output_size(empty.reader()), 0u) << "no document";
  EXPECT_EQ(arnm_json_reader_output_size(nullptr), 0u);
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(nullptr, nullptr, 0), 0u);

  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"n\":1,\"b\":true,\"z\":null,\"a\":[1,2]}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_output_size(owner.reader()), 0u) << "not a string in sight";

  const char *wanted[] = {"n", nullptr};
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), wanted, 2), 0u)
      << "a NULL entry is skipped, and a number is no copy";
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), wanted, 0), 0u);
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), nullptr, 2), 0u);
}

TEST(JsonReader, AnEmptyStringStillCostsItsRoundedTerminator) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"\",\"b\":\"\"}"), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_output_size(owner.reader()), 2u * ARNM_ALIGN8(0u + 1u));

  arnm output{};
  ASSERT_EQ(arnm_init_arena(&output, arnm_json_reader_output_size(owner.reader())), ARNM_SUCCESS);
  ASSERT_EQ(arnm_json_reader_set_output_allocator(owner.reader(), &output), ARNM_SUCCESS);
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "a"), "");
  EXPECT_STREQ(arnm_json_reader_get_string(owner.reader(), "b"), "");
  uint8_t *spare = nullptr;
  EXPECT_EQ(arnm_alloc(&spare, 1, &output), ARNM_ERROR_OUT_OF_MEMORY);
  arnm_release(&output);
}

TEST(JsonReader, MeasuringADeepDocumentCostsNoStack) {
  // The measurement walks the document's value array and not its tree, so nesting is a number
  // and not a call depth. A recursive walk would have gone over the side long before this.
  constexpr uint32_t kDepth = 60000;
  std::string json(kDepth, '[');
  json += "\"deep\"";
  json.append(kDepth, ']');

  // on the host: sixty thousand values do not fit the arena the other tests share
  arnm_json_reader reader{};
  ASSERT_EQ(arnm_json_reader_init(&reader, nullptr, ARNM_JSON_READ_DEFAULT), ARNM_SUCCESS);
  ASSERT_EQ(Parse(&reader, json), ARNM_SUCCESS);
  EXPECT_EQ(arnm_json_reader_output_size(&reader), ARNM_ALIGN8(4u + 1u));
  EXPECT_EQ(arnm_json_reader_value_count(&reader), kDepth + 1u);
  EXPECT_EQ(arnm_json_reader_release(&reader), ARNM_SUCCESS);
}

TEST(JsonReader, TheSameNameTwiceIsCountedTwice) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":\"xy\"}"), ARNM_SUCCESS);
  const char *once[] = {"a"};
  const char *twice[] = {"a", "a"};
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), once, 1), ARNM_ALIGN8(2u + 1u));
  EXPECT_EQ(arnm_json_reader_output_size_for_keys(owner.reader(), twice, 2), ARNM_ALIGN8(2u + 1u))
      << "a name that matches stops the comparison; the list is read as given, not summed twice";
}

// ---------------------------------------------------------------------------
// the shapes a string carries: hex, base64, uuid
// ---------------------------------------------------------------------------

namespace {

/** The value of the single member `v` of a document, parsed through @p reader. */
arnm_json_value *SingleValue(ArenaReader &owner, const std::string &spelled) {
  const std::string json = "{\"v\":" + spelled + "}";
  EXPECT_EQ(Parse(owner.reader(), json), ARNM_SUCCESS) << json;
  arnm_json_value *member = nullptr;
  EXPECT_EQ(
      arnm_json_object_get(arnm_json_reader_root(owner.reader()), "v", &member), ARNM_SUCCESS
  );
  return member;
}

} // namespace

TEST(JsonReader, HexFixedReadsExactlyTheFieldItWasGiven) {
  ArenaReader owner;
  uint8_t out[4] = {0, 0, 0, 0};
  EXPECT_EQ(arnm_json_read_hex_fixed(SingleValue(owner, "\"deadbeef\""), out, 4), ARNM_SUCCESS);
  EXPECT_EQ(out[0], 0xdeu);
  EXPECT_EQ(out[1], 0xadu);
  EXPECT_EQ(out[2], 0xbeu);
  EXPECT_EQ(out[3], 0xefu);
}

TEST(JsonReader, HexFixedRefusesEveryLengthButItsOwn) {
  ArenaReader owner;
  uint8_t out[4] = {0};
  // one byte short, one byte long, and the odd number that is no hex string at all
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "\"deadbe\""), out, 4), ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "\"deadbeef00\""), out, 4),
      ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "\"deadbeefa\""), out, 4),
      ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "\"deadbeez\""), out, 4), ARNM_ERROR_DECODE_FAILED
  );
}

TEST(JsonReader, HexReadsWhateverLengthTheDocumentSpells) {
  ArenaReader owner;
  uint8_t out[8] = {0};
  uint32_t size = 0;
  EXPECT_EQ(
      arnm_json_read_hex(SingleValue(owner, "\"0a0b0c\""), out, sizeof(out), &size), ARNM_SUCCESS
  );
  EXPECT_EQ(size, 3u);
  EXPECT_EQ(out[0], 0x0au);
  EXPECT_EQ(out[2], 0x0cu);

  // an empty string spells no bytes, and no bytes is not a length this reads -- the converter
  // underneath refuses it and a document that carries one is a document that is wrong
  size = 99u;
  EXPECT_EQ(
      arnm_json_read_hex(SingleValue(owner, "\"\""), out, sizeof(out), &size),
      ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(size, 99u) << "a refusal leaves the size as the caller had it";
}

TEST(JsonReader, HexRefusesMoreBytesThanTheBufferHolds) {
  ArenaReader owner;
  uint8_t out[2] = {0xaa, 0xaa};
  EXPECT_EQ(
      arnm_json_read_hex(SingleValue(owner, "\"00112233\""), out, sizeof(out), nullptr),
      ARNM_ERROR_DECODE_FAILED
  );
  // refused before the converter ran, so the buffer is as the caller had it
  EXPECT_EQ(out[0], 0xaau);
  EXPECT_EQ(out[1], 0xaau);
}

TEST(JsonReader, AStringThatEndsBeforeTheDocumentSaysIsNoHex) {
  // the converter reads to the terminator; a NUL of the string's own would stop it early and
  // leave the rest of the field untouched, so the string is refused instead
  ArenaReader owner;
  uint8_t out[4] = {0};
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "\"dead\\u0000beef\""), out, 4),
      ARNM_ERROR_DECODE_FAILED
  );
  uint32_t size = 0;
  EXPECT_EQ(
      arnm_json_read_hex(SingleValue(owner, "\"dead\\u0000beef\""), out, sizeof(out), &size),
      ARNM_ERROR_DECODE_FAILED
  );
}

TEST(JsonReader, UuidReadsTheCanonicalFormAndNothingElse) {
  ArenaReader owner;
  uint8_t out[ARNM_UUID_BINARY_SIZE] = {0};
  EXPECT_EQ(
      arnm_json_read_uuid(SingleValue(owner, "\"019e2c31-a303-75c0-941e-f35c59e4f978\""), out),
      ARNM_SUCCESS
  );
  EXPECT_EQ(out[0], 0x01u);
  EXPECT_EQ(out[15], 0x78u);

  EXPECT_EQ(
      arnm_json_read_uuid(SingleValue(owner, "\"019e2c31a30375c0941ef35c59e4f978\""), out),
      ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(
      arnm_json_read_uuid(SingleValue(owner, "\"not-a-uuid\""), out), ARNM_ERROR_DECODE_FAILED
  );
}

TEST(JsonReader, Base64BlockTakesExactlyWhatTheStringDecodesTo) {
  ArenaReader owner;
  arnm_memory_block block{};
  // "hello" is five bytes, which the padded eight characters would over-measure by one
  EXPECT_EQ(
      arnm_json_read_base64_block(&block, SingleValue(owner, "\"aGVsbG8=\""), owner.arena()),
      ARNM_SUCCESS
  );
  ASSERT_NE(block.data, nullptr);
  EXPECT_EQ(block.size, 5u);
  EXPECT_EQ(0, memcmp(block.data, "hello", 5));
  EXPECT_EQ(arnm_memory_block_free(&block, owner.arena()), ARNM_SUCCESS);
}

TEST(JsonReader, AnEmptyBase64StringCostsNoAllocation) {
  ArenaReader owner;
  const uint32_t before = owner.used();
  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_read_base64_block(&block, SingleValue(owner, "\"\""), owner.arena()), ARNM_SUCCESS
  );
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);
  EXPECT_GE(owner.used(), before); // the document itself is what grew, if anything did
}

TEST(JsonReader, Base64BlockRefusesWhatIsNotBase64) {
  ArenaReader owner;
  arnm_memory_block block{};
  // five characters are not a whole number of four character groups
  EXPECT_EQ(
      arnm_json_read_base64_block(&block, SingleValue(owner, "\"aGVsb\""), owner.arena()),
      ARNM_ERROR_DECODE_FAILED
  );
  EXPECT_EQ(block.data, nullptr);
  // and this one is the right length with a character the alphabet does not have
  EXPECT_EQ(
      arnm_json_read_base64_block(&block, SingleValue(owner, "\"aGVs*G8=\""), owner.arena()),
      ARNM_ERROR_DECODE_FAILED
  );
}

TEST(JsonReader, TheShapeReadsRefuseAValueThatIsNoString) {
  ArenaReader owner;
  uint8_t out[4] = {0};
  arnm_memory_block block{};
  EXPECT_EQ(
      arnm_json_read_hex_fixed(SingleValue(owner, "42"), out, 4), ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(
      arnm_json_read_hex(SingleValue(owner, "42"), out, sizeof(out), nullptr),
      ARNM_ERROR_INVALID_ENUM_TYPE
  );
  EXPECT_EQ(arnm_json_read_uuid(SingleValue(owner, "42"), out), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(
      arnm_json_read_base64_block(&block, SingleValue(owner, "42"), owner.arena()),
      ARNM_ERROR_INVALID_ENUM_TYPE
  );
}

TEST(JsonReader, TheShapeReadsRefuseNullArguments) {
  ArenaReader owner;
  uint8_t out[4] = {0};
  arnm_memory_block block{};
  EXPECT_EQ(arnm_json_read_hex_fixed(nullptr, out, 4), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_hex(nullptr, out, 4, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uuid(nullptr, out), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_base64_block(&block, nullptr, owner.arena()), ARNM_ERROR_NULL_POINTER);
  arnm_json_value *value = SingleValue(owner, "\"00\"");
  EXPECT_EQ(arnm_json_read_hex_fixed(value, nullptr, 1), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_uuid(value, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_json_read_base64_block(nullptr, value, owner.arena()), ARNM_ERROR_NULL_POINTER);
}

// ---------------------------------------------------------------------------
// reading a whole object in one walk
// ---------------------------------------------------------------------------

namespace {

/** Every target one walk can fill, together in the shape a consumer would hold them. */
struct Shape {
  bool flag = false;
  int64_t wide = 0;
  uint32_t narrow = 0;
  double real = 0.0;
  arnm_memory_block text{};
  uint8_t key_bytes[4] = {0, 0, 0, 0};
  arnm_memory_block key = ARNM_JSON_BLOCK_OF(key_bytes);
  uint8_t uuid_bytes[ARNM_UUID_BINARY_SIZE] = {0};
  arnm_memory_block uuid = ARNM_JSON_BLOCK_OF(uuid_bytes);
  arnm_json_value *nested = nullptr;
};

const char kShapeDocument[] =
    "{\"flag\":true,\"wide\":-9,\"narrow\":7,\"real\":0.25,\"text\":\"here\","
    "\"key\":\"deadbeef\",\"uuid\":\"019e2c31-a303-75c0-941e-f35c59e4f978\","
    "\"nested\":{\"deep\":1}}";

/** The one line every walk below reads the same way. */
arnm_result Walk(ArenaReader &owner, arnm_json_field *fields, uint32_t count, uint64_t *found) {
  return arnm_json_read_object(arnm_json_reader_root(owner.reader()), fields, count, found);
}

} // namespace

TEST(JsonReader, AWalkFillsEveryTargetItsTableNames) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), kShapeDocument), ARNM_SUCCESS);
  Shape shape;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_BOOL("flag", &shape.flag),
                              ARNM_JSON_FIELD_INT64("wide", &shape.wide),
                              ARNM_JSON_FIELD_UINT32("narrow", &shape.narrow),
                              ARNM_JSON_FIELD_DOUBLE("real", &shape.real),
                              ARNM_JSON_FIELD_STRING("text", &shape.text),
                              ARNM_JSON_FIELD_HEX_FIXED("key", &shape.key),
                              ARNM_JSON_FIELD_UUID("uuid", &shape.uuid),
                              ARNM_JSON_FIELD_VALUE("nested", &shape.nested)};
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 8, &found), ARNM_SUCCESS);

  EXPECT_EQ(found, 0xffull) << "every one of the eight was there";
  EXPECT_TRUE(shape.flag);
  EXPECT_EQ(shape.wide, -9);
  EXPECT_EQ(shape.narrow, 7u);
  EXPECT_DOUBLE_EQ(shape.real, 0.25);
  ASSERT_NE(shape.text.data, nullptr);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(shape.text.data), shape.text.size), "here");
  EXPECT_EQ(shape.key_bytes[0], 0xdeu);
  EXPECT_EQ(shape.key_bytes[3], 0xefu);
  EXPECT_EQ(shape.uuid_bytes[0], 0x01u);
  EXPECT_EQ(shape.uuid_bytes[15], 0x78u);
  ASSERT_NE(shape.nested, nullptr);
  EXPECT_EQ(arnm_json_value_type(shape.nested), ARNM_JSON_TYPE_OBJECT)
      << "a nested member is handed over, not walked";
}

TEST(JsonReader, AWalkBorrowsAStringRatherThanCopyingIt) {
  // the block points into the document itself, which is what makes a string field cost nothing
  // and what makes it illegal to free
  ArenaReader owner;
  const std::string json = "{\"text\":\"here\"}";
  ASSERT_EQ(Parse(owner.reader(), json), ARNM_SUCCESS);
  arnm_memory_block text{};
  arnm_json_field fields[] = {ARNM_JSON_FIELD_STRING("text", &text)};
  const uint32_t before = owner.used();
  ASSERT_EQ(Walk(owner, fields, 1, nullptr), ARNM_SUCCESS);

  EXPECT_EQ(text.size, 4u) << "size is the string's length, not an allocation";
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(text.data), text.size), "here");
  EXPECT_EQ(owner.used(), before) << "a walk that only borrows takes no arena byte";
}

TEST(JsonReader, AWalkReadsTheSameDocumentInAnyOrder) {
  // the table starts each key at its lowest unfilled entry; a document in another order costs
  // the entries above it and answers the same
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"third\":3,\"first\":1,\"second\":2}"), ARNM_SUCCESS);
  int64_t first = 0, second = 0, third = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("first", &first), ARNM_JSON_FIELD_INT64("second", &second),
      ARNM_JSON_FIELD_INT64("third", &third)
  };
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 3, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x7ull);
  EXPECT_EQ(first, 1);
  EXPECT_EQ(second, 2);
  EXPECT_EQ(third, 3);
}

TEST(JsonReader, AWalkKeepsTheFirstOfADuplicateWhereverItSits) {
  // an entry is skipped once filled, so the second of a pair never reaches its target. The two
  // positions are asked separately: the field the walk is already past, and the field it would
  // still be scanning towards
  ArenaReader owner;
  {
    ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"a\":2,\"b\":3}"), ARNM_SUCCESS);
    int64_t a = 0, b = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b)};
    uint64_t found = 0;
    ASSERT_EQ(Walk(owner, fields, 2, &found), ARNM_SUCCESS);
    EXPECT_EQ(found, 0x3ull);
    EXPECT_EQ(a, 1) << "the duplicate below the lowest open entry is passed over";
    EXPECT_EQ(b, 3);
  }
  {
    ASSERT_EQ(Parse(owner.reader(), "{\"b\":1,\"b\":2}"), ARNM_SUCCESS);
    int64_t a = -1, b = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b)};
    uint64_t found = 0;
    ASSERT_EQ(Walk(owner, fields, 2, &found), ARNM_SUCCESS);
    EXPECT_EQ(found, 0x2ull);
    EXPECT_EQ(b, 1) << "the duplicate the scan still passes over is skipped by its filled bit";
    EXPECT_EQ(a, -1);
  }
  {
    ASSERT_EQ(Parse(owner.reader(), "{\"w\":1,\"w\":2}"), ARNM_SUCCESS);
    int64_t w = 0;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("w", &w)};
    uint64_t found = 0;
    ASSERT_EQ(Walk(owner, fields, 1, &found), ARNM_SUCCESS);
    EXPECT_EQ(w, 1) << "a table with nothing left open stops looking at all";
  }
}

TEST(JsonReader, AWalkStopsLookingOnceEveryEntryIsFilled) {
  // the members after the last one it wanted are never read, so a value that would be refused
  // costs nothing as long as it comes after
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"a\":\"not a number\"}"), ARNM_SUCCESS);
  int64_t a = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  uint64_t found = 0;
  EXPECT_EQ(Walk(owner, fields, 1, &found), ARNM_SUCCESS)
      << "the second a was never read, so its type was never asked";
  EXPECT_EQ(found, 0x1ull);
  EXPECT_EQ(a, 1);
}

TEST(JsonReader, AWalkSkipsWhatItDoesNotKnow) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"other\":1,\"wanted\":2,\"more\":3}"), ARNM_SUCCESS);
  int64_t wanted = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("wanted", &wanted)};
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 1, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x1ull);
  EXPECT_EQ(wanted, 2) << "a document may carry more than this reader wants";
}

TEST(JsonReader, AWalkNamesWhatWasThereAndNotWhatIsRequired) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"b\":2}"), ARNM_SUCCESS);
  int64_t a = -1, b = -1, c = -1;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b), ARNM_JSON_FIELD_INT64("c", &c)
  };
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 3, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x2ull) << "only the middle one was carried";
  EXPECT_EQ(a, -1) << "an absent member leaves its target alone";
  EXPECT_EQ(b, 2);
  EXPECT_EQ(c, -1);
}

TEST(JsonReader, AWalkStopsAtTheMemberItCannotReadAndSaysHowFarItCame) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1,\"b\":\"two\",\"c\":3}"), ARNM_SUCCESS);
  int64_t a = 0, b = 0, c = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("a", &a), ARNM_JSON_FIELD_INT64("b", &b), ARNM_JSON_FIELD_INT64("c", &c)
  };
  uint64_t found = 0;
  EXPECT_EQ(Walk(owner, fields, 3, &found), ARNM_ERROR_INVALID_ENUM_TYPE);
  EXPECT_EQ(found, 0x1ull) << "the first was read, the second refused, the third never reached";
  EXPECT_EQ(a, 1);
  EXPECT_EQ(c, 0);
}

TEST(JsonReader, AWalkCarriesANegativeNumberIntoAnInt32) {
  // the narrow signed types are the ones a range check is easiest to get backwards on, so both
  // ends of both of them are asked here
  ArenaReader owner;
  ASSERT_EQ(
      Parse(owner.reader(), "{\"low\":-2147483648,\"high\":2147483647,\"small\":-1}"), ARNM_SUCCESS
  );
  int32_t low = 0, high = 0, small = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT32("low", &low), ARNM_JSON_FIELD_INT32("high", &high),
      ARNM_JSON_FIELD_INT32("small", &small)
  };
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 3, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x7ull);
  EXPECT_EQ(low, INT32_MIN);
  EXPECT_EQ(high, INT32_MAX);
  EXPECT_EQ(small, -1);
}

TEST(JsonReader, AWalkCarriesTheWidestNumbersWhole) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(owner.reader(), "{\"u\":18446744073709551615,\"i\":-9223372036854775808}"), ARNM_SUCCESS
  );
  uint64_t u = 0;
  int64_t i = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_UINT64("u", &u), ARNM_JSON_FIELD_INT64("i", &i)};
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 2, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3ull);
  EXPECT_EQ(u, UINT64_MAX);
  EXPECT_EQ(i, INT64_MIN);
}

TEST(JsonReader, AWalkRefusesANumberThatWillNotFitItsTarget) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"n\":2147483648}"), ARNM_SUCCESS);
  int32_t narrow = 0;
  arnm_json_field over[] = {ARNM_JSON_FIELD_INT32("n", &narrow)};
  EXPECT_EQ(Walk(owner, over, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(narrow, 0) << "a refused member leaves its target alone";

  ASSERT_EQ(Parse(owner.reader(), "{\"n\":-1}"), ARNM_SUCCESS);
  uint32_t unsigned_narrow = 0;
  uint64_t unsigned_wide = 0xdeadbeefull;
  arnm_json_field negative_u32[] = {ARNM_JSON_FIELD_UINT32("n", &unsigned_narrow)};
  arnm_json_field negative_u64[] = {ARNM_JSON_FIELD_UINT64("n", &unsigned_wide)};
  EXPECT_EQ(Walk(owner, negative_u32, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(Walk(owner, negative_u64, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(unsigned_wide, 0xdeadbeefull);

  // 2^63 is one past what an int64_t holds, and carrying it across would wrap it negative
  ASSERT_EQ(Parse(owner.reader(), "{\"n\":9223372036854775808}"), ARNM_SUCCESS);
  int64_t wide = 0;
  arnm_json_field past_int64[] = {ARNM_JSON_FIELD_INT64("n", &wide)};
  EXPECT_EQ(Walk(owner, past_int64, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(wide, 0);

  ASSERT_EQ(Parse(owner.reader(), "{\"n\":4294967296}"), ARNM_SUCCESS);
  uint32_t past_u32 = 0;
  arnm_json_field over_u32[] = {ARNM_JSON_FIELD_UINT32("n", &past_u32)};
  EXPECT_EQ(Walk(owner, over_u32, 1, nullptr), ARNM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(JsonReader, AWalkRefusesAStringThatIsNotTheShapeItsEntryNames) {
  ArenaReader owner;
  ASSERT_EQ(
      Parse(owner.reader(), "{\"short\":\"01\",\"nothex\":\"zzzzzzzz\",\"empty\":\"\"}"),
      ARNM_SUCCESS
  );
  uint8_t four[4] = {0, 0, 0, 0};

  arnm_memory_block too_short = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field short_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("short", &too_short)};
  EXPECT_EQ(Walk(owner, short_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED)
      << "four bytes want eight characters";

  arnm_memory_block bad = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field not_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("nothex", &bad)};
  EXPECT_EQ(Walk(owner, not_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  arnm_memory_block empty_target = ARNM_JSON_BLOCK_OF(four);
  arnm_json_field empty_hex[] = {ARNM_JSON_FIELD_HEX_FIXED("empty", &empty_target)};
  EXPECT_EQ(Walk(owner, empty_hex, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  uint8_t uuid[ARNM_UUID_BINARY_SIZE] = {0};
  arnm_memory_block uuid_block = ARNM_JSON_BLOCK_OF(uuid);
  arnm_json_field not_a_uuid[] = {ARNM_JSON_FIELD_UUID("short", &uuid_block)};
  EXPECT_EQ(Walk(owner, not_a_uuid, 1, nullptr), ARNM_ERROR_DECODE_FAILED);

  // a uuid decodes into exactly ARNM_UUID_BINARY_SIZE bytes, so a block of any other size is
  // the caller's mistake rather than the document's
  ASSERT_EQ(
      Parse(owner.reader(), "{\"u\":\"019e2c31-a303-75c0-941e-f35c59e4f978\"}"), ARNM_SUCCESS
  );
  uint8_t too_few[8] = {0};
  arnm_memory_block wrong_size = ARNM_JSON_BLOCK_OF(too_few);
  arnm_json_field bad_uuid_target[] = {ARNM_JSON_FIELD_UUID("u", &wrong_size)};
  EXPECT_EQ(Walk(owner, bad_uuid_target, 1, nullptr), ARNM_ERROR_DECODE_FAILED);
}

TEST(JsonReader, AWalkRefusesAMemberOfAnotherJsonType) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"n\":true,\"s\":5,\"b\":\"yes\"}"), ARNM_SUCCESS);
  int64_t number = 0;
  arnm_json_field wrong_number[] = {ARNM_JSON_FIELD_INT64("n", &number)};
  EXPECT_EQ(Walk(owner, wrong_number, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);

  arnm_memory_block text{};
  arnm_json_field wrong_string[] = {ARNM_JSON_FIELD_STRING("s", &text)};
  EXPECT_EQ(Walk(owner, wrong_string, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);

  bool flag = false;
  arnm_json_field wrong_bool[] = {ARNM_JSON_FIELD_BOOL("b", &flag)};
  EXPECT_EQ(Walk(owner, wrong_bool, 1, nullptr), ARNM_ERROR_INVALID_ENUM_TYPE);
}

TEST(JsonReader, AWalkRefusesAnEntryThatCarriesNoType) {
  // there is no placeholder tag: a field nobody set is a mistake and is named as one, rather
  // than being stepped over where a caller believed it was being read
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1}"), ARNM_SUCCESS);
  int64_t a = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  fields[0].type = ARNM_JSON_FIELD_TYPE_NONE;
  uint64_t found = 0;
  EXPECT_EQ(Walk(owner, fields, 1, &found), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(found, 0ull);
  EXPECT_EQ(a, 0);
}

TEST(JsonReader, AWalkRefusesAMatchedEntryWithoutATarget) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"a\":1}"), ARNM_SUCCESS);
  int64_t a = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &a)};
  fields[0].target = nullptr;
  EXPECT_EQ(Walk(owner, fields, 1, nullptr), ARNM_ERROR_NULL_POINTER);
}

TEST(JsonReader, AWalkRefusesWhatIsNoObjectAndWhatIsNoTable) {
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "[1,2]"), ARNM_SUCCESS);
  int64_t value = 0;
  arnm_json_field fields[] = {ARNM_JSON_FIELD_INT64("a", &value)};
  arnm_json_value *root = arnm_json_reader_root(owner.reader());
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

TEST(JsonReader, AWalkFillsTheWidestTableItAccepts) {
  // the mask is a bit per entry, so the last entry of a full table is what says whether the
  // shift that sets it still fits the word, and whether valid_mask survives a count of 64
  ArenaReader owner;
  std::string json = "{";
  for (uint32_t index = 0; index < ARNM_JSON_FIELDS_MAX; ++index) {
    if (index) { json += ","; }
    json += "\"k" + std::to_string(index) + "\":" + std::to_string(index);
  }
  json += "}";
  ASSERT_EQ(Parse(owner.reader(), json), ARNM_SUCCESS);

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
  ASSERT_EQ(Walk(owner, fields.data(), ARNM_JSON_FIELDS_MAX, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, UINT64_MAX) << "every bit of the mask, the topmost one included";
  for (uint32_t index = 0; index < ARNM_JSON_FIELDS_MAX; ++index) {
    EXPECT_EQ(values[index], static_cast<int64_t>(index)) << "entry " << index;
  }
}

TEST(JsonReader, AWalkComparesKeysOverTheirLengthAndNotToATerminator) {
  // the key of an entry is compared over key_length, so a prefix of a longer key is not a match
  // and a key carrying a NUL is compared whole
  ArenaReader owner;
  ASSERT_EQ(Parse(owner.reader(), "{\"abc\":1,\"ab\":2}"), ARNM_SUCCESS);
  int64_t two = 0, three = 0;
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_INT64("ab", &two), ARNM_JSON_FIELD_INT64("abc", &three)
  };
  uint64_t found = 0;
  ASSERT_EQ(Walk(owner, fields, 2, &found), ARNM_SUCCESS);
  EXPECT_EQ(found, 0x3ull);
  EXPECT_EQ(two, 2);
  EXPECT_EQ(three, 1);
}
