#ifndef ARNM_KEY_MAP_H
#define ARNM_KEY_MAP_H

#include "arnm/bucket_vector.h"
#include "arnm/hash.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same reasoning as ARNM_BVEC_MAYBE_UNUSED in arnm/bucket_vector.h: ARNM_KEY_MAP_DEFINE expands
   a whole set of wrappers into the consumer's translation unit, and clang reports each one it
   did not call. */
#if defined(__GNUC__) || defined(__clang__)
#define ARNM_KEY_MAP_MAYBE_UNUSED __attribute__((unused))
#else
#define ARNM_KEY_MAP_MAYBE_UNUSED
#endif

/**
 * @defgroup arnm_key_map arnm_key_map
 * @brief Fixed size keys turned into dense ids: 0, 1, 2, ... in the order they first arrive.
 *
 * A key is a run of bytes whose length is set once per map -- a 32 byte public key, a 16 byte
 * uuid. The first time a key is inserted it receives the next id; every later insert or find of
 * the same bytes answers that id again. There is no value type: the id is the value, and the
 * payload belongs in an @ref arnm_bvec (or any array) at that index, where it is one load away
 * and packed with its neighbours.
 *
 * ### Two blocks of memory
 *
 * The table is one block of 8 byte slots, each holding a 32 bit hash of its key and the key's
 * id. The keys themselves live beside the table in an @ref arnm_bvec, stored once, at their id.
 * A probe compares four bytes per occupied slot and reads a key only when those four match, so
 * the table stays small enough to keep in cache and a long key costs one extra load per hit
 * rather than one per probe. It also makes the keys readable in id order, which is what
 * @ref arnm_key_map_key() is.
 *
 * Collisions are resolved by linear probing: a key that finds its slot taken tries the next
 * one. The table holds at most three quarters of its slots and doubles when an insert would pass
 * that. Measured against the neighbours: one half made misses faster for a table twice the size,
 * seven eighths saved a quarter of the table and made hits and misses slower.
 *
 * ### Nothing is removed
 *
 * There is no delete. Ids are dense precisely because none is ever given back, and without a
 * delete there are no tombstones for a probe to step over. @ref arnm_key_map_clear() forgets
 * every key at once and keeps the memory for the next round.
 *
 * ### Two hashes, and which init picks which
 *
 * Both read every byte of the key. A hash that picks a few is only as good as the guess where a
 * key's entropy sits: keys that share a prefix or a suffix -- serialized records, counters,
 * addresses in one range -- would all land in one slot, and every insert and find would walk
 * the same run. A table of 20000 keys with the same first eight bytes went from 30 ns to 15 us
 * per operation under a hash that read only those eight.
 *
 * @ref arnm_key_map_init() uses @ref arnm_hash_fast() from arnm/hash.h, a multiply-rotate mix
 * with a splitmix64 finalizer. It takes no key and holds no secret, so whoever knows it can
 * compute keys that collide on purpose.
 *
 * @ref arnm_key_map_init_keyed() uses @ref arnm_hash_siphash13() under a 128 bit hash key from
 * the caller. Keys
 * built to collide need that key, so a map whose keys an adversary chooses keeps its speed.
 * Draw the hash key from the operating system's random source once per process; arnm has no
 * source of randomness and keeps no global state, so it asks rather than guesses. Measured on
 * 32 byte keys, it costs about 6 ns per operation more than the mix.
 *
 * Either way a collision never makes an answer wrong -- the key itself is always compared -- it
 * only makes the probe longer.
 *
 * ### One implementation, and a copy of the hot path for your key size
 *
 * Every call below takes the key size from the map at run time, which is what lets one library
 * serve every length. It is also what a lookup pays for: `memcmp` with a length the compiler
 * does not know is a call into code that branches on it, and the hash loop runs a count it
 * cannot unroll. Where a lookup mostly waits on memory, those branches cost more than their
 * instructions -- at 1M keys of 32 bytes a find hit measured 131 ns this way and 95 ns with the
 * size fixed.
 *
 * @ref ARNM_KEY_MAP_DEFINE generates the fixed version for one key size, the way
 * @ref ARNM_BVEC_DEFINE generates a typed vector: `name##_get_or_insert()` and `name##_find()`
 * are inline copies of the lookup with the size a constant, and everything else forwards to the
 * library. The copies are built from the inline blocks at the end of this header --
 * @ref arnm_key_map_slot_hash(), @ref arnm_key_map_probe() -- and the library's own calls are
 * built from the same blocks with the size read from the map, so there is one algorithm and
 * not two. A new key leaves the copy through @ref arnm_key_map_insert_absent(), where every
 * allocation of this container happens.
 *
 * ### Where the memory comes from
 *
 * The allocator named at init serves both blocks, and is kept for the map's life -- the map
 * grows, so it has to be able to ask. A grow allocates the doubled table before letting go of
 * the old one, so for a moment both are held; behind an arena the old one is not taken back at
 * all. Call @ref arnm_key_map_reserve() with the expected key count and neither happens.
 *
 * ### NULL
 *
 * Every call returning an @ref arnm_result answers @ref ARNM_ERROR_NULL_POINTER for a NULL map,
 * key or required output. @ref arnm_key_map_find() answers false, and @ref arnm_key_map_clear()
 * and @ref arnm_key_map_free() do nothing. The inline accessors read a map the caller holds and
 * do not check.
 *
 * @note Nothing here is thread safe. A find is a read and may run beside other finds, but not
 *       beside an insert.
 *
 * @whisper Each arrival is given a number, and keeps it for as long as the map remembers
 *
 * @{
 */

/** @brief Most slots a table ever has: 2^28, whose 8 byte slots still fit one allocation. */
#define ARNM_KEY_MAP_MAX_CAPACITY ((uint32_t)1u << 28)

/** @brief Most keys a map ever holds: three quarters of @ref ARNM_KEY_MAP_MAX_CAPACITY. */
#define ARNM_KEY_MAP_MAX_KEYS (ARNM_KEY_MAP_MAX_CAPACITY - ARNM_KEY_MAP_MAX_CAPACITY / 4u)

/** @brief Which hash a map places its keys by; set by the init call, read by nothing outside. */
typedef enum arnm_key_map_hash {
  ARNM_KEY_MAP_HASH_FAST = 0,      /**< unkeyed mix, from @ref arnm_key_map_init() */
  ARNM_KEY_MAP_HASH_SIPHASH13 = 1, /**< keyed, from @ref arnm_key_map_init_keyed() */
} arnm_key_map_hash;

/**
 * @brief A key map, whatever length its keys have.
 *
 * Fields are readable, as a debugger or a test reads them; writing one is the caller stepping
 * outside the interface. The key count is not stored twice -- it is the size of @c keys -- and
 * neither is the allocator, which @c keys carries.
 *
 * Not usable until @ref arnm_key_map_init(). A zeroed descriptor reads as empty and refuses
 * inserts with @ref ARNM_ERROR_INVALID_STATE.
 */
typedef struct arnm_key_map {
  uint8_t *slots;    /**< @c capacity slots of 8 bytes, or NULL before the first insert. */
  arnm_bvec keys;    /**< One key per id; its element size is the key size. */
  uint64_t hash_k0;  /**< First half of the SipHash key; 0 for the fast hash. */
  uint64_t hash_k1;  /**< Second half of the SipHash key; 0 for the fast hash. */
  uint32_t capacity; /**< Slots in the table: 0, or a power of two from 8 up. */
  uint8_t hash;      /**< An @ref arnm_key_map_hash. */
} arnm_key_map;

/**
 * @brief Prepare an empty map on the fast hash. Allocates nothing; the first insert or reserve
 * builds the table.
 *
 * @param[out]    map              Descriptor to initialize; not NULL. Every field is written
 *                                 and none is read, so uninitialized storage is a valid input.
 * @param[in]     key_size         Bytes per key; 1 to `UINT16_MAX`, and small enough that a
 *                                 bucket of keys fits a uint32_t.
 * @param[in]     keys_bucket_log2 Keys per bucket of the key vector, as a power of two, 1 to
 *                                 15 -- the bucket exponent of @ref arnm_bvec_init(). Together
 *                                 with @ref ARNM_BVEC_MAX_INDEX_CAPACITY it bounds how many
 *                                 keys the map can hold, so pick it for the expected count:
 *                                 12 holds 33 million keys, 15 holds all a map ever can.
 * @param[in,out] allocator        Where the table and the keys come from, or NULL for the host.
 *                                 Kept for the map's whole life.
 * @retval ARNM_SUCCESS             Ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER  @p map is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p key_size or @p keys_bucket_log2 is outside the bounds
 *                                  above.
 * @warning Calling this on a map that still holds memory leaks it. Use
 *          @ref arnm_key_map_free() first.
 * @whisper An empty ledger, its column widths already drawn
 */
arnm_result arnm_key_map_init(
    arnm_key_map *map, size_t key_size, uint8_t keys_bucket_log2, arnm *allocator
);

/**
 * @brief Prepare an empty map on SipHash-1-3 under a hash key. Allocates nothing.
 *
 * Everything @ref arnm_key_map_init() says holds; only the hash differs. The one to use when the
 * keys come from someone who might choose them to collide.
 *
 * @param[out]    map              As in @ref arnm_key_map_init().
 * @param[in]     key_size         As in @ref arnm_key_map_init().
 * @param[in]     keys_bucket_log2 As in @ref arnm_key_map_init().
 * @param[in]     hash_k0          First 8 bytes of the SipHash key, read little endian.
 * @param[in]     hash_k1          Second 8 bytes of the SipHash key. Any value, zero included,
 *                                 is a valid key; its strength is only how hard it is to guess.
 * @param[in,out] allocator        As in @ref arnm_key_map_init().
 * @retval ARNM_SUCCESS             Ready and empty.
 * @retval ARNM_ERROR_NULL_POINTER  @p map is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p key_size or @p keys_bucket_log2 is out of bounds.
 * @whisper The same ledger, its page numbers known only to its keeper
 */
arnm_result arnm_key_map_init_keyed(
    arnm_key_map *map,
    size_t key_size,
    uint8_t keys_bucket_log2,
    uint64_t hash_k0,
    uint64_t hash_k1,
    arnm *allocator
);

/**
 * @brief Make room for @p key_count keys, so that no insert up to that count allocates.
 *
 * Sizes the table so that @p key_count keys stay within three quarters of it, and reserves the
 * key buckets for them. Never shrinks: asking for less than the map already has room for
 * succeeds and changes nothing. The call to make behind an arena, where every table a grow
 * leaves behind stays stranded.
 *
 * @param[in,out] map       Map; not NULL, initialized.
 * @param[in]     key_count Keys to make room for; must be > 0.
 * @retval ARNM_SUCCESS                   The room is there.
 * @retval ARNM_ERROR_NULL_POINTER        @p map is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p key_count is 0.
 * @retval ARNM_ERROR_INVALID_STATE       @p map never saw @ref arnm_key_map_init().
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW More than @ref ARNM_KEY_MAP_MAX_KEYS, or more keys than
 *                                        the key vector can address.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator ran out. Every key and id is as it was;
 *                                        key buckets that were handed over stay for later.
 * @whisper Shelves put up before the goods arrive
 */
arnm_result arnm_key_map_reserve(arnm_key_map *map, uint32_t key_count);

/**
 * @brief The id of @p key, giving it the next one if the map has not seen it.
 *
 * A key the map already holds is answered from the table and never allocates, not even when
 * the table is due to grow. A new key takes id `arnm_key_map_size()`, is copied into the key
 * vector, and may grow the table first.
 *
 * @param[in,out] map          Map; not NULL, initialized.
 * @param[in]     key          @c key_size bytes; not NULL.
 * @param[out]    out_id       Receives the key's id; not NULL. Untouched on failure.
 * @param[out]    out_inserted Receives true when the key was new and false when it was known;
 *                             may be NULL when the caller does not ask. Untouched on failure.
 * @retval ARNM_SUCCESS                   @p out_id holds the id.
 * @retval ARNM_ERROR_NULL_POINTER        @p map, @p key or @p out_id is NULL.
 * @retval ARNM_ERROR_INVALID_STATE       @p map never saw @ref arnm_key_map_init().
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The map holds @ref ARNM_KEY_MAP_MAX_KEYS, or the key
 *                                        vector is full.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator had no room for the doubled table or the
 *                                        next key bucket. The key was not added and every
 *                                        other key keeps its id.
 * @whisper A name read at the door: known faces pass, new ones are written in
 */
arnm_result arnm_key_map_get_or_insert(
    arnm_key_map *map, const uint8_t *key, uint32_t *out_id, bool *out_inserted
);

/**
 * @brief The id of @p key, if the map holds it. Never allocates, never changes the map.
 *
 * @param[in]  map    Map; NULL answers false.
 * @param[in]  key    @c key_size bytes; NULL answers false.
 * @param[out] out_id Receives the id when the key is found; may be NULL to only ask whether it
 *                    is there. Untouched when it is not.
 * @return true when the map holds @p key.
 * @whisper A name looked up, and the page left as it was
 */
bool arnm_key_map_find(const arnm_key_map *map, const uint8_t *key, uint32_t *out_id);

/** Keys held, which is also the id the next new key receives. */
static inline uint32_t arnm_key_map_size(const arnm_key_map *map) {
  return arnm_bvec_size(&map->keys);
}

/** The key stored under @p id -- unchecked, @p id must be below arnm_key_map_size(). */
static inline const uint8_t *arnm_key_map_key(const arnm_key_map *map, uint32_t id) {
  return (const uint8_t *)arnm_bvec_get(&map->keys, id);
}

/**
 * @brief Forget every key and keep the table and the key buckets for the next round.
 *
 * The table is marked empty slot by slot, so the cost is proportional to its size; nothing is
 * allocated or given back. Ids start again at 0.
 *
 * @param[in,out] map Map; NULL is a no-op.
 * @warning Every pointer @ref arnm_key_map_key() handed out is dangling afterwards.
 * @whisper The ledger wiped, the columns still drawn
 */
void arnm_key_map_clear(arnm_key_map *map);

/**
 * @brief Give the table and every key bucket back, leaving a descriptor that can be filled
 * again.
 *
 * What stays is what @ref arnm_key_map_init() wrote: key size, bucket exponent and allocator.
 * Behind an arena the blocks are released on the arena's terms -- only what lies at its tail comes
 * back, the rest waits for its reset.
 *
 * @param[in,out] map Map; NULL is a no-op.
 * @warning Every pointer @ref arnm_key_map_key() handed out is dangling afterwards.
 * @whisper The ledger closed and the paper returned, the rules kept for the next one
 */
void arnm_key_map_free(arnm_key_map *map);

/* ********** building blocks of the inline lookup ********** */

/** @brief Marks a free slot; no key ever receives this id. */
#define ARNM_KEY_MAP_EMPTY_ID UINT32_MAX

/**
 * @brief One slot of the table: the key's folded hash and its id.
 *
 * `id == ARNM_KEY_MAP_EMPTY_ID` marks a free slot. A key's home slot is
 * `hash & (capacity - 1)`; it lives there or in the first free slot after it, wrapping at the
 * end. The same 32 bits pick the home slot and filter the key comparison.
 */
typedef struct arnm_key_map_slot {
  uint32_t hash; /**< The key's hash folded to 32 bits; undefined in a free slot. */
  uint32_t id;   /**< The key's id, or @ref ARNM_KEY_MAP_EMPTY_ID. */
} arnm_key_map_slot;

/**
 * @brief The 32 bits a slot keeps for @p key: the map's hash, folded.
 *
 * Which hash is read from the map; the branch goes the same way for every call on one map.
 *
 * @param map      Map; initialized, not NULL -- unchecked.
 * @param key      @p key_size bytes; not NULL -- unchecked.
 * @param key_size The map's key size. Passed rather than read so a constant can reach the loops.
 */
static inline uint32_t arnm_key_map_slot_hash(
    const arnm_key_map *map, const uint8_t *key, uint32_t key_size
) {
  const uint64_t hash = ARNM_KEY_MAP_HASH_SIPHASH13 == map->hash
                            ? arnm_hash_siphash13(key, key_size, map->hash_k0, map->hash_k1)
                            : arnm_hash_fast(key, key_size);
  return (uint32_t)(hash ^ (hash >> 32));
}

/**
 * @brief The slot holding @p key, or the free slot where it would go.
 *
 * Walks from the home slot until it meets the key or a free slot. Terminates because the load
 * limit keeps a quarter of the table free.
 *
 * @param map      Map with a table (`capacity > 0`) -- unchecked.
 * @param key      @p key_size bytes; not NULL -- unchecked.
 * @param hash     @ref arnm_key_map_slot_hash() of @p key.
 * @param key_size The map's key size; a constant turns the comparison into a few word compares.
 * @return Index of the slot; its id is @ref ARNM_KEY_MAP_EMPTY_ID when the key is absent.
 */
static inline uint32_t arnm_key_map_probe(
    const arnm_key_map *map, const uint8_t *key, uint32_t hash, uint32_t key_size
) {
  const arnm_key_map_slot *slots = (const arnm_key_map_slot *)(void *)map->slots;
  const uint32_t mask = map->capacity - 1u;
  uint32_t index = hash & mask;
  while (ARNM_KEY_MAP_EMPTY_ID != slots[index].id) {
    if (slots[index].hash == hash &&
        0 == memcmp(arnm_bvec_get(&map->keys, slots[index].id), key, key_size)) {
      break;
    }
    index = (index + 1u) & mask;
  }
  return index;
}

/**
 * @brief Add a key the probe did not find: the cold half of every get_or_insert.
 *
 * Grows the table first when the key would pass the load limit (placing the key again in the
 * new table), copies the key into the key vector, then writes the slot. Public because it is
 * where every allocation of the insert path happens, and because the lookups
 * @ref ARNM_KEY_MAP_DEFINE generates are inline and have to reach it.
 *
 * @param[in,out] map        Map; not NULL, initialized.
 * @param[in]     key        @c key_size bytes; not NULL. Must not be in the map -- unchecked.
 * @param[in]     hash       @ref arnm_key_map_slot_hash() of @p key -- unchecked; a different
 *                           value files the key where no lookup will find it.
 * @param[in]     slot_index The free slot @ref arnm_key_map_probe() stopped at. Ignored while the
 *                           map has no table or when the table grows.
 * @param[out]    out_id     Receives the new id; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS                   Added; @p out_id holds its id.
 * @retval ARNM_ERROR_NULL_POINTER        @p map, @p key or @p out_id is NULL.
 * @retval ARNM_ERROR_INVALID_STATE       @p map never saw an init.
 * @retval ARNM_ERROR_INVALID_PARAM       @p slot_index is outside the table or not free.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW As @ref arnm_key_map_get_or_insert().
 * @retval ARNM_ERROR_OUT_OF_MEMORY       As @ref arnm_key_map_get_or_insert(): nothing added.
 * @whisper The newcomer's name, written in only once the page has room
 */
arnm_result arnm_key_map_insert_absent(
    arnm_key_map *map, const uint8_t *key, uint32_t hash, uint32_t slot_index, uint32_t *out_id
);

/* ********** one key size, fixed at compile time ********** */

/**
 * @brief Generate the key map calls for keys of @p key_size bytes, under the prefix @p name.
 *
 * `name##_get_or_insert()` and `name##_find()` are inline copies of the lookup with the key size
 * a constant -- the reason to use the macro at all. The rest forwards to the library with that
 * size filled in. Answers and result codes are those of the untyped calls, with one more: a map
 * opened for another key size is refused with @ref ARNM_ERROR_INVALID_STATE (and `_find`
 * answers false), which also covers a map never initialized.
 *
 * @code
 * ARNM_KEY_MAP_DEFINE(pubkey_map, 32)
 * arnm_key_map map;
 * pubkey_map_init(&map, 12, &arena);          // fast hash, 4096 keys per bucket
 * pubkey_map_reserve(&map, 50000);
 * uint32_t id;
 * pubkey_map_get_or_insert(&map, public_key, &id, NULL);
 * pubkey_map_free(&map);
 * @endcode
 *
 * @param name     Prefix for the generated functions.
 * @param key_size Key size in bytes, a constant expression; bounds as @ref arnm_key_map_init().
 */
#define ARNM_KEY_MAP_DEFINE(name, key_size)                                                        \
  /** Prepare an empty map for this key size on the fast hash. */                                  \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline arnm_result name##_init(                                 \
      arnm_key_map *map, uint8_t keys_bucket_log2, arnm *allocator                                 \
  ) {                                                                                              \
    return arnm_key_map_init(map, (key_size), keys_bucket_log2, allocator);                        \
  }                                                                                                \
                                                                                                   \
  /** Prepare an empty map for this key size on SipHash-1-3 under a hash key. */                   \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline arnm_result name##_init_keyed(                           \
      arnm_key_map *map, uint8_t keys_bucket_log2, uint64_t hash_k0, uint64_t hash_k1,             \
      arnm *allocator                                                                              \
  ) {                                                                                              \
    return arnm_key_map_init_keyed(                                                                \
        map, (key_size), keys_bucket_log2, hash_k0, hash_k1, allocator                             \
    );                                                                                             \
  }                                                                                                \
                                                                                                   \
  /** Make room for @p key_count keys. */                                                          \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline arnm_result name##_reserve(                              \
      arnm_key_map *map, uint32_t key_count                                                        \
  ) {                                                                                              \
    return arnm_key_map_reserve(map, key_count);                                                   \
  }                                                                                                \
                                                                                                   \
  /** The key's id, giving it the next one if it is new -- inline, key size fixed. */              \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline arnm_result name##_get_or_insert(                        \
      arnm_key_map *map, const uint8_t *key, uint32_t *out_id, bool *out_inserted                  \
  ) {                                                                                              \
    if (!map || !key || !out_id) { return ARNM_ERROR_NULL_POINTER; }                               \
    if ((key_size) != map->keys.element_size) { return ARNM_ERROR_INVALID_STATE; }                 \
    const uint32_t hash = arnm_key_map_slot_hash(map, key, (key_size));                            \
    uint32_t index = 0;                                                                            \
    if (map->capacity) {                                                                           \
      index = arnm_key_map_probe(map, key, hash, (key_size));                                      \
      const uint32_t id = ((const arnm_key_map_slot *)(void *)map->slots)[index].id;               \
      if (ARNM_KEY_MAP_EMPTY_ID != id) {                                                           \
        *out_id = id;                                                                              \
        if (out_inserted) { *out_inserted = false; }                                               \
        return ARNM_SUCCESS;                                                                       \
      }                                                                                            \
    }                                                                                              \
    const arnm_result result = arnm_key_map_insert_absent(map, key, hash, index, out_id);          \
    if (ARNM_SUCCESS == result && out_inserted) { *out_inserted = true; }                          \
    return result;                                                                                 \
  }                                                                                                \
                                                                                                   \
  /** The key's id if the map holds it -- inline, key size fixed. */                               \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline bool name##_find(                                        \
      const arnm_key_map *map, const uint8_t *key, uint32_t *out_id                                \
  ) {                                                                                              \
    if (!map || !key || !map->capacity || (key_size) != map->keys.element_size) { return false; }  \
    const uint32_t index =                                                                         \
        arnm_key_map_probe(map, key, arnm_key_map_slot_hash(map, key, (key_size)), (key_size));    \
    const uint32_t id = ((const arnm_key_map_slot *)(void *)map->slots)[index].id;                 \
    if (ARNM_KEY_MAP_EMPTY_ID == id) { return false; }                                             \
    if (out_id) { *out_id = id; }                                                                  \
    return true;                                                                                   \
  }                                                                                                \
                                                                                                   \
  /** Keys held. */                                                                                \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline uint32_t name##_size(const arnm_key_map *map) {          \
    return arnm_key_map_size(map);                                                                 \
  }                                                                                                \
                                                                                                   \
  /** The key stored under @p id; unchecked. */                                                    \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline const uint8_t *name##_key(                               \
      const arnm_key_map *map, uint32_t id                                                         \
  ) {                                                                                              \
    return arnm_key_map_key(map, id);                                                              \
  }                                                                                                \
                                                                                                   \
  /** Forget every key, keep the memory. */                                                        \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline void name##_clear(arnm_key_map *map) {                   \
    arnm_key_map_clear(map);                                                                       \
  }                                                                                                \
                                                                                                   \
  /** Give the memory back, keep the descriptor. */                                                \
  ARNM_KEY_MAP_MAYBE_UNUSED static inline void name##_free(arnm_key_map *map) {                    \
    arnm_key_map_free(map);                                                                        \
  }

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_KEY_MAP_H
