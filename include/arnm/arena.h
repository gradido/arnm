#ifndef ARNM_ARENA_MEMORY_H
#define ARNM_ARENA_MEMORY_H

#include <stdint.h>

#include "memory.h"
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

arnm_result arnm_init_arena(arnm *memory, uint32_t capacity);

arnm_result arnm_init_arena_borrow(arnm *memory, uint8_t *data, uint32_t capacity);

static inline arnm_result arnm_reinit_arena(arnm *memory, uint32_t capacity) {
  arnm_release(memory);
  return arnm_init_arena(memory, capacity);
}

// true implies memory != NULL, so callers can skip their own null check
bool arnm_is_arena(const arnm *memory);

size_t arnm_arena_overflow_total(const arnm *memory);

#ifdef __cplusplus
}
#endif

#endif // ARNM_ARENA_MEMORY_H
