#include "arnm/converter.h"
#include "arnm/memory_block.h"
#include "arnm/mono_timer.h"
#include <gtest/gtest.h>

#include "memory_limit.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

TEST(Converter, arnm_uint64_to_string) {
  char buffer[20];
  auto expectedSize = arnm_uint64_to_string(buffer, sizeof(buffer), 123456789);
  EXPECT_EQ(expectedSize, 9);
  EXPECT_STREQ(buffer, "123456789");
}

TEST(Converter, arnm_uint64_to_string_full) {
  char buffer[20];
  auto expectedSize = arnm_uint64_to_string(buffer, sizeof(buffer), 1234567890123456789);
  EXPECT_EQ(expectedSize, 19);
  EXPECT_STREQ(buffer, "1234567890123456789");
}

TEST(Converter, arnm_uint64_to_string_empty) {
  char buffer[20];
  auto expectedSize = arnm_uint64_to_string(buffer, sizeof(buffer), 0);
  EXPECT_EQ(expectedSize, 1);
  EXPECT_STREQ(buffer, "0");
}

TEST(Converter, arnm_uint64_to_string_too_small_buffer) {
  char buffer[1];
  auto expectedSize = arnm_uint64_to_string(buffer, sizeof(buffer), 123456789);
  EXPECT_EQ(expectedSize, 9);
  EXPECT_STREQ(buffer, "");
}

size_t arnm_uint64_to_string_size_old(uint64_t value) {
  static uint64_t powers[] = {
      10,
      100,
      1000,
      10000,
      100000,
      1000000,
      10000000,
      100000000,
      1000000000,
      10000000000,
      100000000000,
      1000000000000,
      10000000000000,
      100000000000000,
      1000000000000000,
      10000000000000000,
      100000000000000000,
      1000000000000000000,
      10000000000000000000u
  };
  int i = 0;
  while (i < 19 && value >= powers[i]) { ++i; }
  return i + 1;
}

TEST(Converter, arnm_uint64_to_string_size_validation) {
  auto ref = arnm_uint64_to_string_size_old;
  auto opt = arnm_uint64_to_string_size;

  // --- 1. Explicit Edge Cases ---
  uint64_t cases[] = {0ULL, 1ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, UINT64_MAX};

  for (uint64_t v : cases) { ASSERT_EQ(ref(v), opt(v)) << "Edge case failed: " << v; }

  // --- 2. Boundaries around powers of 10 ---
  uint64_t p = 1;
  for (int d = 1; d <= 19; d++) {
    uint64_t low = p;
    uint64_t high = p * 10 - 1;

    for (int i = -2; i <= 2; i++) {
      if (low + i > 0) { ASSERT_EQ(ref(low + i), opt(low + i)) << "Low boundary: " << (low + i); }

      ASSERT_EQ(ref(high + i), opt(high + i)) << "High boundary: " << (high + i);
    }

    p *= 10;
  }

  // --- 3. Bit-structure tests (logarithmic distribution) ---
  for (uint64_t v = 1; v != 0; v <<= 1) {
    ASSERT_EQ(ref(v), opt(v)) << "Power of 2: " << v;

    if (v > 0) ASSERT_EQ(ref(v - 1), opt(v - 1));
    if (v < UINT64_MAX) ASSERT_EQ(ref(v + 1), opt(v + 1));
  }

  // --- 4. Random values ---
  std::mt19937_64 rng(123456);

  for (size_t i = 0; i < 5'000'000; i++) {
    uint64_t v = rng();
    ASSERT_EQ(ref(v), opt(v));
  }
}

// INT64_MIN is the one value that cannot be negated in int64_t: `v * -1` is undefined there,
// and the result only looked right because two's complement wrapping happened to land on it.
TEST(ConverterInt64, HandlesInt64Min) {
  const std::string expected = std::to_string(INT64_MIN); // "-9223372036854775808"
  ASSERT_EQ(expected.size(), 20u);

  EXPECT_EQ(arnm_int64_to_string_size(INT64_MIN), expected.size());

  char buffer[32] = {};
  const size_t written = arnm_int64_to_string_known_string_size(buffer, INT64_MIN, expected.size());
  EXPECT_EQ(written, expected.size());
  EXPECT_STREQ(buffer, expected.c_str());

  // and the neighbours, so an off by one in the negation would not slip through
  EXPECT_EQ(arnm_int64_to_string_size(INT64_MIN + 1), 20u);
  EXPECT_EQ(arnm_int64_to_string_size(INT64_MAX), 19u);
  EXPECT_EQ(arnm_int64_to_string_size(-1), 2u);
  EXPECT_EQ(arnm_int64_to_string_size(0), 1u);
}

/* --- hex ---------------------------------------------------------------------------------- */

// The reference the fast conversion is checked against: one printf per byte, slow and obvious.
// Written here rather than borrowed from a crypto library, because arnm links none -- and a
// reference that shares no line of code with the thing it judges is the point either way.
static std::string reference_hex(const uint8_t *bytes, size_t count) {
  std::string out;
  char pair[3];
  for (size_t i = 0; i < count; ++i) {
    snprintf(pair, sizeof(pair), "%02x", static_cast<unsigned>(bytes[i]));
    out += pair;
  }
  return out;
}

// a missing pointer and an empty block are different mistakes and say so
TEST(HexTest, RejectsNullAndEmptySeparately) {
  uint8_t payload[4] = {1, 2, 3, 4};
  char out[16];
  arnm_memory_block data{payload, sizeof(payload)};
  arnm_memory_block empty{payload, 0};

  EXPECT_EQ(arnm_binary_to_hex(nullptr, &data), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_binary_to_hex(out, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_binary_to_hex(out, &empty), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_binary_to_hex(out, &data), ARNM_SUCCESS);

  arnm_memory_block no_data{nullptr, 4};
  EXPECT_EQ(arnm_binary_to_hex(out, &no_data), ARNM_ERROR_NULL_POINTER);
}

// promise: the computed digits agree with the printf reference, for every byte value and every
// length from one byte up. The lengths cover the loop's edges -- an optimised build converts the
// bulk a vector register at a time and the remainder one byte at a time, so a mistake in either
// half shows only at the lengths that reach it. Every byte value covers the arithmetic that
// picks the digit, whose two ranges meet between '9' and 'a'.
TEST(HexTest, MatchesTheReferenceForEveryByteValueAndLength) {
  for (unsigned value = 0; value < 256; ++value) {
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); ++i) {
      payload[i] = static_cast<uint8_t>((value + i * 7u) & 0xFFu);
    }
    payload[0] = static_cast<uint8_t>(value);

    for (size_t length = 1; length <= sizeof(payload); ++length) {
      arnm_memory_block block{payload, static_cast<uint32_t>(length)};

      char ours[sizeof(payload) * 2 + 1];
      ASSERT_EQ(arnm_binary_to_hex(ours, &block), ARNM_SUCCESS);
      ASSERT_EQ(std::string(ours), reference_hex(payload, length))
          << "value " << value << " length " << length;
      ASSERT_EQ(strlen(ours), length * 2) << "value " << value << " length " << length;

      uint8_t decoded[sizeof(payload)];
      ASSERT_EQ(arnm_binary_from_hex(decoded, ours), ARNM_SUCCESS);
      ASSERT_EQ(memcmp(decoded, payload, length), 0) << "value " << value << " length " << length;
    }
  }
}

// promise: the terminator lands right after the digits and nothing is written past it
TEST(HexTest, WritesTheTerminatorAndNothingBeyondIt) {
  uint8_t payload[7] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x7f, 0x80};
  arnm_memory_block block{payload, sizeof(payload)};

  char guarded[sizeof(payload) * 2 + 1 + 8];
  memset(guarded, 0x7A, sizeof(guarded));

  ASSERT_EQ(arnm_binary_to_hex(guarded, &block), ARNM_SUCCESS);
  EXPECT_STREQ(guarded, "deadbeef007f80");
  EXPECT_EQ(guarded[sizeof(payload) * 2], '\0');
  for (size_t i = sizeof(payload) * 2 + 1; i < sizeof(guarded); ++i) {
    EXPECT_EQ(guarded[i], 0x7A) << "wrote past the terminator, at " << i;
  }
}

// promise: upper case digits decode to the same bytes, and an empty string is a conversion of
// nothing rather than an error
TEST(HexTest, AcceptsBothDigitCasesAndTheEmptyString) {
  uint8_t payload[8] = {0x00, 0x0f, 0xa5, 0xff, 0x10, 0xde, 0xad, 0xbe};
  arnm_memory_block block{payload, sizeof(payload)};

  char lower[sizeof(payload) * 2 + 1];
  ASSERT_EQ(arnm_binary_to_hex(lower, &block), ARNM_SUCCESS);
  for (const char *c = lower; *c; ++c) { ASSERT_FALSE(isupper(static_cast<unsigned char>(*c))); }

  std::string upper(lower);
  for (char &c : upper) { c = static_cast<char>(toupper(static_cast<unsigned char>(c))); }

  uint8_t from_lower[sizeof(payload)];
  uint8_t from_upper[sizeof(payload)];
  ASSERT_EQ(arnm_binary_from_hex(from_lower, lower), ARNM_SUCCESS);
  ASSERT_EQ(arnm_binary_from_hex(from_upper, upper.c_str()), ARNM_SUCCESS);
  EXPECT_EQ(memcmp(from_lower, payload, sizeof(payload)), 0);
  EXPECT_EQ(memcmp(from_upper, payload, sizeof(payload)), 0);

  // mixed case within one byte, which is where a single range check would fall over
  uint8_t mixed[2];
  ASSERT_EQ(arnm_binary_from_hex(mixed, "aBcD"), ARNM_SUCCESS);
  EXPECT_EQ(mixed[0], 0xab);
  EXPECT_EQ(mixed[1], 0xcd);

  uint8_t untouched[4];
  memset(untouched, 0x77, sizeof(untouched));
  EXPECT_EQ(arnm_binary_from_hex(untouched, ""), ARNM_SUCCESS);
  for (unsigned char byte : untouched) { EXPECT_EQ(byte, 0x77); }
}

// promise: anything that is not an even run of hex digits is refused, the output is cleared
// rather than left half converted, and nothing is written past the bytes the string accounts
// for. Separators are the case worth naming: some decoders can be told to skip them, this one
// cannot, and a caller expecting that should meet a refusal rather than a guess.
TEST(HexTest, RejectsWhatIsNotHexWithoutOverrunning) {
  struct {
    const char *what;
    const char *input;
    arnm_result expected;
  } const cases[] = {
      {"odd number of digits", "abc", ARNM_ERROR_INVALID_PARAM},
      {"a single digit", "a", ARNM_ERROR_INVALID_PARAM},
      {"not a digit, first position", "zz00", ARNM_ERROR_DECODE_FAILED},
      {"not a digit, low nibble", "azcd", ARNM_ERROR_DECODE_FAILED},
      {"not a digit, last position", "00ffz0", ARNM_ERROR_DECODE_FAILED},
      {"separator between the bytes", "de:ad", ARNM_ERROR_INVALID_PARAM},
      {"separators, even length", "de:ad:be", ARNM_ERROR_DECODE_FAILED},
      {"a space", "de ad", ARNM_ERROR_INVALID_PARAM},
      {"the character right below '0'", "//00", ARNM_ERROR_DECODE_FAILED},
      {"the character right above '9'", "::00", ARNM_ERROR_DECODE_FAILED},
      {"the character right above 'f'", "gg00", ARNM_ERROR_DECODE_FAILED},
      {"the character right above 'F'", "GG00", ARNM_ERROR_DECODE_FAILED},
      // spelled in two literals: a hex escape swallows every hex digit that follows it, so
      // "\xa400" would be one character with a value nobody meant
      {"a byte above 0x7F",
       "\xc3\xa4"
       "00",
       ARNM_ERROR_DECODE_FAILED},
  };

  for (const auto &c : cases) {
    uint8_t guarded[16];
    memset(guarded, 0xCD, sizeof(guarded));

    EXPECT_EQ(arnm_binary_from_hex(guarded, c.input), c.expected) << c.what;

    const size_t decoded_bytes = strlen(c.input) / 2;
    for (size_t i = decoded_bytes; i < sizeof(guarded); ++i) {
      EXPECT_EQ(guarded[i], 0xCD) << c.what << ": wrote past byte " << decoded_bytes;
    }
    if (c.expected == ARNM_ERROR_DECODE_FAILED) {
      for (size_t i = 0; i < decoded_bytes; ++i) {
        EXPECT_EQ(guarded[i], 0) << c.what << ": left a half converted byte at " << i;
      }
    } else {
      // A parameter error is refused before anything is written, so the buffer still holds what
      // the caller put there. Clearing it would erase bytes this call never produced. Wiping a
      // buffer that has served its purpose stays with whoever owns it.
      for (size_t i = 0; i < decoded_bytes; ++i) {
        EXPECT_EQ(guarded[i], 0xCD) << c.what << ": touched a buffer it had refused, at " << i;
      }
    }
  }

  uint8_t out[4];
  EXPECT_EQ(arnm_binary_from_hex(nullptr, "dead"), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_binary_from_hex(out, nullptr), ARNM_ERROR_NULL_POINTER);
}

// promise: every one of the 256 characters is judged the same whether it stands in the high or
// the low nibble, and only the 22 hex digits are let through. The validity test is arithmetic
// rather than a table, so a wrong constant would open a whole range at once.
TEST(HexTest, AcceptsExactlyTheHexDigitsInBothNibbles) {
  for (unsigned c = 1; c < 256; ++c) { // 0 would end the string instead of being read
    const bool is_digit =
        (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');

    char high_bad[3] = {static_cast<char>(c), '0', '\0'};
    char low_bad[3] = {'0', static_cast<char>(c), '\0'};
    uint8_t out[1];

    EXPECT_EQ(
        arnm_binary_from_hex(out, high_bad), is_digit ? ARNM_SUCCESS : ARNM_ERROR_DECODE_FAILED
    ) << "character "
      << c << " in the high nibble";
    EXPECT_EQ(
        arnm_binary_from_hex(out, low_bad), is_digit ? ARNM_SUCCESS : ARNM_ERROR_DECODE_FAILED
    ) << "character "
      << c << " in the low nibble";
  }
}

/* --- uuid --------------------------------------------------------------------------------- */

TEST(UuidTest, RoundtripValidUuid) {
  const uint8_t original[ARNM_UUID_BINARY_SIZE] = {0x48, 0x06, 0x6a, 0x47, 0xa0, 0x2f, 0x45, 0x96,
                                                   0x88, 0x3c, 0x30, 0x2c, 0x2b, 0x1a, 0xa1, 0xe1};
  const char expected[] = "48066a47-a02f-4596-883c-302c2b1aa1e1";

  char uuid_string[ARNM_UUID_STRING_LENGTH + 1];
  arnm_uuid_to_string(uuid_string, original);
  EXPECT_STREQ(uuid_string, expected);
  EXPECT_EQ(strlen(uuid_string), ARNM_UUID_STRING_LENGTH);

  uint8_t decoded[ARNM_UUID_BINARY_SIZE];
  EXPECT_EQ(arnm_uuid_from_string(decoded, uuid_string), ARNM_SUCCESS);
  EXPECT_EQ(memcmp(original, decoded, sizeof(original)), 0);
}

// promise: a length that is wrong is caught before the parser writes anything, so the buffer is
// left as the caller had it. Only ARNM_ERROR_DECODE_FAILED clears it -- that is the path where
// the parser has already put something there, or is about to.
TEST(UuidTest, AWrongLengthLeavesTheBufferAlone) {
  const char *const cases[] = {
      "",
      "too-short",
      "48066a47-a02f-4596-883c-302c2b1aa1e",
      "48066a47-a02f-4596-883c-302c2b1aa1e1-extra",
  };

  for (const char *input : cases) {
    uint8_t guarded[ARNM_UUID_BINARY_SIZE];
    memset(guarded, 0xCD, sizeof(guarded));

    EXPECT_EQ(arnm_uuid_from_string(guarded, input), ARNM_ERROR_INVALID_PARAM) << input;
    for (unsigned char byte : guarded) {
      EXPECT_EQ(byte, 0xCD) << input << ": touched a buffer it had refused";
    }
  }

  // and the other side of the rule: a string of the right length that does not parse is cleared
  uint8_t cleared[ARNM_UUID_BINARY_SIZE];
  memset(cleared, 0xCD, sizeof(cleared));
  EXPECT_EQ(
      arnm_uuid_from_string(cleared, "XXXX6a47-a02f-4596-883c-302c2b1aa1e1"),
      ARNM_ERROR_DECODE_FAILED
  );
  for (unsigned char byte : cleared) { EXPECT_EQ(byte, 0); }
}

TEST(UuidTest, RejectsNullAndWrongLength) {
  uint8_t uuid[ARNM_UUID_BINARY_SIZE];

  EXPECT_EQ(
      arnm_uuid_from_string(nullptr, "48066a47-a02f-4596-883c-302c2b1aa1e1"),
      ARNM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(arnm_uuid_from_string(uuid, nullptr), ARNM_ERROR_NULL_POINTER);

  // a length that is not 36 is a parameter the caller can fix, not an undecodable string
  EXPECT_EQ(arnm_uuid_from_string(uuid, ""), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_uuid_from_string(uuid, "too-short"), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(
      arnm_uuid_from_string(uuid, "48066a47-a02f-4596-883c-302c2b1aa1e"), ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      arnm_uuid_from_string(uuid, "48066a47-a02f-4596-883c-302c2b1aa1e1-extra"),
      ARNM_ERROR_INVALID_PARAM
  );

  EXPECT_EQ(
      arnm_uuid_from_string(uuid, "XXXX6a47-a02f-4596-883c-302c2b1aa1e1"), ARNM_ERROR_DECODE_FAILED
  );
}

// promise: a string of the documented length whose separators are missing or sit elsewhere is
// rejected. Accepting it would be worse than a wrong answer -- every absent separator turns two
// more characters into an output byte, and the first case below would write 18 bytes into the 16
// the caller owns. The guard bytes here catch that without a sanitizer.
TEST(UuidTest, SeparatorsMustSitWhereTheFormatSaysAndNeverOverrun) {
  struct {
    const char *what;
    const char *input;
  } const cases[] = {
      {"no separators at all", "48066a47a02f4596883c302c2b1aa1e1abcd"},
      {"two separators short", "48066a47a02f4596-883c-302c2b1aa1e1ab"},
      {"separators only", "------------------------------------"},
      {"first separator one group early", "4806-6a47a02f-4596-883c-302c2b1aa1e1"},
      {"a digit where the last separator belongs", "48066a47-a02f-4596-883cf302c2b1aa1e1"},
  };

  for (const auto &c : cases) {
    uint8_t guarded[ARNM_UUID_BINARY_SIZE * 2];
    memset(guarded, 0xCD, sizeof(guarded));

    EXPECT_EQ(arnm_uuid_from_string(guarded, c.input), ARNM_ERROR_DECODE_FAILED) << c.what;

    for (size_t i = ARNM_UUID_BINARY_SIZE; i < sizeof(guarded); ++i) {
      EXPECT_EQ(guarded[i], 0xCD) << c.what << ": wrote past the 16 bytes it was given, at " << i;
    }
    // a rejected string leaves no half decoded bytes behind
    for (size_t i = 0; i < ARNM_UUID_BINARY_SIZE; ++i) { EXPECT_EQ(guarded[i], 0) << c.what; }
  }
}

// promise: every byte value survives the round trip, at every one of the 16 positions, in both
// digit cases -- the lookup tables both directions use are indexed by the data itself, so a
// wrong entry would show up only for the values that reach it.
TEST(UuidTest, RoundTripCoversEveryByteValueAndBothCases) {
  for (unsigned value = 0; value < 256; ++value) {
    for (size_t position = 0; position < ARNM_UUID_BINARY_SIZE; ++position) {
      uint8_t original[ARNM_UUID_BINARY_SIZE];
      memset(original, 0x5A, sizeof(original));
      original[position] = static_cast<uint8_t>(value);

      char text[ARNM_UUID_STRING_LENGTH + 1];
      arnm_uuid_to_string(text, original);
      ASSERT_EQ(strlen(text), static_cast<size_t>(ARNM_UUID_STRING_LENGTH));
      ASSERT_EQ(text[8], '-');
      ASSERT_EQ(text[13], '-');
      ASSERT_EQ(text[18], '-');
      ASSERT_EQ(text[23], '-');

      uint8_t decoded[ARNM_UUID_BINARY_SIZE];
      ASSERT_EQ(arnm_uuid_from_string(decoded, text), ARNM_SUCCESS) << text;
      ASSERT_EQ(memcmp(original, decoded, sizeof(original)), 0) << text;

      char upper[ARNM_UUID_STRING_LENGTH + 1];
      memcpy(upper, text, sizeof(upper));
      for (size_t i = 0; i < ARNM_UUID_STRING_LENGTH; ++i) {
        upper[i] = static_cast<char>(toupper(static_cast<unsigned char>(upper[i])));
      }
      uint8_t decoded_upper[ARNM_UUID_BINARY_SIZE];
      ASSERT_EQ(arnm_uuid_from_string(decoded_upper, upper), ARNM_SUCCESS) << upper;
      ASSERT_EQ(memcmp(original, decoded_upper, sizeof(original)), 0) << upper;
    }
  }
}

// promise: the 32 hex characters agree with what the block conversion writes for the same bytes,
// separators removed. Two implementations of the same alphabet, one computed and one from a
// table -- if they ever drifted apart, a uuid would read differently depending on which one
// rendered it.
TEST(UuidTest, TheSameBytesReadTheSameThroughBothConversions) {
  for (unsigned seed = 0; seed < 64; ++seed) {
    uint8_t bytes[ARNM_UUID_BINARY_SIZE];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      bytes[i] = static_cast<uint8_t>((seed * 37u + i * 13u) & 0xFFu);
    }

    char uuid_form[ARNM_UUID_STRING_LENGTH + 1];
    arnm_uuid_to_string(uuid_form, bytes);

    arnm_memory_block block{bytes, sizeof(bytes)};
    char hex_form[sizeof(bytes) * 2 + 1];
    ASSERT_EQ(arnm_binary_to_hex(hex_form, &block), ARNM_SUCCESS);

    std::string without_separators;
    for (size_t i = 0; i < ARNM_UUID_STRING_LENGTH; ++i) {
      if (uuid_form[i] != '-') { without_separators += uuid_form[i]; }
    }
    EXPECT_EQ(without_separators, std::string(hex_form)) << "seed " << seed;
  }
}

// promise: writing stops at the terminator, and the terminator sits at index 36
TEST(UuidTest, WritesExactlyThirtySevenBytes) {
  const uint8_t bytes[ARNM_UUID_BINARY_SIZE] = {0xff, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                                0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  char guarded[ARNM_UUID_STRING_LENGTH + 1 + 8];
  memset(guarded, 0x7A, sizeof(guarded));

  arnm_uuid_to_string(guarded, bytes);
  EXPECT_STREQ(guarded, "ff001122-3344-5566-7788-99aabbccddee");
  EXPECT_EQ(guarded[ARNM_UUID_STRING_LENGTH], '\0');
  for (size_t i = ARNM_UUID_STRING_LENGTH + 1; i < sizeof(guarded); ++i) {
    EXPECT_EQ(guarded[i], 0x7A) << "wrote past the terminator, at " << i;
  }
}

TEST(UuidTest, AllZerosAndKnownStrings) {
  const uint8_t zeros[ARNM_UUID_BINARY_SIZE] = {0};
  char uuid_string[ARNM_UUID_STRING_LENGTH + 1];
  arnm_uuid_to_string(uuid_string, zeros);
  EXPECT_STREQ(uuid_string, "00000000-0000-0000-0000-000000000000");

  uint8_t decoded[ARNM_UUID_BINARY_SIZE];
  EXPECT_EQ(arnm_uuid_from_string(decoded, uuid_string), ARNM_SUCCESS);
  EXPECT_EQ(memcmp(zeros, decoded, sizeof(zeros)), 0);

  const char *known[] = {
      "123e4567-e89b-12d3-a456-426614174000",
      "00000000-0000-0000-0000-000000000000",
      "ffffffff-ffff-ffff-ffff-ffffffffffff",
      "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  };

  for (const char *str : known) {
    uint8_t bytes[ARNM_UUID_BINARY_SIZE];
    EXPECT_EQ(arnm_uuid_from_string(bytes, str), ARNM_SUCCESS) << str;

    char encoded[ARNM_UUID_STRING_LENGTH + 1];
    arnm_uuid_to_string(encoded, bytes);
    EXPECT_STREQ(encoded, str);
  }
}

// promise: every one of the 256 characters is judged the same at every hex position, and only
// the 22 hex digits are let through. The sentinel table is what settles that, and a wrong entry
// reaches only the string that carries exactly that character.
TEST(UuidTest, AcceptsExactlyTheHexDigitsAtEveryPosition) {
  const char valid[] = "48066a47-a02f-4596-883c-302c2b1aa1e1";

  for (size_t position = 0; position < ARNM_UUID_STRING_LENGTH; ++position) {
    const bool is_separator = position == 8 || position == 13 || position == 18 || position == 23;
    if (is_separator) { continue; }

    for (unsigned c = 1; c < 256; ++c) { // 0 would shorten the string instead of being read
      const bool is_digit =
          (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');

      char text[ARNM_UUID_STRING_LENGTH + 1];
      memcpy(text, valid, sizeof(text));
      text[position] = static_cast<char>(c);

      uint8_t decoded[ARNM_UUID_BINARY_SIZE];
      ASSERT_EQ(
          arnm_uuid_from_string(decoded, text), is_digit ? ARNM_SUCCESS : ARNM_ERROR_DECODE_FAILED
      ) << "character "
        << c << " at position " << position;
    }
  }
}

TEST(Converter, arnm_uint64_to_string_twenty_digits) {
  // Everything at or above 10^19 is twenty digits wide, and the conversion fills its buffer
  // from the back: a length one short does not shorten the number, it drops its first digit and
  // says nothing. The whole top of the uint64_t range lives here.
  struct {
    uint64_t value;
    const char *text;
  } cases[] = {
      {9999999999999999999ULL, "9999999999999999999"},
      {10000000000000000000ULL, "10000000000000000000"},
      {12345678901234567890ULL, "12345678901234567890"},
      {UINT64_MAX, "18446744073709551615"},
  };

  for (auto &one : cases) {
    char buffer[32];
    std::memset(buffer, '?', sizeof(buffer));
    const uint8_t reported = arnm_uint64_to_string_size(one.value);
    const uint8_t written = arnm_uint64_to_string(buffer, sizeof(buffer), one.value);

    EXPECT_STREQ(buffer, one.text);
    EXPECT_EQ(written, std::strlen(one.text));
    EXPECT_EQ(reported, written) << "the size answered ahead has to be the size written";
  }
}

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

namespace {

std::string Base64Of(const std::string &input) {
  std::string out(ARNM_BASE64_STRING_LENGTH(input.size()) + 1u, '\0');
  const arnm_memory_block block{
      reinterpret_cast<uint8_t *>(const_cast<char *>(input.data())), (uint32_t)input.size()
  };
  EXPECT_EQ(arnm_binary_to_base64(out.data(), &block), ARNM_SUCCESS);
  out.resize(std::strlen(out.c_str()));
  return out;
}

} // namespace

TEST(Base64, TheVectorsFromRfc4648) {
  // the ones the RFC prints, which pin all three padding cases and the alphabet's order
  EXPECT_EQ(Base64Of("f"), "Zg==");
  EXPECT_EQ(Base64Of("fo"), "Zm8=");
  EXPECT_EQ(Base64Of("foo"), "Zm9v");
  EXPECT_EQ(Base64Of("foob"), "Zm9vYg==");
  EXPECT_EQ(Base64Of("fooba"), "Zm9vYmE=");
  EXPECT_EQ(Base64Of("foobar"), "Zm9vYmFy");
}

TEST(Base64, EverySixBitValueMapsToItsCharacterOfTheStandardAlphabet) {
  // The encoder computes the character instead of reading a table, which is four compares
  // walking an offset across five runs. Every one of the 64 values is checked here, because a
  // boundary that is off by one is a character that is wrong and nothing else notices.
  static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  for (uint32_t value = 0; value < 64u; ++value) {
    // three bytes whose first six bits are the value under test, so it lands in character 0
    const uint8_t bytes[3] = {(uint8_t)(value << 2u), 0, 0};
    const arnm_memory_block block{const_cast<uint8_t *>(bytes), sizeof(bytes)};
    char text[5];
    ASSERT_EQ(arnm_binary_to_base64(text, &block), ARNM_SUCCESS);
    EXPECT_EQ(text[0], alphabet[value]) << "six bit value " << value;
  }
}

TEST(Base64, BothCharactersOutsideTheAlphanumericRunAreWritten) {
  // 0xFB 0xFF encodes the two groups that reach '+' and '/', which a table can get wrong
  const uint8_t bytes[] = {0xfb, 0xff, 0xbf};
  const arnm_memory_block block{const_cast<uint8_t *>(bytes), sizeof(bytes)};
  char text[ARNM_BASE64_STRING_LENGTH(sizeof(bytes)) + 1u];
  ASSERT_EQ(arnm_binary_to_base64(text, &block), ARNM_SUCCESS);
  EXPECT_STREQ(text, "+/+/");
}

TEST(Base64, EveryLengthSurvivesTheRoundTrip) {
  // every remainder class, several times over, with bytes that use the whole range
  for (uint32_t size = 1; size <= 300; ++size) {
    std::vector<uint8_t> bytes(size);
    for (uint32_t i = 0; i < size; ++i) { bytes[i] = (uint8_t)((i * 37u + size) & 0xFFu); }

    std::string text(ARNM_BASE64_STRING_LENGTH(size) + 1u, '\0');
    const arnm_memory_block block{bytes.data(), size};
    ASSERT_EQ(arnm_binary_to_base64(text.data(), &block), ARNM_SUCCESS) << "size " << size;
    ASSERT_EQ(std::strlen(text.c_str()), ARNM_BASE64_STRING_LENGTH(size))
        << "the length macro has to be exact, a writer counts a field with it";

    std::vector<uint8_t> back(ARNM_BASE64_BINARY_SIZE(std::strlen(text.c_str())));
    uint32_t written = 0;
    ASSERT_EQ(arnm_binary_from_base64(back.data(), &written, text.c_str()), ARNM_SUCCESS);
    ASSERT_EQ(written, size);
    EXPECT_EQ(0, std::memcmp(back.data(), bytes.data(), size)) << "size " << size;
  }
}

TEST(Base64, NoBytesIsRefusedAndAnEmptyStringDecodesToNothing) {
  const uint8_t byte = 0;
  const arnm_memory_block empty{const_cast<uint8_t *>(&byte), 0};
  char text[8];
  EXPECT_EQ(arnm_binary_to_base64(text, &empty), ARNM_ERROR_INVALID_PARAM);

  uint8_t out[4] = {1, 2, 3, 4};
  uint32_t written = 99;
  EXPECT_EQ(arnm_binary_from_base64(out, &written, ""), ARNM_SUCCESS);
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(out[0], 1u) << "nothing to write means nothing written";
}

TEST(Base64, ALengthThatIsNotAMultipleOfFourIsRefusedBeforeAnythingIsWritten) {
  uint8_t out[8] = {7, 7, 7, 7, 7, 7, 7, 7};
  uint32_t written = 0;
  EXPECT_EQ(arnm_binary_from_base64(out, &written, "Zm9vY"), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(out[0], 7u) << "refused before anything is written, so the buffer is untouched";
}

TEST(Base64, AForeignCharacterIsRefusedAndTheOutputZeroed) {
  // '-' and '_' are the URL safe alphabet, which this pair deliberately does not read
  // all eight characters long, so it is the alphabet that refuses them and not the length
  for (const char *bad : {"Zm9v!mFy", "Zm9vYm-y", "Zm9vYm_y", "Zm9v Zm9", "Zm9v\nYmF"}) {
    uint8_t out[6] = {9, 9, 9, 9, 9, 9};
    uint32_t written = 0;
    EXPECT_EQ(arnm_binary_from_base64(out, &written, bad), ARNM_ERROR_DECODE_FAILED) << bad;
    EXPECT_EQ(out[0], 0u) << "a caller who overlooks the code must not read half a decode";
  }
}

TEST(Base64, WhitespaceIsNotSkippedEvenWhereTheLengthWouldAllowIt) {
  // a wrapped base64 blob is a common thing to be handed; this pair refuses it rather than
  // quietly reading past the newline, which is the one behaviour a caller cannot detect
  uint8_t out[6] = {9, 9, 9, 9, 9, 9};
  uint32_t written = 0;
  EXPECT_EQ(arnm_binary_from_base64(out, &written, "Zm9v\nYmFyZm9v"), ARNM_ERROR_INVALID_PARAM);
}

TEST(Base64, PaddingIsOnlyAllowedWhereItBelongs) {
  for (const char *bad : {"Zm=vYmFy", "Zm9=YmFy", "=m9vYmFy", "Zm9vY==y", "Z===Zm9v"}) {
    uint8_t out[6] = {9, 9, 9, 9, 9, 9};
    uint32_t written = 0;
    EXPECT_EQ(arnm_binary_from_base64(out, &written, bad), ARNM_ERROR_DECODE_FAILED) << bad;
  }
  // and where it does belong it is read
  uint8_t out[4] = {0};
  uint32_t written = 0;
  EXPECT_EQ(arnm_binary_from_base64(out, &written, "Zg=="), ARNM_SUCCESS);
  EXPECT_EQ(written, 1u);
  EXPECT_EQ(out[0], 'f');
}
