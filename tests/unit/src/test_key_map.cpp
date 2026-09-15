#include "arnm/arena.h"
#include "arnm/hash.h"
#include "arnm/key_map.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include "memory_limit.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <vector>

/*
 * arnm_key_map against a std::map that assigns ids the same way -- first sight, counting from 0
 * -- and against the things a hash table gets wrong: keys that share every byte but one, a
 * table filled to its load limit, an allocator that says no halfway, and a key vector that is
 * full. What the slots carry is checked against the hash itself, because a wrong hash still
 * makes a working table and nobody would notice -- only the probes would grow.
 */

namespace {

constexpr uint64_t kHashK0 = 0x243f6a8885a308d3ULL;
constexpr uint64_t kHashK1 = 0x13198a2e03707344ULL;

/** The two ways to open a map; most tests run on both. */
enum class Hash { Fast, Keyed };
const Hash kBothHashes[] = {Hash::Fast, Hash::Keyed};

const char *Name(Hash hash) {
  return Hash::Fast == hash ? "fast hash" : "SipHash-1-3";
}

arnm_result InitMap(arnm_key_map *map, Hash hash, size_t key_size, uint8_t log2, arnm *allocator) {
  return Hash::Fast == hash
             ? arnm_key_map_init(map, key_size, log2, allocator)
             : arnm_key_map_init_keyed(map, key_size, log2, kHashK0, kHashK1, allocator);
}

/** The 64 bit hash a map of @p hash places @p key by. */
uint64_t HashOf(Hash hash, const uint8_t *key, uint32_t size) {
  return Hash::Fast == hash ? arnm_hash_fast(key, size)
                            : arnm_hash_siphash13(key, size, kHashK0, kHashK1);
}

using Key = std::vector<uint8_t>;

// the generated calls for the three sizes the tests below run them at
ARNM_KEY_MAP_DEFINE(key8_map, 8)
ARNM_KEY_MAP_DEFINE(key16_map, 16)
ARNM_KEY_MAP_DEFINE(key32_map, 32)

/** get_or_insert and find, either the untyped library calls or a generated pair. */
struct Access {
  const char *name;
  arnm_result (*get_or_insert)(arnm_key_map *, const uint8_t *, uint32_t *, bool *);
  bool (*find)(const arnm_key_map *, const uint8_t *, uint32_t *);
};
const Access kUntyped = {"untyped", arnm_key_map_get_or_insert, arnm_key_map_find};

/** The generated pair for @p key_size; the tests define 8, 16 and 32. */
const Access &Generated(size_t key_size) {
  static const Access k8 = {"generated 8", key8_map_get_or_insert, key8_map_find};
  static const Access k16 = {"generated 16", key16_map_get_or_insert, key16_map_find};
  static const Access k32 = {"generated 32", key32_map_get_or_insert, key32_map_find};
  return key_size == 8 ? k8 : key_size == 16 ? k16 : k32;
}

Key RandomKey(std::mt19937_64 &rng, size_t size) {
  Key key(size);
  for (uint8_t &byte : key) { byte = static_cast<uint8_t>(rng()); }
  return key;
}

/** A borrowed arena over a static blob: a refusal is cheap and nothing reaches the host. */
struct SmallArena {
  explicit SmallArena(uint32_t bytes) : blob(bytes / 8u) {
    EXPECT_EQ(
        arnm_init_arena_borrow(&arena, reinterpret_cast<uint8_t *>(blob.data()), bytes),
        ARNM_SUCCESS
    );
  }
  std::vector<uint64_t> blob; // uint64_t so the storage is 8 byte aligned
  arnm arena{};
};

/**
 * Insert @p sequence, checking every answer against a std::map, then find every key and every
 * miss. The map is left filled for the caller.
 */
void CheckAgainstReference(
    arnm_key_map *map,
    const std::vector<Key> &sequence,
    const std::vector<Key> &misses,
    const Access &access = kUntyped
) {
  SCOPED_TRACE(access.name);
  std::map<Key, uint32_t> reference;
  for (const Key &key : sequence) {
    uint32_t id = UINT32_MAX;
    bool inserted = false;
    ASSERT_EQ(access.get_or_insert(map, key.data(), &id, &inserted), ARNM_SUCCESS);
    auto it = reference.find(key);
    if (it == reference.end()) {
      ASSERT_TRUE(inserted);
      ASSERT_EQ(id, reference.size()) << "a new key takes the next id";
      reference.emplace(key, id);
    } else {
      ASSERT_FALSE(inserted);
      ASSERT_EQ(id, it->second);
    }
  }
  ASSERT_EQ(arnm_key_map_size(map), reference.size());
  for (const auto &[key, expected] : reference) {
    uint32_t id = UINT32_MAX;
    ASSERT_TRUE(access.find(map, key.data(), &id));
    ASSERT_EQ(id, expected);
    ASSERT_EQ(0, memcmp(arnm_key_map_key(map, expected), key.data(), key.size()));
  }
  for (const Key &key : misses) {
    if (reference.count(key)) { continue; }
    uint32_t id = 12345;
    ASSERT_FALSE(access.find(map, key.data(), &id));
    ASSERT_EQ(id, 12345u) << "a miss leaves the output alone";
  }
}

/**
 * The map holds exactly @p keys, @p keys[i] under id i, and none of @p misses. Asks without
 * inserting, so it can be pointed at a map that is already filled.
 */
void CheckHolds(
    const arnm_key_map *map, const std::vector<Key> &keys, const std::vector<Key> &misses
) {
  ASSERT_EQ(arnm_key_map_size(map), keys.size());
  for (uint32_t i = 0; i < keys.size(); ++i) {
    uint32_t id = UINT32_MAX;
    ASSERT_TRUE(arnm_key_map_find(map, keys[i].data(), &id)) << "key " << i;
    ASSERT_EQ(id, i);
    ASSERT_EQ(0, memcmp(arnm_key_map_key(map, i), keys[i].data(), keys[i].size()));
  }
  for (const Key &key : misses) { ASSERT_FALSE(arnm_key_map_find(map, key.data(), nullptr)); }
}

} // namespace

// ---------------------------------------------------------------------------
// the hash
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// lifecycle and arguments
// ---------------------------------------------------------------------------

TEST(KeyMap, InitAllocatesNothingAndChecksItsArguments) {
  arnm_key_map map;
  EXPECT_EQ(arnm_key_map_init(nullptr, 32, 12, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_init(&map, 0, 12, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_key_map_init(&map, 70000, 12, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_key_map_init(&map, 32, 0, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_key_map_init(&map, 32, 16, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_key_map_init_keyed(nullptr, 32, 12, 1, 2, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_init_keyed(&map, 0, 12, 1, 2, nullptr), ARNM_ERROR_INVALID_PARAM);
  EXPECT_EQ(arnm_key_map_init_keyed(&map, 32, 16, 1, 2, nullptr), ARNM_ERROR_INVALID_PARAM);

  SmallArena small(64);
  ASSERT_EQ(arnm_key_map_init(&map, 32, 12, &small.arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&small.arena), 64u);
  EXPECT_EQ(map.slots, nullptr);
  EXPECT_EQ(map.capacity, 0u);
  EXPECT_EQ(map.hash, ARNM_KEY_MAP_HASH_FAST);
  EXPECT_EQ(map.hash_k0, 0u);
  EXPECT_EQ(map.hash_k1, 0u);
  EXPECT_EQ(arnm_key_map_size(&map), 0u);
  EXPECT_FALSE(arnm_key_map_find(&map, std::array<uint8_t, 32>{}.data(), nullptr));

  ASSERT_EQ(arnm_key_map_init_keyed(&map, 32, 12, kHashK0, kHashK1, &small.arena), ARNM_SUCCESS);
  EXPECT_EQ(arnm_arena_remaining(&small.arena), 64u);
  EXPECT_EQ(map.hash, ARNM_KEY_MAP_HASH_SIPHASH13);
  EXPECT_EQ(map.hash_k0, kHashK0);
  EXPECT_EQ(map.hash_k1, kHashK1);
}

TEST(KeyMap, NullAndUninitialized) {
  const uint8_t key[32] = {1};
  uint32_t id = 7;
  bool inserted = true;
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 32, 4, nullptr), ARNM_SUCCESS);

  EXPECT_EQ(arnm_key_map_get_or_insert(nullptr, key, &id, &inserted), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_get_or_insert(&map, nullptr, &id, &inserted), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_get_or_insert(&map, key, nullptr, &inserted), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_reserve(nullptr, 10), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_reserve(&map, 0), ARNM_ERROR_INVALID_PARAM);
  EXPECT_FALSE(arnm_key_map_find(nullptr, key, &id));
  EXPECT_FALSE(arnm_key_map_find(&map, nullptr, &id));
  arnm_key_map_clear(nullptr);
  arnm_key_map_free(nullptr);
  EXPECT_EQ(id, 7u);
  EXPECT_TRUE(inserted) << "failures leave outputs untouched";

  arnm_key_map zeroed{};
  EXPECT_EQ(arnm_key_map_get_or_insert(&zeroed, key, &id, nullptr), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(arnm_key_map_reserve(&zeroed, 10), ARNM_ERROR_INVALID_STATE);
  EXPECT_FALSE(arnm_key_map_find(&zeroed, key, &id));
  EXPECT_EQ(arnm_key_map_size(&zeroed), 0u);
  arnm_key_map_clear(&zeroed);
  arnm_key_map_free(&zeroed);

  // the inserted flag is optional
  ASSERT_EQ(arnm_key_map_get_or_insert(&map, key, &id, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(id, 0u);
  arnm_key_map_free(&map);
}

// ---------------------------------------------------------------------------
// agreement with a reference, on both hashes
// ---------------------------------------------------------------------------

TEST(KeyMap, AgreesWithReferenceForEveryKeySize) {
  std::mt19937_64 rng(2026);
  for (Hash hash : kBothHashes) {
    SCOPED_TRACE(Name(hash));
    for (size_t key_size : {1u, 7u, 8u, 9u, 16u, 32u, 33u, 100u}) {
      SCOPED_TRACE(testing::Message() << "key size " << key_size);
      // a key size of 1 has 256 keys to give, so it is filled completely and asked for all
      const size_t distinct = key_size == 1 ? 256 : 4000;
      std::vector<Key> pool;
      if (key_size == 1) {
        for (int b = 0; b < 256; ++b) { pool.push_back(Key{static_cast<uint8_t>(b)}); }
      } else {
        for (size_t i = 0; i < distinct; ++i) { pool.push_back(RandomKey(rng, key_size)); }
      }
      std::vector<Key> sequence;
      for (int i = 0; i < 20000; ++i) { sequence.push_back(pool[rng() % pool.size()]); }
      std::vector<Key> misses;
      for (int i = 0; i < 1000; ++i) { misses.push_back(RandomKey(rng, key_size)); }

      arnm_key_map on_host;
      ASSERT_EQ(InitMap(&on_host, hash, key_size, 6, nullptr), ARNM_SUCCESS);
      CheckAgainstReference(&on_host, sequence, misses);
      arnm_key_map_free(&on_host);

      arnm arena{};
      ASSERT_EQ(arnm_init_arena(&arena, 16u * 1024u * 1024u), ARNM_SUCCESS);
      arnm_key_map in_arena;
      ASSERT_EQ(InitMap(&in_arena, hash, key_size, 10, &arena), ARNM_SUCCESS);
      CheckAgainstReference(&in_arena, sequence, misses);
      arnm_key_map_free(&in_arena);
      arnm_release(&arena);
    }
  }
}

TEST(KeyMap, KeysDifferingInOneByteStayApart) {
  // every key the same but for one byte, at every position, including inside the last partial
  // word the hash reads
  std::vector<Key> keys;
  for (size_t position = 0; position < 37; ++position) {
    for (int value = 0; value < 256; value += 17) {
      Key key(37, 0xab);
      key[position] = static_cast<uint8_t>(value);
      keys.push_back(key);
    }
  }
  std::vector<Key> misses;
  for (size_t position = 0; position < 37; ++position) {
    Key key(37, 0xab);
    key[position] = 0x01; // 1 is not a multiple of 17
    misses.push_back(key);
  }
  for (Hash hash : kBothHashes) {
    SCOPED_TRACE(Name(hash));
    arnm_key_map map;
    ASSERT_EQ(InitMap(&map, hash, 37, 8, nullptr), ARNM_SUCCESS);
    CheckAgainstReference(&map, keys, misses);
    arnm_key_map_free(&map);
  }
}

TEST(KeyMap, SlotsCarryTheFoldedHash) {
  // white box: a slot is {32 bit hash, id}, the key's hash folded to 32 bits. A different hash
  // -- or the right one under the wrong hash key -- would still make a working table, so this
  // is the test that would notice.
  std::mt19937_64 rng(3);
  const Key key = RandomKey(rng, 32);
  for (Hash hash : kBothHashes) {
    SCOPED_TRACE(Name(hash));
    arnm_key_map map;
    ASSERT_EQ(InitMap(&map, hash, 32, 4, nullptr), ARNM_SUCCESS);
    uint32_t id = 1;
    ASSERT_EQ(arnm_key_map_get_or_insert(&map, key.data(), &id, nullptr), ARNM_SUCCESS);
    const uint64_t full = HashOf(hash, key.data(), 32);
    const uint32_t folded = static_cast<uint32_t>(full ^ (full >> 32));
    int holding = 0;
    for (uint32_t i = 0; i < map.capacity; ++i) {
      uint32_t slot_hash, slot_id;
      memcpy(&slot_hash, map.slots + 8u * i, 4);
      memcpy(&slot_id, map.slots + 8u * i + 4u, 4);
      if (slot_id == UINT32_MAX) { continue; }
      ++holding;
      EXPECT_EQ(slot_id, 0u);
      EXPECT_EQ(slot_hash, folded);
      EXPECT_EQ(i, folded & (map.capacity - 1u)) << "a lone key sits in its home slot";
    }
    EXPECT_EQ(holding, 1);
    arnm_key_map_free(&map);
  }

  // and the hash key is what the keyed map hashes under: another one places the key elsewhere
  const uint64_t under_other_key = arnm_hash_siphash13(key.data(), 32, kHashK0 ^ 1u, kHashK1);
  EXPECT_NE(HashOf(Hash::Keyed, key.data(), 32), under_other_key);
}

TEST(KeyMap, EqualSlotHashesAreToldApartByTheKey) {
  // For each hash, two 8 byte keys whose hash folds to the same 32 bits, found by a birthday
  // search over little endian integers (the keyed pair under kHashK0/kHashK1). They share a home
  // slot and pass the four byte filter, so only the key comparison keeps them apart.
  const struct {
    Hash hash;
    uint8_t first[8];
    uint8_t second[8];
  } pairs[] = {
      {Hash::Fast, {0xd2, 0xb1, 0, 0, 0, 0, 0, 0}, {0xe3, 0x96, 0x01, 0, 0, 0, 0, 0}},
      {Hash::Keyed, {0xa4, 0xd2, 0, 0, 0, 0, 0, 0}, {0x13, 0xd6, 0, 0, 0, 0, 0, 0}},
  };
  for (const auto &pair : pairs) {
    SCOPED_TRACE(Name(pair.hash));
    const uint64_t first_hash = HashOf(pair.hash, pair.first, 8);
    const uint64_t second_hash = HashOf(pair.hash, pair.second, 8);
    ASSERT_EQ(
        static_cast<uint32_t>(first_hash ^ (first_hash >> 32)),
        static_cast<uint32_t>(second_hash ^ (second_hash >> 32))
    ) << "the pair no longer collides; search a new one";

    arnm_key_map map;
    ASSERT_EQ(InitMap(&map, pair.hash, 8, 4, nullptr), ARNM_SUCCESS);
    uint32_t id = 9;
    bool inserted = false;
    ASSERT_EQ(arnm_key_map_get_or_insert(&map, pair.first, &id, &inserted), ARNM_SUCCESS);
    EXPECT_EQ(id, 0u);
    EXPECT_FALSE(arnm_key_map_find(&map, pair.second, &id));
    ASSERT_EQ(arnm_key_map_get_or_insert(&map, pair.second, &id, &inserted), ARNM_SUCCESS);
    EXPECT_EQ(id, 1u);
    EXPECT_TRUE(inserted);
    ASSERT_TRUE(arnm_key_map_find(&map, pair.first, &id));
    EXPECT_EQ(id, 0u);
    ASSERT_TRUE(arnm_key_map_find(&map, pair.second, &id));
    EXPECT_EQ(id, 1u);
    arnm_key_map_free(&map);
  }
}

TEST(KeyMap, SharedPrefixOrSuffixDoesNotShareASlot) {
  // 16000 keys that differ only in their first eight bytes, then 16000 that differ only in their
  // last eight: a hash that skipped either end would give each set one home slot and a run as
  // long as the set. Measured white box as the longest distance any key sits from its home slot,
  // which random keys keep in the tens at this load.
  for (Hash hash : kBothHashes) {
    for (bool vary_front : {true, false}) {
      SCOPED_TRACE(
          testing::Message() << Name(hash) << ", keys differ in the "
                             << (vary_front ? "first" : "last") << " 8 bytes"
      );
      arnm_key_map map;
      ASSERT_EQ(InitMap(&map, hash, 32, 12, nullptr), ARNM_SUCCESS);
      std::vector<Key> keys;
      for (uint64_t i = 0; i < 16000; ++i) {
        Key key(32, 0x5a);
        memcpy(key.data() + (vary_front ? 0 : 24), &i, 8);
        keys.push_back(key);
      }
      // through the generated calls, so the fixed size path meets these keys too
      CheckAgainstReference(&map, keys, {}, Generated(32));

      const uint32_t mask = map.capacity - 1u;
      uint32_t longest = 0;
      for (uint32_t i = 0; i < map.capacity; ++i) {
        uint32_t slot_hash, slot_id;
        memcpy(&slot_hash, map.slots + 8u * i, 4);
        memcpy(&slot_id, map.slots + 8u * i + 4u, 4);
        if (slot_id == UINT32_MAX) { continue; }
        const uint32_t distance = (i - (slot_hash & mask)) & mask;
        if (distance > longest) { longest = distance; }
      }
      EXPECT_LT(longest, 200u);
      arnm_key_map_free(&map);
    }
  }
}

TEST(KeyMap, WholeTableIsSearchedAcrossTheWrap) {
  // no hash key can pin a slot position, so this walks the table instead: fill to the load
  // limit, then every key must still be found, whichever of them ended up wrapping past the
  // last slot
  for (Hash hash : kBothHashes) {
    SCOPED_TRACE(Name(hash));
    arnm_key_map map;
    ASSERT_EQ(InitMap(&map, hash, 16, 4, nullptr), ARNM_SUCCESS);
    std::mt19937_64 rng(5);
    std::vector<Key> keys;
    for (int i = 0; i < 6; ++i) { keys.push_back(RandomKey(rng, 16)); }
    CheckAgainstReference(&map, keys, {RandomKey(rng, 16)});
    EXPECT_EQ(map.capacity, 8u) << "six keys fit three quarters of eight slots";
    arnm_key_map_free(&map);
  }
}

// ---------------------------------------------------------------------------
// the generated calls
// ---------------------------------------------------------------------------

TEST(KeyMapDefine, BuildsTheSameTableAsTheUntypedCalls) {
  // Same keys in the same order, once through the library and once through the inline copy:
  // the ids have to agree, and so does every byte of the slot table -- the copy is the same
  // algorithm with the size fixed, not a second one.
  std::mt19937_64 rng(23);
  for (Hash hash : kBothHashes) {
    for (size_t key_size : {8u, 16u, 32u}) {
      SCOPED_TRACE(testing::Message() << Name(hash) << ", key size " << key_size);
      std::vector<Key> pool;
      for (int i = 0; i < 3000; ++i) { pool.push_back(RandomKey(rng, key_size)); }
      std::vector<Key> sequence;
      for (int i = 0; i < 12000; ++i) { sequence.push_back(pool[rng() % pool.size()]); }
      std::vector<Key> misses;
      for (int i = 0; i < 500; ++i) { misses.push_back(RandomKey(rng, key_size)); }

      arnm_key_map untyped, generated;
      ASSERT_EQ(InitMap(&untyped, hash, key_size, 8, nullptr), ARNM_SUCCESS);
      ASSERT_EQ(InitMap(&generated, hash, key_size, 8, nullptr), ARNM_SUCCESS);
      CheckAgainstReference(&untyped, sequence, misses, kUntyped);
      CheckAgainstReference(&generated, sequence, misses, Generated(key_size));
      ASSERT_EQ(untyped.capacity, generated.capacity);
      EXPECT_EQ(0, memcmp(untyped.slots, generated.slots, 8u * untyped.capacity));
      // and each reads what the other wrote
      for (const Key &key : pool) {
        uint32_t a = 1, b = 2;
        const bool in_generated = arnm_key_map_find(&generated, key.data(), &a);
        ASSERT_EQ(in_generated, Generated(key_size).find(&untyped, key.data(), &b));
        if (in_generated) { EXPECT_EQ(a, b); }
      }
      arnm_key_map_free(&untyped);
      arnm_key_map_free(&generated);
    }
  }
}

TEST(KeyMapDefine, TheOtherGeneratedFunctionsForward) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 1024u * 1024u), ARNM_SUCCESS);
  arnm_key_map map;
  ASSERT_EQ(key32_map_init(&map, 6, &arena), ARNM_SUCCESS);
  EXPECT_EQ(map.keys.element_size, 32u);
  EXPECT_EQ(map.hash, ARNM_KEY_MAP_HASH_FAST);
  ASSERT_EQ(key32_map_reserve(&map, 100), ARNM_SUCCESS);
  const uint32_t remaining = arnm_arena_remaining(&arena);
  std::mt19937_64 rng(29);
  std::vector<Key> keys;
  for (int i = 0; i < 100; ++i) { keys.push_back(RandomKey(rng, 32)); }
  CheckAgainstReference(&map, keys, {}, Generated(32));
  EXPECT_EQ(arnm_arena_remaining(&arena), remaining);
  EXPECT_EQ(key32_map_size(&map), 100u);
  EXPECT_EQ(0, memcmp(key32_map_key(&map, 42), keys[42].data(), 32));
  key32_map_clear(&map);
  EXPECT_EQ(key32_map_size(&map), 0u);
  key32_map_free(&map);
  EXPECT_EQ(map.slots, nullptr);

  ASSERT_EQ(key16_map_init_keyed(&map, 6, kHashK0, kHashK1, nullptr), ARNM_SUCCESS);
  EXPECT_EQ(map.keys.element_size, 16u);
  EXPECT_EQ(map.hash, ARNM_KEY_MAP_HASH_SIPHASH13);
  EXPECT_EQ(map.hash_k0, kHashK0);
  EXPECT_EQ(map.hash_k1, kHashK1);
  key16_map_free(&map);
  arnm_release(&arena);
}

TEST(KeyMapDefine, RefusesAMapOfAnotherKeySize) {
  const uint8_t key[32] = {7};
  uint32_t id = 5;
  bool inserted = true;
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 16, 4, nullptr), ARNM_SUCCESS);
  ASSERT_EQ(arnm_key_map_get_or_insert(&map, key, &id, nullptr), ARNM_SUCCESS);
  id = 5;
  EXPECT_EQ(key32_map_get_or_insert(&map, key, &id, &inserted), ARNM_ERROR_INVALID_STATE);
  EXPECT_FALSE(key32_map_find(&map, key, &id));
  EXPECT_EQ(key8_map_get_or_insert(&map, key, &id, &inserted), ARNM_ERROR_INVALID_STATE);
  EXPECT_EQ(id, 5u);
  EXPECT_TRUE(inserted);
  EXPECT_EQ(arnm_key_map_size(&map), 1u);
  EXPECT_TRUE(key16_map_find(&map, key, &id));
  EXPECT_EQ(id, 0u);

  arnm_key_map zeroed{};
  EXPECT_EQ(key32_map_get_or_insert(&zeroed, key, &id, nullptr), ARNM_ERROR_INVALID_STATE);
  EXPECT_FALSE(key32_map_find(&zeroed, key, &id));
  EXPECT_EQ(key32_map_get_or_insert(nullptr, key, &id, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(key32_map_get_or_insert(&map, nullptr, &id, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(key32_map_get_or_insert(&map, key, nullptr, nullptr), ARNM_ERROR_NULL_POINTER);
  EXPECT_FALSE(key32_map_find(nullptr, key, &id));
  arnm_key_map_free(&map);
}

TEST(KeyMapDefine, GeneratedInsertRefusalKeepsEveryKey) {
  std::mt19937_64 rng(31);
  SmallArena small(20000u);
  arnm_key_map map;
  ASSERT_EQ(key32_map_init(&map, 3, &small.arena), ARNM_SUCCESS);
  std::vector<Key> held;
  arnm_result result = ARNM_SUCCESS;
  while (ARNM_SUCCESS == result) {
    Key key = RandomKey(rng, 32);
    uint32_t id = 77;
    result = key32_map_get_or_insert(&map, key.data(), &id, nullptr);
    if (ARNM_SUCCESS == result) {
      ASSERT_EQ(id, held.size());
      held.push_back(key);
    } else {
      EXPECT_EQ(id, 77u);
    }
  }
  EXPECT_EQ(result, ARNM_ERROR_OUT_OF_MEMORY);
  CheckHolds(&map, held, {RandomKey(rng, 32)});
}

TEST(KeyMapDefine, InsertAbsentChecksWhatItCan) {
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 8, 4, nullptr), ARNM_SUCCESS);
  const uint64_t first = 1, second = 2;
  const uint8_t *first_key = reinterpret_cast<const uint8_t *>(&first);
  const uint8_t *second_key = reinterpret_cast<const uint8_t *>(&second);
  uint32_t id = 9;
  EXPECT_EQ(arnm_key_map_insert_absent(nullptr, first_key, 0, 0, &id), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_insert_absent(&map, nullptr, 0, 0, &id), ARNM_ERROR_NULL_POINTER);
  EXPECT_EQ(arnm_key_map_insert_absent(&map, first_key, 0, 0, nullptr), ARNM_ERROR_NULL_POINTER);
  arnm_key_map zeroed{};
  EXPECT_EQ(arnm_key_map_insert_absent(&zeroed, first_key, 0, 0, &id), ARNM_ERROR_INVALID_STATE);

  // no table yet: the slot index is ignored, the table is built around the key
  const uint32_t first_hash = arnm_key_map_slot_hash(&map, first_key, 8);
  ASSERT_EQ(arnm_key_map_insert_absent(&map, first_key, first_hash, 12345, &id), ARNM_SUCCESS);
  EXPECT_EQ(id, 0u);
  ASSERT_TRUE(arnm_key_map_find(&map, first_key, &id));
  EXPECT_EQ(id, 0u);

  // with a table: the slot has to be inside it and free
  const uint32_t second_hash = arnm_key_map_slot_hash(&map, second_key, 8);
  const uint32_t occupied = arnm_key_map_probe(&map, first_key, first_hash, 8);
  id = 9;
  EXPECT_EQ(
      arnm_key_map_insert_absent(&map, second_key, second_hash, map.capacity, &id),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      arnm_key_map_insert_absent(&map, second_key, second_hash, occupied, &id),
      ARNM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(id, 9u);
  EXPECT_EQ(arnm_key_map_size(&map), 1u);

  const uint32_t free_slot = arnm_key_map_probe(&map, second_key, second_hash, 8);
  ASSERT_EQ(
      arnm_key_map_insert_absent(&map, second_key, second_hash, free_slot, &id), ARNM_SUCCESS
  );
  EXPECT_EQ(id, 1u);
  ASSERT_TRUE(arnm_key_map_find(&map, second_key, &id));
  EXPECT_EQ(id, 1u);
  arnm_key_map_free(&map);
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

TEST(KeyMap, ReserveMeansNoAllocationUpToTheCount) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 8u * 1024u * 1024u), ARNM_SUCCESS);
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 32, 12, &arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_key_map_reserve(&map, 10000), ARNM_SUCCESS);
  const uint32_t remaining = arnm_arena_remaining(&arena);
  EXPECT_EQ(map.capacity, 16384u);

  std::mt19937_64 rng(9);
  std::vector<Key> keys;
  for (int i = 0; i < 10000; ++i) { keys.push_back(RandomKey(rng, 32)); }
  CheckAgainstReference(&map, keys, {});
  EXPECT_EQ(arnm_arena_remaining(&arena), remaining);

  // asking for less changes nothing
  ASSERT_EQ(arnm_key_map_reserve(&map, 100), ARNM_SUCCESS);
  EXPECT_EQ(map.capacity, 16384u);
  EXPECT_EQ(arnm_arena_remaining(&arena), remaining);

  EXPECT_EQ(arnm_key_map_reserve(&map, ARNM_KEY_MAP_MAX_KEYS + 1u), ARNM_ERROR_ARITHMETIC_OVERFLOW);
  arnm_release(&arena);
}

TEST(KeyMap, KnownKeyNeverAllocatesAndRefusalChangesNothing) {
  // an arena with room for exactly one table of 8 slots and one bucket of 8 keys, nothing more
  SmallArena exact(8u * 8u + 8u * 32u + 8u * 8u);
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 32, 3, &exact.arena), ARNM_SUCCESS);
  std::mt19937_64 rng(11);
  std::vector<Key> keys;
  for (int i = 0; i < 6; ++i) { keys.push_back(RandomKey(rng, 32)); }
  CheckAgainstReference(&map, keys, {});
  ASSERT_EQ(map.capacity, 8u);

  // the table is at its load limit: a known key must still answer without asking for memory
  uint32_t id = 99;
  bool inserted = true;
  ASSERT_EQ(arnm_key_map_get_or_insert(&map, keys[3].data(), &id, &inserted), ARNM_SUCCESS);
  EXPECT_EQ(id, 3u);
  EXPECT_FALSE(inserted);

  // a new key needs the doubled table, which the arena cannot give
  Key fresh = RandomKey(rng, 32);
  id = 99;
  inserted = true;
  EXPECT_EQ(
      arnm_key_map_get_or_insert(&map, fresh.data(), &id, &inserted), ARNM_ERROR_OUT_OF_MEMORY
  );
  EXPECT_EQ(id, 99u);
  EXPECT_TRUE(inserted);
  EXPECT_EQ(arnm_key_map_size(&map), 6u);
  EXPECT_EQ(map.capacity, 8u);
  CheckHolds(&map, keys, {fresh});
}

TEST(KeyMap, RefusedKeyBucketWritesNoSlot) {
  // Table and key buckets reserved for 40 keys, and not a byte more in the arena: the 41st key
  // fits the table (48 of 64 slots) but needs a sixth bucket. The refusal has to come before the
  // slot is written -- a slot naming id 40 with no key behind it would send the next probe for
  // that key into a bucket that does not exist.
  SmallArena exact(64u * 8u + 5u * 8u + 5u * 8u * 32u);
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 32, 3, &exact.arena), ARNM_SUCCESS);
  ASSERT_EQ(arnm_key_map_reserve(&map, 40), ARNM_SUCCESS);
  ASSERT_EQ(map.capacity, 64u);
  ASSERT_EQ(arnm_arena_remaining(&exact.arena), 0u) << "the arena is sized to the reserve";

  std::mt19937_64 rng(19);
  std::vector<Key> keys;
  for (int i = 0; i < 40; ++i) { keys.push_back(RandomKey(rng, 32)); }
  CheckAgainstReference(&map, keys, {});

  const Key fresh = RandomKey(rng, 32);
  uint32_t id = 77;
  EXPECT_EQ(arnm_key_map_get_or_insert(&map, fresh.data(), &id, nullptr), ARNM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(id, 77u);
  CheckHolds(&map, keys, {fresh});
}

TEST(KeyMap, RefusedAllocationAtAnyPointKeepsEveryKey) {
  std::mt19937_64 rng(13);
  // the smallest leaves room for one table of 8 slots, one index array and one bucket of 8 keys
  // (384 bytes) and nothing else; the larger ones run out at a grow of the table, of the index
  // array or of a bucket, whichever comes first
  for (uint32_t bytes : {384u, 1024u, 4096u, 20000u, 100000u}) {
    SCOPED_TRACE(testing::Message() << "arena bytes " << bytes);
    SmallArena small(bytes);
    arnm_key_map map;
    ASSERT_EQ(arnm_key_map_init(&map, 32, 3, &small.arena), ARNM_SUCCESS);
    std::vector<Key> held;
    arnm_result result = ARNM_SUCCESS;
    while (ARNM_SUCCESS == result) {
      Key key = RandomKey(rng, 32);
      uint32_t id = 0;
      result = arnm_key_map_get_or_insert(&map, key.data(), &id, nullptr);
      if (ARNM_SUCCESS == result) {
        ASSERT_EQ(id, held.size());
        held.push_back(key);
      }
    }
    EXPECT_EQ(result, ARNM_ERROR_OUT_OF_MEMORY);
    ASSERT_FALSE(held.empty());
    CheckHolds(&map, held, {RandomKey(rng, 32)});
  }
}

TEST(KeyMap, FullKeyVectorIsAnOverflowNotACorruption) {
  // Two keys per bucket, so the key vector's bucket index is the first thing to run out. Where
  // exactly depends on how the vector got there: reserved, it takes all
  // ARNM_BVEC_MAX_INDEX_CAPACITY buckets; grown push by push, its index steps by
  // ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE and refuses the step that would pass the ceiling, a
  // few buckets short of it. Both are the vector's to decide -- what the map owes is that the
  // refusal arrives as an overflow and leaves every key it had.
  const uint32_t reserved_limit = 2u * ARNM_BVEC_MAX_INDEX_CAPACITY;
  for (bool reserve_first : {false, true}) {
    SCOPED_TRACE(reserve_first ? "reserved" : "grown");
    arnm_key_map map;
    ASSERT_EQ(arnm_key_map_init(&map, 8, 1, nullptr), ARNM_SUCCESS);
    if (reserve_first) { ASSERT_EQ(arnm_key_map_reserve(&map, reserved_limit), ARNM_SUCCESS); }

    arnm_result result = ARNM_SUCCESS;
    uint64_t next = 0;
    for (; next <= reserved_limit; ++next) {
      uint32_t id = 0;
      result =
          arnm_key_map_get_or_insert(&map, reinterpret_cast<const uint8_t *>(&next), &id, nullptr);
      if (ARNM_SUCCESS != result) { break; }
      ASSERT_EQ(id, next);
    }
    EXPECT_EQ(result, ARNM_ERROR_ARITHMETIC_OVERFLOW);
    const uint32_t held = static_cast<uint32_t>(next);
    if (reserve_first) {
      EXPECT_EQ(held, reserved_limit);
    } else {
      EXPECT_GT(held, reserved_limit - 2u * ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE);
      EXPECT_LE(held, reserved_limit);
    }

    uint32_t id = 42;
    EXPECT_EQ(
        arnm_key_map_get_or_insert(&map, reinterpret_cast<const uint8_t *>(&next), &id, nullptr),
        ARNM_ERROR_ARITHMETIC_OVERFLOW
    );
    EXPECT_EQ(id, 42u);
    EXPECT_EQ(arnm_key_map_size(&map), held);
    EXPECT_FALSE(arnm_key_map_find(&map, reinterpret_cast<const uint8_t *>(&next), nullptr));
    for (uint64_t i = 0; i < held; i += 97) {
      ASSERT_TRUE(arnm_key_map_find(&map, reinterpret_cast<const uint8_t *>(&i), &id));
      ASSERT_EQ(id, i);
    }
    // a known key is still answered at the ceiling
    const uint64_t first = 0;
    EXPECT_EQ(
        arnm_key_map_get_or_insert(&map, reinterpret_cast<const uint8_t *>(&first), &id, nullptr),
        ARNM_SUCCESS
    );
    EXPECT_EQ(id, 0u);
    EXPECT_EQ(arnm_key_map_reserve(&map, reserved_limit + 1u), ARNM_ERROR_ARITHMETIC_OVERFLOW);
    arnm_key_map_free(&map);
  }
}

TEST(KeyMap, ClearKeepsTheMemoryAndStartsIdsAgain) {
  arnm arena{};
  ASSERT_EQ(arnm_init_arena(&arena, 1024u * 1024u), ARNM_SUCCESS);
  arnm_key_map map;
  ASSERT_EQ(arnm_key_map_init(&map, 32, 8, &arena), ARNM_SUCCESS);
  std::mt19937_64 rng(17);
  std::vector<Key> first, second;
  for (int i = 0; i < 1000; ++i) { first.push_back(RandomKey(rng, 32)); }
  for (int i = 0; i < 1000; ++i) { second.push_back(RandomKey(rng, 32)); }

  CheckAgainstReference(&map, first, {});
  const uint32_t remaining = arnm_arena_remaining(&arena);
  arnm_key_map_clear(&map);
  EXPECT_EQ(arnm_key_map_size(&map), 0u);
  for (const Key &key : first) { ASSERT_FALSE(arnm_key_map_find(&map, key.data(), nullptr)); }
  CheckAgainstReference(&map, second, first);
  EXPECT_EQ(arnm_arena_remaining(&arena), remaining) << "the second round reused the first";

  // free keeps what init wrote, so the descriptor fills again without another init
  arnm_key_map_free(&map);
  EXPECT_EQ(map.slots, nullptr);
  EXPECT_EQ(arnm_key_map_size(&map), 0u);
  arnm_reset(&arena);
  CheckAgainstReference(&map, first, second);
  arnm_release(&arena);
}
