#ifndef ARNM_ARENA_FREE_LIST_H
#define ARNM_ARENA_FREE_LIST_H

/*
 * The link an arena carries while it is free, shared by the two pools that lend arenas out.
 * Not installed and not part of the public interface.
 *
 * A free arena has nothing to store, so it stores the link: the first bytes of its buffer hold
 * the address of the next free arena, and a pool holds only the head. Nothing is allocated for
 * the list, and its length cannot drift from the number of arenas. The bytes are overwritten
 * the moment the arena is handed out, which is exactly when the link is no longer needed.
 *
 * Written and read with memcpy, because a uint8_t buffer is not a arnm * and pretending
 * otherwise is the kind of aliasing a sanitizer is right to complain about. The pointer fits:
 * both pools round their capacity up to 8 and refuse 0, and the assert below settles that 8 is
 * enough on this target.
 */

#include "memory_intern.h"

#include "arnm/memory.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static_assert(
    sizeof(arnm *) <= 8, "the free list link has to fit in the smallest arena, which is 8 bytes"
);
static_assert(ARNM_ALIGN8(sizeof(arnm)) == sizeof(arnm), "arnm struct must be 8-Byte aligned");

/* The link goes into the arena's buffer, never into `arena->bytes` -- that is the descriptor
   itself, and its first eight bytes are the pointer to the buffer. Writing the link there
   overwrites exactly what makes the arena an arena, and the first allocation out of it lands
   on whatever address the link happened to hold. Hence the reach into the private layout. */
static inline uint8_t *arnm_arena_link_slot(const arnm *arena) {
  return arnm_intern_of_const(arena)->data;
}

/** Read the link a free arena carries in the first bytes of its buffer. */
static inline arnm *arnm_arena_next_free(const arnm *arena) {
  arnm *next = NULL;
  memcpy(&next, arnm_arena_link_slot(arena), sizeof(arnm *));
  return next;
}

/** Write the link into a free arena's buffer. Only ever called on an arena a pool holds. */
static inline void arnm_arena_set_next_free(arnm *arena, arnm *next) {
  memcpy(arnm_arena_link_slot(arena), &next, sizeof(arnm *));
}

#ifdef __cplusplus
}
#endif

#endif // ARNM_ARENA_FREE_LIST_H
