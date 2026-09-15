#include "arnm/key_map.h"

#include "arnm/bucket_vector.h"
#include "arnm/memory.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/*
 * The table:
 *
 *   [ hash | id ][ hash | id ] ... [ hash | id ]      capacity slots, 8 bytes each
 *
 * The slot layout, the hashes and the probe live in the header, because the lookups
 * ARNM_KEY_MAP_DEFINE generates are inline and read them there. What is here is what they reach
 * only on the way to memory: growing the table and adding a key.
 *
 * The same 32 bits pick the home slot and filter the key comparison, which is what lets a grow
 * place every key again from the slots alone, without reading a single key.
 *
 * No count is stored: the key vector's size is the count, and one number that could disagree
 * with another is one too many. The allocator is not stored either; the key vector keeps it.
 */

#define EMPTY_ID ARNM_KEY_MAP_EMPTY_ID
#define MIN_CAPACITY 8u

typedef arnm_key_map_slot key_map_slot;

static_assert(sizeof(key_map_slot) == 8, "a slot is two uint32_t and nothing else");
static_assert(
    (uint64_t)ARNM_KEY_MAP_MAX_CAPACITY * sizeof(key_map_slot) <= ARNM_MAX_ALLOC_SIZE,
    "the largest table has to fit one allocation"
);

static inline key_map_slot *slots_of(const arnm_key_map *map) {
  return (key_map_slot *)(void *)map->slots;
}

/** Keys a table of @p capacity slots takes before it has to double: three quarters of it. */
static inline uint32_t load_limit(uint32_t capacity) {
  return capacity - capacity / 4u;
}

/**
 * Move every key into a fresh table of @p capacity slots, which must hold them within the load
 * limit. On failure the map is exactly as it was.
 */
static arnm_result rebuild(arnm_key_map *map, uint32_t capacity) {
  const uint32_t bytes = capacity * (uint32_t)sizeof(key_map_slot);
  uint8_t *block = NULL;
  const arnm_result result = arnm_alloc(&block, bytes, map->keys.allocator);
  if (ARNM_SUCCESS != result) { return result; }
  // all bits set is EMPTY_ID in every id; the hash of a free slot is never read
  memset(block, 0xff, bytes);

  const key_map_slot *old_slots = slots_of(map);
  key_map_slot *new_slots = (key_map_slot *)(void *)block;
  const uint32_t mask = capacity - 1u;
  for (uint32_t i = 0; i < map->capacity; ++i) {
    if (EMPTY_ID == old_slots[i].id) { continue; }
    // every key is distinct already: find a free slot, compare nothing
    uint32_t index = old_slots[i].hash & mask;
    while (EMPTY_ID != new_slots[index].id) { index = (index + 1u) & mask; }
    new_slots[index] = old_slots[i];
  }

  if (map->slots) {
    // behind an arena a table that is not at the tail is not taken back. That is the cost the
    // header names and reserve avoids, not a failure of this insert -- the same call the
    // bucket vector makes about its superseded index arrays.
    (void)arnm_free(
        map->slots, map->capacity * (uint32_t)sizeof(key_map_slot), map->keys.allocator
    );
  }
  map->slots = block;
  map->capacity = capacity;
  return ARNM_SUCCESS;
}

/** Smallest table that holds @p key_count keys within the load limit; @p key_count is bounded. */
static uint32_t capacity_for(uint32_t key_count) {
  uint32_t capacity = MIN_CAPACITY;
  while (load_limit(capacity) < key_count) { capacity <<= 1; }
  return capacity;
}

// ********** manage the map *******************

/** Both inits: everything but the hash is the same, and so is what a refusal leaves behind. */
static arnm_result init_map(
    arnm_key_map *map,
    size_t key_size,
    uint8_t keys_bucket_log2,
    arnm_key_map_hash hash,
    uint64_t hash_k0,
    uint64_t hash_k1,
    arnm *allocator
) {
  if (!map) { return ARNM_ERROR_NULL_POINTER; }
  // the bucket vector checks key size and exponent against the same bounds the header names;
  // on refusal it writes nothing, and neither does this
  arnm_bvec keys;
  const arnm_result result = arnm_bvec_init(&keys, keys_bucket_log2, 0, key_size, allocator);
  if (ARNM_SUCCESS != result) { return result; }

  map->slots = NULL;
  map->keys = keys;
  map->hash_k0 = hash_k0;
  map->hash_k1 = hash_k1;
  map->capacity = 0;
  map->hash = (uint8_t)hash;
  return ARNM_SUCCESS;
}

arnm_result arnm_key_map_init(
    arnm_key_map *map, size_t key_size, uint8_t keys_bucket_log2, arnm *allocator
) {
  return init_map(map, key_size, keys_bucket_log2, ARNM_KEY_MAP_HASH_FAST, 0u, 0u, allocator);
}

arnm_result arnm_key_map_init_keyed(
    arnm_key_map *map,
    size_t key_size,
    uint8_t keys_bucket_log2,
    uint64_t hash_k0,
    uint64_t hash_k1,
    arnm *allocator
) {
  return init_map(
      map, key_size, keys_bucket_log2, ARNM_KEY_MAP_HASH_SIPHASH13, hash_k0, hash_k1, allocator
  );
}

arnm_result arnm_key_map_reserve(arnm_key_map *map, uint32_t key_count) {
  if (!map) { return ARNM_ERROR_NULL_POINTER; }
  if (!key_count) { return ARNM_ERROR_INVALID_PARAM; }
  if (!map->keys.element_size) { return ARNM_ERROR_INVALID_STATE; }
  if (key_count > ARNM_KEY_MAP_MAX_KEYS) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  // the keys first: the vector refuses a count it cannot address before it allocates anything,
  // and a table sized for keys that could never be stored would be memory for nothing
  arnm_result result = arnm_bvec_reserve(&map->keys, key_count);
  if (ARNM_SUCCESS != result) { return result; }

  const uint32_t capacity = capacity_for(key_count);
  if (capacity <= map->capacity) { return ARNM_SUCCESS; }
  return rebuild(map, capacity);
}

void arnm_key_map_clear(arnm_key_map *map) {
  if (!map) { return; }
  if (map->slots) { memset(map->slots, 0xff, map->capacity * (uint32_t)sizeof(key_map_slot)); }
  arnm_bvec_clear(&map->keys);
}

void arnm_key_map_free(arnm_key_map *map) {
  if (!map) { return; }
  if (map->slots) {
    // released on the allocator's terms: behind an arena a table below the tail stays until the
    // arena's reset, which is nothing this descriptor could change
    (void)arnm_free(
        map->slots, map->capacity * (uint32_t)sizeof(key_map_slot), map->keys.allocator
    );
  }
  map->slots = NULL;
  map->capacity = 0;
  arnm_bvec_free(&map->keys);
}

// ********** keys *******************

/**
 * The cold half of an insert, for a key known to be absent. Shared by the untyped
 * get_or_insert below and by arnm_key_map_insert_absent(), which checks its arguments first.
 */
static arnm_result insert_absent(
    arnm_key_map *map, const uint8_t *key, uint32_t hash, uint32_t index, uint32_t *out_id
) {
  // the table grows before anything is written, so a refused grow adds nothing
  const uint32_t count = arnm_bvec_size(&map->keys);
  if (count >= load_limit(map->capacity)) {
    if (map->capacity >= ARNM_KEY_MAP_MAX_CAPACITY) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    const arnm_result result = rebuild(map, map->capacity ? map->capacity * 2u : MIN_CAPACITY);
    if (ARNM_SUCCESS != result) { return result; }
    // the key is known to be absent, so the first free slot from its home is its place
    const key_map_slot *slots = slots_of(map);
    const uint32_t mask = map->capacity - 1u;
    index = hash & mask;
    while (EMPTY_ID != slots[index].id) { index = (index + 1u) & mask; }
  }

  // the key goes in before the slot does: a refused bucket must not leave a slot naming an id
  // that has no key behind it
  void *stored = NULL;
  const arnm_result result = arnm_bvec_emplace(&map->keys, &stored);
  if (ARNM_SUCCESS != result) { return result; }
  memcpy(stored, key, map->keys.element_size);

  key_map_slot *slot = &slots_of(map)[index];
  slot->hash = hash;
  slot->id = count;
  *out_id = count;
  return ARNM_SUCCESS;
}

arnm_result arnm_key_map_insert_absent(
    arnm_key_map *map, const uint8_t *key, uint32_t hash, uint32_t slot_index, uint32_t *out_id
) {
  if (!map || !key || !out_id) { return ARNM_ERROR_NULL_POINTER; }
  if (!map->keys.element_size) { return ARNM_ERROR_INVALID_STATE; }
  // the slot is only read when there is a table and it will not grow; then it has to be free
  if (map->capacity && arnm_bvec_size(&map->keys) < load_limit(map->capacity) &&
      (slot_index >= map->capacity || EMPTY_ID != slots_of(map)[slot_index].id)) {
    return ARNM_ERROR_INVALID_PARAM;
  }
  return insert_absent(map, key, hash, slot_index, out_id);
}

arnm_result arnm_key_map_get_or_insert(
    arnm_key_map *map, const uint8_t *key, uint32_t *out_id, bool *out_inserted
) {
  if (!map || !key || !out_id) { return ARNM_ERROR_NULL_POINTER; }
  const uint32_t key_size = map->keys.element_size;
  if (!key_size) { return ARNM_ERROR_INVALID_STATE; }

  const uint32_t hash = arnm_key_map_slot_hash(map, key, key_size);
  uint32_t index = 0;
  if (map->capacity) {
    index = arnm_key_map_probe(map, key, hash, key_size);
    const key_map_slot *found = &slots_of(map)[index];
    if (EMPTY_ID != found->id) {
      *out_id = found->id;
      if (out_inserted) { *out_inserted = false; }
      return ARNM_SUCCESS;
    }
  }
  const arnm_result result = insert_absent(map, key, hash, index, out_id);
  if (ARNM_SUCCESS == result && out_inserted) { *out_inserted = true; }
  return result;
}

bool arnm_key_map_find(const arnm_key_map *map, const uint8_t *key, uint32_t *out_id) {
  if (!map || !key || !map->capacity) { return false; }
  const uint32_t key_size = map->keys.element_size;
  const uint32_t index =
      arnm_key_map_probe(map, key, arnm_key_map_slot_hash(map, key, key_size), key_size);
  const key_map_slot *slot = &slots_of(map)[index];
  if (EMPTY_ID == slot->id) { return false; }
  if (out_id) { *out_id = slot->id; }
  return true;
}
