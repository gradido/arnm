#ifndef ARNM_HASH_H
#define ARNM_HASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_hash arnm_hash
 * @brief Two hashes over every byte of a buffer: a fast unkeyed one and SipHash under a key.
 *
 * Both are here for hash tables -- @ref arnm_key_map places its keys by them -- and both are
 * `static inline` on purpose. A table that knows its key size at compile time passes it as a
 * constant, and the loops below then compile to a handful of word operations; a call into the
 * library could not see that constant. Measured through @ref ARNM_KEY_MAP_DEFINE at 1M keys of
 * 32 bytes, the hash with its length fixed saved about 6 ns per lookup.
 *
 * ### Every byte
 *
 * Neither hash picks bytes. A hash that reads a few is only as good as the guess where the
 * entropy of its input sits: random public keys have it everywhere, a serialized record has a
 * fixed header and trailer, and a hash over the first and last four bytes of the second kind
 * sends every record to the same slot.
 *
 * ### Which one
 *
 * @ref arnm_hash_fast() when nobody chooses the input to collide. It holds no secret: whoever
 * knows the function can compute inputs that share a hash.
 *
 * @ref arnm_hash_siphash13() when someone might. SipHash (Aumasson, Bernstein) under a 128 bit
 * key they cannot see keeps crafted collisions out; draw the key from the operating system's
 * random source once per process. It is a flooding defence, not a MAC. On 32 byte inputs it
 * measured about 6 ns more than the fast hash.
 *
 * ### Values are not a format
 *
 * The fast hash may change between arnm versions. Use its values to place data in memory, not
 * to name data on disk or on the wire. SipHash values are fixed by its specification and checked
 * against the reference vectors.
 *
 * @note Byte order: input words are read little endian byte by byte, so both hashes answer the
 *       same on every platform. Compilers turn the shifts into a single load where the machine
 *       order already is little endian.
 *
 * @whisper Every grain counted, and the count scattered where it cannot be foretold
 *
 * @{
 */

/** 8 bytes at @p bytes as a little endian 64 bit value; not NULL, at least 8 bytes. */
static inline uint64_t arnm_hash_load_le64(const uint8_t *bytes) {
  return (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8) | ((uint64_t)bytes[2] << 16) |
         ((uint64_t)bytes[3] << 24) | ((uint64_t)bytes[4] << 32) | ((uint64_t)bytes[5] << 40) |
         ((uint64_t)bytes[6] << 48) | ((uint64_t)bytes[7] << 56);
}

/** Rotate @p x left by @p bits, 1 to 63. */
static inline uint64_t arnm_hash_rotl64(uint64_t x, unsigned bits) {
  return (x << bits) | (x >> (64u - bits));
}

/** One word folded into the fast hash state: xor, multiply by the golden ratio, rotate. */
static inline uint64_t arnm_hash_fast_fold(uint64_t state, uint64_t word) {
  return arnm_hash_rotl64((state ^ word) * 0x9e3779b97f4a7c15ULL, 31);
}

/**
 * @brief The fast hash of @p length bytes: unkeyed, every byte read.
 *
 * The input is read in 8 byte little endian words, each folded in by
 * @ref arnm_hash_fast_fold(); a tail shorter than a word is zero padded into one more. The
 * length does not enter the hash, so inputs of different lengths whose padded words agree hash
 * alike -- a table of fixed size keys never meets that case. A splitmix64 finalizer spreads every
 * input bit over the whole result, which is what lets a table take its slot from the low bits.
 *
 * @param data   Bytes to hash; may be NULL only when @p length is 0.
 * @param length Byte count. A constant here unrolls the loop.
 * @return 64 bit hash. Not stable across arnm versions; see the module notes.
 * @whisper Each word stirred into the last, then the whole shaken until no grain sits still
 */
static inline uint64_t arnm_hash_fast(const uint8_t *data, uint32_t length) {
  uint64_t state = 0x243f6a8885a308d3ULL;
  const uint32_t whole = length & ~(uint32_t)7u;
  for (uint32_t offset = 0; offset < whole; offset += 8u) {
    state = arnm_hash_fast_fold(state, arnm_hash_load_le64(data + offset));
  }
  if (length != whole) {
    uint64_t tail = 0;
    for (uint32_t i = whole; i < length; ++i) { tail |= (uint64_t)data[i] << (8u * (i - whole)); }
    state = arnm_hash_fast_fold(state, tail);
  }
  state ^= state >> 30;
  state *= 0xbf58476d1ce4e5b9ULL;
  state ^= state >> 27;
  state *= 0x94d049bb133111ebULL;
  state ^= state >> 31;
  return state;
}

/** One SipRound over the four state words @p v. */
static inline void arnm_hash_sipround(uint64_t *v) {
  v[0] += v[1];
  v[1] = arnm_hash_rotl64(v[1], 13);
  v[1] ^= v[0];
  v[0] = arnm_hash_rotl64(v[0], 32);
  v[2] += v[3];
  v[3] = arnm_hash_rotl64(v[3], 16);
  v[3] ^= v[2];
  v[0] += v[3];
  v[3] = arnm_hash_rotl64(v[3], 21);
  v[3] ^= v[0];
  v[2] += v[1];
  v[1] = arnm_hash_rotl64(v[1], 17);
  v[1] ^= v[2];
  v[2] = arnm_hash_rotl64(v[2], 32);
}

/**
 * @brief SipHash-c-d of @p length bytes under the key (@p k0, @p k1).
 *
 * The general form, with the round counts as parameters: `c_rounds` compression rounds per
 * word, `d_rounds` finalization rounds. SipHash-2-4 is the variant of the paper and of the
 * published test vectors; @ref arnm_hash_siphash13() is the one to use in a hash table.
 *
 * @param data     Bytes to hash; may be NULL only when @p length is 0.
 * @param length   Byte count; its low 8 bits enter the final block, as the specification has it.
 * @param k0       First 8 bytes of the key, read little endian.
 * @param k1       Second 8 bytes of the key.
 * @param c_rounds Compression rounds per 8 byte word, at least 1.
 * @param d_rounds Finalization rounds, at least 1.
 * @return 64 bit SipHash-c-d.
 */
static inline uint64_t arnm_hash_siphash(
    const uint8_t *data,
    uint32_t length,
    uint64_t k0,
    uint64_t k1,
    unsigned c_rounds,
    unsigned d_rounds
) {
  uint64_t v[4] = {
      0x736f6d6570736575ULL ^ k0,
      0x646f72616e646f6dULL ^ k1,
      0x6c7967656e657261ULL ^ k0,
      0x7465646279746573ULL ^ k1,
  };
  const uint32_t whole = length & ~(uint32_t)7u;
  for (uint32_t offset = 0; offset < whole; offset += 8u) {
    const uint64_t m = arnm_hash_load_le64(data + offset);
    v[3] ^= m;
    for (unsigned r = 0; r < c_rounds; ++r) { arnm_hash_sipround(v); }
    v[0] ^= m;
  }
  // the last block: the length's low byte on top, the up to seven remaining bytes below it
  uint64_t last = (uint64_t)(length & 0xffu) << 56;
  for (uint32_t i = whole; i < length; ++i) { last |= (uint64_t)data[i] << (8u * (i - whole)); }
  v[3] ^= last;
  for (unsigned r = 0; r < c_rounds; ++r) { arnm_hash_sipround(v); }
  v[0] ^= last;
  v[2] ^= 0xffu;
  for (unsigned r = 0; r < d_rounds; ++r) { arnm_hash_sipround(v); }
  return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/**
 * @brief SipHash-1-3 of @p length bytes under the key (@p k0, @p k1).
 *
 * One compression round per word and three to finish -- what Rust's HashMap and CPython settled
 * on for hash tables fed from outside. @ref arnm_key_map_init_keyed() places keys by it.
 *
 * @param data   Bytes to hash; may be NULL only when @p length is 0.
 * @param length Byte count. A constant here unrolls the loop.
 * @param k0     First 8 bytes of the key, read little endian.
 * @param k1     Second 8 bytes of the key. Any value is a valid key; its strength is only how
 *               hard it is to guess.
 * @return 64 bit SipHash-1-3.
 * @whisper A secret stirred in with the grain, so no one outside can say where it settles
 */
static inline uint64_t arnm_hash_siphash13(
    const uint8_t *data, uint32_t length, uint64_t k0, uint64_t k1
) {
  return arnm_hash_siphash(data, length, k0, k1, 1u, 3u);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_HASH_H
