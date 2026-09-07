#include "arnm/utf8.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

// The sources here are ASCII, so every byte above 0x7F is written as an escape. What is checked
// is the shape of a sequence, and the cases that matter are the ones that decode to something
// and are still not UTF-8: an overlong form, a surrogate half, a code point past the end of
// Unicode. A validator that only assembles code points accepts all three.

namespace {

/**
 * The reference, written the other way round on purpose: this one assembles the code point and
 * then judges it, where the implementation judges the bytes and never assembles anything. Two
 * formulations that share no line agreeing on every input is the point of having it.
 */
size_t ReferenceValidLength(const char *text, size_t length) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(text);
  static const uint32_t smallest_for[5] = {0, 0, 0x80u, 0x800u, 0x10000u};
  size_t i = 0;
  while (i < length) {
    const uint8_t lead = b[i];
    size_t len = 0;
    uint32_t cp = 0;
    if (lead < 0x80u) {
      len = 1;
      cp = lead;
    } else if ((lead & 0xE0u) == 0xC0u) {
      len = 2;
      cp = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
      len = 3;
      cp = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
      len = 4;
      cp = lead & 0x07u;
    } else {
      return i; // a continuation byte with nothing to continue, or 0xF8 and up
    }
    if (length - i < len) { return i; }
    for (size_t k = 1; k < len; ++k) {
      if ((b[i + k] & 0xC0u) != 0x80u) { return i; }
      cp = (cp << 6) | (b[i + k] & 0x3Fu);
    }
    if (cp < smallest_for[len]) { return i; }         // written longer than it needed to be
    if (cp > 0x10FFFFu) { return i; }                 // past the end of Unicode
    if (cp >= 0xD800u && cp <= 0xDFFFu) { return i; } // half of a surrogate pair
    i += len;
  }
  return length;
}

/** Both answers about one run, checked against the reference and against each other. */
void ExpectAgrees(const std::string &bytes, const char *what) {
  const size_t expected = ReferenceValidLength(bytes.data(), bytes.size());
  const size_t answered = arnm_utf8_valid_length(bytes.data(), bytes.size());
  ASSERT_EQ(answered, expected) << what;
  EXPECT_EQ(arnm_utf8_is_valid(bytes.data(), bytes.size()), expected == bytes.size()) << what;
}

} // namespace

// promise: well formed text of every sequence length passes, whole and in pieces
TEST(Utf8, AcceptsEverySequenceLength) {
  const std::string ascii = "plain ascii, digits 0123456789, punctuation !?-_";
  const std::string two = "\xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f";       // aeoeuess
  const std::string three = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"; // three CJK characters
  const std::string four = "\xf0\x9f\x8e\x89\xf0\x9f\x92\xa1";      // two above the BMP

  for (const std::string *one : {&ascii, &two, &three, &four}) {
    EXPECT_TRUE(arnm_utf8_is_valid(one->data(), one->size()));
    EXPECT_EQ(arnm_utf8_valid_length(one->data(), one->size()), one->size());
  }

  const std::string mixed = ascii + two + ascii + three + four + ascii;
  EXPECT_TRUE(arnm_utf8_is_valid(mixed.data(), mixed.size()));
  ExpectAgrees(mixed, "the four kinds interleaved");

  // the edges of each range, which is where a bound that is off by one shows
  const std::string edges = std::string("\x7f", 1) + "\xc2\x80" + "\xdf\xbf" + "\xe0\xa0\x80" +
                            "\xef\xbf\xbf" + "\xf0\x90\x80\x80" + "\xf4\x8f\xbf\xbf";
  EXPECT_TRUE(arnm_utf8_is_valid(edges.data(), edges.size()));
  ExpectAgrees(edges, "the first and last of every length");
}

// promise: a NUL is a byte like any other here -- UTF-8 says nothing about where a string ends,
// and this call is given a length rather than looking for one
TEST(Utf8, ANulByteIsContentAndNotAnEnd) {
  const std::string embedded = std::string("before\0after", 12) + "\xc3\xa4";
  ASSERT_EQ(embedded.size(), 14u);
  EXPECT_TRUE(arnm_utf8_is_valid(embedded.data(), embedded.size()));
  EXPECT_EQ(arnm_utf8_valid_length(embedded.data(), embedded.size()), 14u);
}

// promise: what the standard rejects is rejected, at the offset where it starts -- including
// the three kinds that decode perfectly well and are still not UTF-8
TEST(Utf8, RefusesWhatDecodesButIsNotUtf8) {
  struct Case {
    const char *bytes;
    size_t length;
    size_t valid_prefix;
    const char *what;
  };
  const Case cases[] = {
      {"ab\xc0\x80", 4, 2, "overlong NUL: two bytes for a code point that needs one"},
      {"ab\xc1\xbf", 4, 2, "overlong, the other lead the standard forbids outright"},
      {"ab\xe0\x80\x80", 5, 2, "overlong three byte form"},
      {"ab\xf0\x80\x80\x80", 6, 2, "overlong four byte form"},
      {"ab\xed\xa0\x80", 5, 2, "U+D800, the first half of a surrogate pair"},
      {"ab\xed\xbf\xbf", 5, 2, "U+DFFF, the last of them"},
      {"ab\xf4\x90\x80\x80", 6, 2, "U+110000, one past the end of Unicode"},
      {"ab\xf5\x80\x80\x80", 6, 2, "a lead byte no code point can have"},
      {"ab\xff\xfe", 4, 2, "0xFF and 0xFE are not UTF-8 at all"},
      {"ab\x80\x80", 4, 2, "a continuation byte with nothing in front of it"},
      {"ab\xc3", 3, 2, "a two byte sequence the run ends inside"},
      {"ab\xe6\x97", 4, 2, "a three byte sequence cut short"},
      {"ab\xf0\x9f\x8e", 5, 2, "a four byte sequence cut short"},
      {"ab\xc3\x28", 4, 2, "a continuation byte that is not one"},
      {"ab\xe6\x97\x28", 5, 2, "the second continuation is not one"},
  };

  for (const Case &one : cases) {
    const std::string bytes(one.bytes, one.length);
    EXPECT_EQ(arnm_utf8_valid_length(bytes.data(), bytes.size()), one.valid_prefix) << one.what;
    EXPECT_FALSE(arnm_utf8_is_valid(bytes.data(), bytes.size())) << one.what;
    ExpectAgrees(bytes, one.what);
  }
}

// promise: the empty run and the absent one are answered rather than walked
TEST(Utf8, AnswersTheEmptyAndTheAbsentRun) {
  EXPECT_TRUE(arnm_utf8_is_valid("", 0));
  EXPECT_EQ(arnm_utf8_valid_length("anything", 0), 0u);

  EXPECT_TRUE(arnm_utf8_is_valid(nullptr, 0)) << "nothing to walk is nothing wrong";
  EXPECT_EQ(arnm_utf8_valid_length(nullptr, 0), 0u);
  EXPECT_FALSE(arnm_utf8_is_valid(nullptr, 5)) << "and a length without bytes is not read";
  EXPECT_EQ(arnm_utf8_valid_length(nullptr, 5), 0u);
}

// promise: the eight byte ASCII stride answers the same as walking one byte at a time, at every
// offset a run can start a sequence at -- which is where a stride that overshoots would show
TEST(Utf8, TheAsciiStrideAgreesAtEveryAlignment) {
  const std::string bad = "\xed\xa0\x80"; // a surrogate, so the answer is never the whole run
  for (size_t lead_in = 0; lead_in < 40; ++lead_in) {
    const std::string bytes = std::string(lead_in, 'a') + bad + std::string(9, 'z');
    EXPECT_EQ(arnm_utf8_valid_length(bytes.data(), bytes.size()), lead_in)
        << "after " << lead_in << " ascii bytes";
    ExpectAgrees(bytes, "ascii lead in");
  }

  // and a run that is nothing but ascii, at every length across the stride boundary
  for (size_t length = 0; length < 40; ++length) {
    const std::string bytes(length, 'x');
    EXPECT_TRUE(arnm_utf8_is_valid(bytes.data(), bytes.size())) << "length " << length;
    EXPECT_EQ(arnm_utf8_valid_length(bytes.data(), bytes.size()), length);
  }
}

// promise: the two formulations agree on anything at all, including bytes no encoder would make
TEST(Utf8, AgreesWithTheReferenceOnRandomBytes) {
  std::mt19937 rng(20260907);

  // pure noise: mostly invalid, and the offsets have to match exactly
  for (int round = 0; round < 20000; ++round) {
    std::string bytes(rng() % 24u, '\0');
    for (char &c : bytes) { c = static_cast<char>(rng() & 0xFFu); }
    ExpectAgrees(bytes, "random bytes");
  }

  // valid sequences with one byte corrupted, which is where a plausible run turns
  const char *pieces[] = {"a", "\xc3\xa4", "\xe6\x97\xa5", "\xf0\x9f\x8e\x89"};
  for (int round = 0; round < 20000; ++round) {
    std::string bytes;
    for (int piece = 0; piece < 6; ++piece) { bytes += pieces[rng() % 4u]; }
    if (!bytes.empty() && (rng() & 1u)) {
      bytes[rng() % bytes.size()] = static_cast<char>(rng() & 0xFFu);
    }
    ExpectAgrees(bytes, "a corrupted run");
  }
}
