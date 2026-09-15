#include "arnm/hash.h"

#include "memory_limit.h"
#include <bitset>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

/*
 * The two hashes of arnm/hash.h. SipHash against the published vectors, the fast hash against
 * values a second implementation computed from its description -- a wrong hash still fills a
 * working table, so nothing downstream would notice. Then the property both are there for:
 * every input bit reaches the result.
 */

namespace {

// SipHash reference vectors: key 00 01 .. 0f, message 00 01 .. (length - 1), every length 0..63.
// SipHash-2-4 is vectors.h of github.com/veorq/SipHash; SipHash-1-3 was computed with a
// reference implementation that reproduces all 64 of those.
const uint64_t kSipHash24[64] = {
    0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL, 0x85676696d7fb7e2dULL,
    0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL, 0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL,
    0x93f5f5799a932462ULL, 0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
    0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL, 0xa129ca6149be45e5ULL,
    0x3f2acc7f57c29bdbULL, 0x699ae9f52cbe4794ULL, 0x4bc1b3f0968dd39cULL, 0xbb6dc91da77961bdULL,
    0xbed65cf21aa2ee98ULL, 0xd0f2cbb02e3b67c7ULL, 0x93536795e3a33e88ULL, 0xa80c038ccd5ccec8ULL,
    0xb8ad50c6f649af94ULL, 0xbce192de8a85b8eaULL, 0x17d835b85bbb15f3ULL, 0x2f2e6163076bcfadULL,
    0xde4daaaca71dc9a5ULL, 0xa6a2506687956571ULL, 0xad87a3535c49ef28ULL, 0x32d892fad841c342ULL,
    0x7127512f72f27cceULL, 0xa7f32346f95978e3ULL, 0x12e0b01abb051238ULL, 0x15e034d40fa197aeULL,
    0x314dffbe0815a3b4ULL, 0x027990f029623981ULL, 0xcadcd4e59ef40c4dULL, 0x9abfd8766a33735cULL,
    0x0e3ea96b5304a7d0ULL, 0xad0c42d6fc585992ULL, 0x187306c89bc215a9ULL, 0xd4a60abcf3792b95ULL,
    0xf935451de4f21df2ULL, 0xa9538f0419755787ULL, 0xdb9acddff56ca510ULL, 0xd06c98cd5c0975ebULL,
    0xe612a3cb9ecba951ULL, 0xc766e62cfcadaf96ULL, 0xee64435a9752fe72ULL, 0xa192d576b245165aULL,
    0x0a8787bf8ecb74b2ULL, 0x81b3e73d20b49b6fULL, 0x7fa8220ba3b2eceaULL, 0x245731c13ca42499ULL,
    0xb78dbfaf3a8d83bdULL, 0xea1ad565322a1a0bULL, 0x60e61c23a3795013ULL, 0x6606d7e446282b93ULL,
    0x6ca4ecb15c5f91e1ULL, 0x9f626da15c9625f3ULL, 0xe51b38608ef25f57ULL, 0x958a324ceb064572ULL,
};
const uint64_t kSipHash13[64] = {
    0xabac0158050fc4dcULL, 0xc9f49bf37d57ca93ULL, 0x82cb9b024dc7d44dULL, 0x8bf80ab8e7ddf7fbULL,
    0xcf75576088d38328ULL, 0xdef9d52f49533b67ULL, 0xc50d2b50c59f22a7ULL, 0xd3927d989bb11140ULL,
    0x369095118d299a8eULL, 0x25a48eb36c063de4ULL, 0x79de85ee92ff097fULL, 0x70c118c1f94dc352ULL,
    0x78a384b157b4d9a2ULL, 0x306f760c1229ffa7ULL, 0x605aa111c0f95d34ULL, 0xd320d86d2a519956ULL,
    0xcc4fdd1a7d908b66ULL, 0x9cf2689063dbd80cULL, 0x8ffc389cb473e63eULL, 0xf21f9de58d297d1cULL,
    0xc0dc2f46a6cce040ULL, 0xb992abfe2b45f844ULL, 0x7ffe7b9ba320872eULL, 0x525a0e7fdae6c123ULL,
    0xf464aeb267349c8cULL, 0x45cd5928705b0979ULL, 0x3a3e35e3ca9913a5ULL, 0xa91dc74e4ade3b35ULL,
    0xfb0bed02ef6cd00dULL, 0x88d93cb44ab1e1f4ULL, 0x540f11d643c5e663ULL, 0x2370dd1f8c21d1bcULL,
    0x81157b6c16a7b60dULL, 0x4d54b9e57a8ff9bfULL, 0x759f12781f2a753eULL, 0xcea1a3bebf186b91ULL,
    0x2cf508d3ada26206ULL, 0xb6101c2da3c33057ULL, 0xb3f47496ae3a36a1ULL, 0x626b57547b108392ULL,
    0xc1d2363299e41531ULL, 0x667cc1923f1ad944ULL, 0x65704ffec8138825ULL, 0x24f280d1c28949a6ULL,
    0xc2ca1cedfaf8876bULL, 0xc2164bfc9f042196ULL, 0xa16e9c9368b1d623ULL, 0x49fb169c8b5114fdULL,
    0x9f3143f8df074c46ULL, 0xc6fdaf2412cc86b3ULL, 0x7eaf49d10a52098fULL, 0x1cf313559d292f9aULL,
    0xc44a30dda2f41f12ULL, 0x36fae98943a71ed0ULL, 0x318fb34c73f0bce6ULL, 0xa27abf3670a7e980ULL,
    0xb4bcc0db243c6d75ULL, 0x23f8d852fdb71513ULL, 0x8f035f4da67d8a08ULL, 0xd89cd0e5b7e8f148ULL,
    0xf6f4e6bcf7a644eeULL, 0xaec59ad80f1837f2ULL, 0xc3b2f6154b6694e0ULL, 0x9d199062b7bbb3a8ULL,
};

constexpr uint64_t kReferenceK0 = 0x0706050403020100ULL;
constexpr uint64_t kReferenceK1 = 0x0f0e0d0c0b0a0908ULL;

std::vector<uint8_t> Counting(uint32_t length) {
  std::vector<uint8_t> bytes(length + 1u); // one more, so data() is never NULL
  for (uint32_t i = 0; i < length; ++i) { bytes[i] = static_cast<uint8_t>(i); }
  return bytes;
}

} // namespace

TEST(Hash, SipHashReferenceVectors) {
  for (uint32_t length = 0; length < 64; ++length) {
    const auto message = Counting(length);
    EXPECT_EQ(
        arnm_hash_siphash(message.data(), length, kReferenceK0, kReferenceK1, 2, 4),
        kSipHash24[length]
    ) << "SipHash-2-4, length "
      << length;
    EXPECT_EQ(
        arnm_hash_siphash13(message.data(), length, kReferenceK0, kReferenceK1), kSipHash13[length]
    ) << "SipHash-1-3, length "
      << length;
    EXPECT_EQ(
        arnm_hash_siphash13(message.data(), length, kReferenceK0, kReferenceK1),
        arnm_hash_siphash(message.data(), length, kReferenceK0, kReferenceK1, 1, 3)
    );
  }
}

TEST(Hash, FastPinnedValues) {
  // Message 00 01 .. (length - 1). Computed from the description in arnm/hash.h by a second
  // implementation, so a change to word order, tail padding or constants shows up here on every
  // platform. The fast hash may change between versions; when it does on purpose, so does this.
  const struct {
    uint32_t length;
    uint64_t hash;
  } pinned[] = {
      {0, 0xe9e0033e3badaf36ULL},  {1, 0x38f50c7db773a51eULL},  {7, 0x095327c0a660cd51ULL},
      {8, 0x4fa814e21d9922e3ULL},  {9, 0xd7165b931cf7bba6ULL},  {16, 0x38ccf3c9b7ba20f6ULL},
      {31, 0x1d703bf7d48f9d15ULL}, {32, 0xadbebe45b18683d0ULL}, {33, 0xfc017663f7e3b092ULL},
      {64, 0xf1d9110f00e03326ULL},
  };
  for (const auto &entry : pinned) {
    const auto message = Counting(entry.length);
    EXPECT_EQ(arnm_hash_fast(message.data(), entry.length), entry.hash)
        << "length " << entry.length;
  }
}

TEST(Hash, EmptyInputMayBeNull) {
  EXPECT_EQ(arnm_hash_fast(nullptr, 0), 0xe9e0033e3badaf36ULL);
  EXPECT_EQ(arnm_hash_siphash13(nullptr, 0, kReferenceK0, kReferenceK1), kSipHash13[0]);
  EXPECT_EQ(arnm_hash_siphash(nullptr, 0, kReferenceK0, kReferenceK1, 2, 4), kSipHash24[0]);
}

TEST(Hash, EveryInputBitChangesTheResult) {
  // 45 bytes: five whole words and a five byte tail, so the tail path is flipped too. Each of the
  // 360 single bit flips has to change the hash, and on average about half its bits -- a hash
  // that skipped bytes or had a weak finalizer would fail one or the other.
  std::mt19937_64 rng(41);
  const uint32_t length = 45;
  for (int round = 0; round < 20; ++round) {
    std::vector<uint8_t> input(length);
    for (uint8_t &byte : input) { byte = static_cast<uint8_t>(rng()); }
    const uint64_t k0 = rng(), k1 = rng();
    const uint64_t fast = arnm_hash_fast(input.data(), length);
    const uint64_t sip = arnm_hash_siphash13(input.data(), length, k0, k1);
    uint64_t fast_changed = 0, sip_changed = 0;
    for (uint32_t bit = 0; bit < length * 8u; ++bit) {
      input[bit / 8u] ^= static_cast<uint8_t>(1u << (bit % 8u));
      const uint64_t fast_flipped = arnm_hash_fast(input.data(), length);
      const uint64_t sip_flipped = arnm_hash_siphash13(input.data(), length, k0, k1);
      input[bit / 8u] ^= static_cast<uint8_t>(1u << (bit % 8u));
      ASSERT_NE(fast_flipped, fast) << "fast hash ignores bit " << bit;
      ASSERT_NE(sip_flipped, sip) << "SipHash ignores bit " << bit;
      fast_changed += std::bitset<64>(fast ^ fast_flipped).count();
      sip_changed += std::bitset<64>(sip ^ sip_flipped).count();
    }
    const double flips = length * 8.0;
    EXPECT_NEAR(static_cast<double>(fast_changed) / flips, 32.0, 2.0);
    EXPECT_NEAR(static_cast<double>(sip_changed) / flips, 32.0, 2.0);
  }
}

TEST(Hash, SipHashDependsOnBothKeyHalves) {
  const auto message = Counting(32);
  const uint64_t base = arnm_hash_siphash13(message.data(), 32, kReferenceK0, kReferenceK1);
  EXPECT_NE(arnm_hash_siphash13(message.data(), 32, kReferenceK0 ^ 1u, kReferenceK1), base);
  EXPECT_NE(arnm_hash_siphash13(message.data(), 32, kReferenceK0, kReferenceK1 ^ 1u), base);
  EXPECT_NE(arnm_hash_siphash13(message.data(), 32, kReferenceK1, kReferenceK0), base);
}
