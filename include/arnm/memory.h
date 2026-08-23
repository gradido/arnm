#ifndef ARNM_MEMORY_H
#define ARNM_MEMORY_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

// make sure size for allocation is 8-Byte aligned
//
// Written as /8*8 rather than the usual & ~7 so the operand keeps its own type. `~7` is an int,
// which turns every use into a signed-to-unsigned conversion, and the obvious repair `~7u` is
// worse than what it replaces: it is 32 bits wide, so a 64-bit operand loses its upper half --
// ARNM_ALIGN8((size_t)0x100000007) would answer 8. Callers pass uint32_t, size_t and uintptr_t,
// and fixed_arena_pool.c needs it inside a static_assert, so it has to stay type-generic and a
// constant expression -- which rules out an inline function. gcc and clang both still emit
// lea/and for it, the same two instructions as the mask; nothing here costs a division.
#define ARNM_ALIGN8(x) ((((x) + 7u) / 8u) * 8u)

// max allocations size, because uint32_t is used
#define ARNM_MAX_ALLOC_SIZE (UINT32_MAX - 7u)

// fast track for check for max alloc size and align in one step
// \return 0 if size exceed max allocation size, else return aligned size
static inline uint32_t arnm_align8_u32(uint32_t size) {
  if (size > ARNM_MAX_ALLOC_SIZE) { return 0; }
  return ARNM_ALIGN8(size);
}

// Sized opaque struct
typedef struct arnm {
  uint8_t bytes[32];
} arnm;

// ********** manage memory allocator themself *******************
//
// create default allocator which can be placed also in another allocator or usage of single
// allocation strategy like malloc if allocator is NULL direct usage of this is the same as use NULL
// for allocator
arnm *arnm_create(arnm *allocator);

void arnm_reset(arnm *memory);

void arnm_release(arnm *memory);

arnm_result arnm_destroy(arnm *memory, arnm *allocator);


// ********** manage memory allocations with data ptr and size explicit *******************

arnm_result arnm_alloc(uint8_t **buffer, uint32_t size, arnm *memory);

arnm_result arnm_realloc(uint8_t **buffer, uint32_t old_size, uint32_t new_size, arnm *memory);

arnm_result arnm_clone(uint8_t **dst_buffer, const uint8_t *src, uint32_t size, arnm *memory);

arnm_result arnm_free(uint8_t *buffer, uint32_t size, arnm *memory);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_MEMORY_H
