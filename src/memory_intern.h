#ifndef ARNM_MEMORY_INTERN_H
#define ARNM_MEMORY_INTERN_H

/*
 * The layout behind the opaque `arnm`, shared by the implementation and by the tests that
 * check it. Not installed and not part of the public interface: consumers see the sized
 * opaque struct in arnm/memory.h and nothing of what is below. Kept in one place rather than
 * mirrored into the tests, so a field cannot move without the tests moving with it.
 */

#include "arnm/bucket_vector.h"
#include "arnm/memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum arnm_alloc_type {
  ARNM_ALLOC_TYPE_DEFAULT = 0,
  ARNM_ALLOC_TYPE_ARENA_OWNED,
  ARNM_ALLOC_TYPE_ARENA_EXTERNAL,
  ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC,
  ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED
} arnm_alloc_type;

static_assert(
    ARNM_ALLOC_TYPE_ARENA_OWNED + 1 == ARNM_ALLOC_TYPE_ARENA_EXTERNAL &&
        ARNM_ALLOC_TYPE_ARENA_EXTERNAL + 1 == ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC &&
        ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC + 1 == ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED,
    "arnm_alloc_type arena values must be contiguous"
);

/** @brief What a chain holds: its arenas, the shape it opens them with, and where to look. */
typedef struct arnm_multi_arena {
  arnm_bvec arenas;         /**< Every arena, oldest first. Never reordered. */
  uint32_t arena_capacity;  /**< Bytes a regular arena reserves; 0 means the default. */
  uint32_t arena_max_count; /**< Max arena count, 0 for unlimited */
  uint32_t full_remaining;  /**< Remainder that counts as used up; 0 means the default. */
  uint32_t first_open;      /**< Earliest arena that may still have room; only walks forward. */
} arnm_multi_arena;

/**
 * @brief The layout behind the opaque @ref arnm -- one of two shapes, told apart by @c
 *        allocation_type.
 *
 * A single arena carries its block and the index into it; a chain carries only a pointer to
 * its @ref arnm_multi_arena. They share storage, so nothing but @c allocation_type says which
 * one is present.
 */
typedef struct arnm_intern {
  union {
    struct {
      uint8_t *data;       /**< Base of the arena (owned or external), 8 byte aligned. */
      uint32_t last_index; /**< Next free offset from @p data. */
      uint32_t capacity;   /**< Bytes available in the arena, rounded up to 8. */
      uint32_t out_of_memory_capacity; /**< Accumulated overflow since last reset, saturating. */
    };
    arnm_multi_arena *multi_arena;
  };

  uint8_t allocation_type; /**< arnm_alloc_type, one byte is enough. */
} arnm_intern;

static_assert(sizeof(arnm) == sizeof(arnm_intern), "arnm and arnm_intern need to be the same size");

// true implies memory != NULL, so callers can skip their own null check
static inline bool is_arena(const arnm_intern *memory) {
  if (!memory) return false;
  const arnm_alloc_type type = (arnm_alloc_type)memory->allocation_type;
  return type >= ARNM_ALLOC_TYPE_ARENA_OWNED && type <= ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED;
}

static inline bool is_single_arena(const arnm_intern *memory) {
  if (!memory) return false;
  const arnm_alloc_type type = (arnm_alloc_type)memory->allocation_type;
  return ARNM_ALLOC_TYPE_ARENA_OWNED == type || ARNM_ALLOC_TYPE_ARENA_EXTERNAL == type;
}

static inline bool is_multi_arena(const arnm_intern *memory) {
  if (!memory || !memory->multi_arena) return false;
  const arnm_alloc_type type = (arnm_alloc_type)memory->allocation_type;
  return ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC == type || ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED == type;
}

/**
 * Has an arena's remainder fallen to where its chain writes it off?
 *
 * The only place the full threshold is ever compared. Four callers turn on it -- the scan skips
 * an arena that is used up, _measure counts the ones that are not, _reopen pulls the marker
 * back only onto one that is not, and _options_validate refuses a threshold that would write a
 * fresh arena off before it served anything. They describe one rule from four sides, and a
 * strict < in any single one of them puts that caller an alignment step out of step with the
 * other three. That is how the boundary drifted apart once already.
 *
 * <= and not <: the threshold is the last remainder that still counts as used up. Naming the
 * capacity as the threshold therefore writes off an arena that has not been touched, which is
 * exactly the case _options_validate is there to refuse.
 *
 * @param remaining      Bytes still to be had from the arena.
 * @param full_remaining The chain's threshold, already resolved -- 0 means 0 here, not the
 *                       default. The defaults are filled in before a chain is built.
 */
static inline bool arnm_arena_is_used_up(uint32_t remaining, uint32_t full_remaining) {
  return remaining <= full_remaining;
}

/** The layout view of a public handle. Whether it is an arena at all is is_arena()'s answer. */
static inline arnm_intern *arnm_intern_of(arnm *memory) {
  return (arnm_intern *)memory;
}

static inline const arnm_intern *arnm_intern_of_const(const arnm *memory) {
  return (const arnm_intern *)memory;
}

/** Shorthand for the tests, which reach for a field rather than for the struct. */
#define ARNM_INTERN(m) (arnm_intern_of((arnm *)(m)))

#ifdef __cplusplus
}
#endif

#endif // ARNM_MEMORY_INTERN_H
