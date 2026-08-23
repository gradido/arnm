#include "arnm/memory.h"
#include "arnm/arena.h"
#include "arnm/bucket_vector.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"
#include "memory_intern.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ********** manage memory allocator themself *******************

arnm *arnm_create(arnm *allocator) {
  arnm *memory = NULL;
  // for allocator == NULL, arnm_alloc use default single allocation (like malloc)
  if (ARNM_SUCCESS != arnm_alloc((uint8_t **)&memory, sizeof(arnm), allocator)) { return NULL; }
  // zeroed, so it is in a valid state and can be directly used
  memset(memory, 0, sizeof(arnm));
  return memory;
}

void arnm_reset(arnm *m) {
  if (!m) return;
  arnm_intern *memory = (arnm_intern *)m;
  switch (memory->allocation_type) {
  case ARNM_ALLOC_TYPE_DEFAULT:
    break;
  case ARNM_ALLOC_TYPE_ARENA_OWNED:
  case ARNM_ALLOC_TYPE_ARENA_EXTERNAL:
    memory->last_index = 0;
    memory->out_of_memory_capacity = 0;
    break;
  case ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC:
  case ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED:
    for (uint32_t i = 0; i < arnm_bvec_size(&memory->multi_arena->arenas); i++) {
      arnm_reset((arnm *)arnm_bvec_at(&memory->multi_arena->arenas, i));
    }
    memory->multi_arena->first_open = 0;
    break;
  }
}

void arnm_release(arnm *m) {
  if (!m) return;
  arnm_intern *memory = (arnm_intern *)m;
  // is_arena() would answer true for a chain as well and swallow the branch below, leaving
  // every arena body and the descriptor vector behind
  if (is_single_arena(memory)) {
    // external arenas belong to the caller, default mode holds nothing
    if (memory->data && ARNM_ALLOC_TYPE_ARENA_OWNED == memory->allocation_type) {
      free(memory->data);
      memory->data = NULL;
    }
    memory->capacity = 0;
    arnm_reset(m);
  } else if (is_multi_arena(memory)) {
    arnm_bvec *arenas = &memory->multi_arena->arenas;
    // owned arenas give their buffer back, borrowed ones are simply let go
    for (uint32_t i = 0; i < arnm_bvec_size(arenas); i++) {
      arnm_release((arnm *)arnm_bvec_at(arenas, i));
    }
    // leaves the vector in its empty state with the bookkeeping allocator still attached
    arnm_bvec_free(arenas);
    memory->multi_arena->first_open = 0;
  }
}

arnm_result arnm_destroy(arnm *m, arnm *allocator) {
  // nothing to give back is not a failure; arnm_free would warn here, which would read as
  // "something stayed behind" when nothing was ever handed out
  if (!m) { return ARNM_SUCCESS; }
  arnm_release(m);
  arnm_intern *memory = (arnm_intern *)m;
  if (is_multi_arena(memory)) {
    uint32_t allocation_size = sizeof(arnm) + sizeof(arnm_multi_arena);
    return arnm_free((uint8_t *)memory, allocation_size, allocator);
  } else {
    // whatever the arena it was carved from answers is the caller's to see: the descriptor is
    // gone from their point of view either way, but its bytes may only come back on reset
    return arnm_free((uint8_t *)memory, sizeof(arnm), allocator);
  }
}
// **************** arena functions *******************************************************

static bool is_single_arena_reclaimable(
    const uint8_t *buffer, uint32_t aligned_size, const arnm_intern *memory
) {
  if (is_single_arena(memory) && buffer && aligned_size) {
    return memory->data + memory->last_index - aligned_size == buffer;
  }
  return false;
}

// bytes still to be had from this arena. last_index never passes capacity, so this cannot wrap
static inline uint32_t remaining_bytes(const arnm_intern *memory) {
  if (is_single_arena(memory)) { return memory->capacity - memory->last_index; }
  return 0;
}

// true implies memory != NULL, so callers can skip their own null check
bool arnm_is_arena(const arnm *m) {
  return (is_arena((const arnm_intern *)m));
}
// true implies memory != NULL, so callers can skip their own null check
static inline bool has_out_of_memory_capacity_check(const arnm_intern *memory) {
  return is_single_arena(memory);
}

size_t arnm_arena_overflow_total(const arnm *m) {
  const arnm_intern *memory = (const arnm_intern *)m;
  if (!has_out_of_memory_capacity_check(memory)) { return 0; }
  return memory->out_of_memory_capacity;
}

// True if the request runs past the end. Records the shortfall for
// arnm_arena_overflow_total(), saturating -- a counter that rolls over to a small number
// is worse than one that is capped.
static bool account_capacity_exceeded(uint32_t aligned_size, arnm_intern *memory) {
  if (has_out_of_memory_capacity_check(memory)) {
    // no underflow: last_index never passes capacity
    if (remaining_bytes(memory) < aligned_size) {
      if ((uint64_t)memory->out_of_memory_capacity + (uint64_t)aligned_size > UINT32_MAX) {
        memory->out_of_memory_capacity = UINT32_MAX;
      } else {
        memory->out_of_memory_capacity += aligned_size;
      }
      return true;
    }
  }
  return false;
}

arnm_result arnm_init_arena(arnm *m, uint32_t capacity) {
  if (!m) { return ARNM_ERROR_NULL_POINTER; }
  if (!capacity) { return ARNM_ERROR_INVALID_PARAM; }
  uint32_t aligned_capacity = arnm_align8_u32(capacity);
  if (!aligned_capacity) return ARNM_ERROR_ARITHMETIC_OVERFLOW;

  // allocate before touching *memory, so a failure leaves it exactly as it was
  uint8_t *data = NULL;
  arnm_result result = arnm_alloc(&data, aligned_capacity, NULL);
  if (ARNM_SUCCESS != result) { return result; }

  // every field is written, none is read: uninitialized storage is a valid input
  arnm_intern *memory = (arnm_intern *)m;
  memory->data = data;
  memory->last_index = 0;
  memory->capacity = aligned_capacity;
  memory->out_of_memory_capacity = 0;
  memory->allocation_type = ARNM_ALLOC_TYPE_ARENA_OWNED;
  return ARNM_SUCCESS;
}

arnm_result arnm_init_arena_borrow(arnm *m, uint8_t *data, uint32_t capacity) {
  if (!m || !data) return ARNM_ERROR_NULL_POINTER;
  if (!capacity) return ARNM_ERROR_INVALID_PARAM;
  uint32_t aligned_capacity = arnm_align8_u32(capacity);
  if (!aligned_capacity) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  // Rejected, not rounded: an unaligned base would break the "every pointer is 8 byte
  // aligned" invariant, and a rounded up capacity would let the arena bump past the end of
  // a buffer the caller sized exactly.
  if (aligned_capacity != capacity || ARNM_ALIGN8((uintptr_t)data) != (uintptr_t)data) {
    return ARNM_ERROR_INVALID_PARAM;
  }

  // like arnm_init_arena: writes every field, reads none
  arnm_intern *memory = (arnm_intern *)m;
  memory->data = data;
  memory->capacity = aligned_capacity;
  memory->allocation_type = ARNM_ALLOC_TYPE_ARENA_EXTERNAL;
  arnm_reset(m);
  return ARNM_SUCCESS;
}

// ************* multi arena functions ***************************************************

// Append an arena that is already initialized. On failure the descriptor is released, so the
// caller's stack copy never outlives the buffer it points at.
static inline arnm_result multi_arena_push(arnm_multi_arena *m, arnm *arena) {
  arnm_result result = arnm_bvec_push_ptr(&m->arenas, arena);
  if (ARNM_SUCCESS != result) {
    arnm_release(arena);
    return result;
  }
  return ARNM_SUCCESS;
}

static inline bool multi_arena_is_borrowed(arnm_intern *arena) {
  return ARNM_ALLOC_TYPE_ARENA_EXTERNAL == arena->allocation_type;
}

// The arena a buffer was carved from: the one whose block the address falls into, or NULL for
// an address that belongs to none of them -- a foreign pointer, or one from an arena that has
// since been released. Whether the block can actually come back is _reclaimable's business;
// this only names the arena that would have to answer for it.
//
// @p out_index receives the arena's position in the chain, which the callers need to put the
// first_open marker right again after they hand bytes back.
//
// The walk starts at first_open and wraps, because that is where the last allocation came
// from: alloc takes the earliest arena at or after the marker that had room, and every arena
// opened after that sits further back still. Freeing what was just handed out -- the one case
// an arena can reclaim at all -- therefore hits on the first step or close to it. The arenas
// before the marker are reached last, and only for a block that is buried anyway.
static arnm_intern *multi_arena_find_owner(
    const arnm_multi_arena *m, const uint8_t *buffer, uint32_t *out_index
) {
  if (!m || !buffer) { return NULL; }
  const uint32_t count = arnm_bvec_size(&m->arenas);
  if (!count) { return NULL; }
  // first_open is allowed to sit at count, meaning "every arena is closed"; start over then
  const uint32_t start = m->first_open < count ? m->first_open : 0;

  for (uint32_t seen = 0; seen < count; ++seen) {
    uint32_t index = start + seen;
    if (index >= count) { index -= count; }
    arnm_intern *arena = (arnm_intern *)arnm_bvec_get(&m->arenas, index);
    if (!is_single_arena(arena) || !arena->data) { continue; }
    // compared as integers, not as pointers: subtracting two pointers into different objects
    // is not defined, and the whole point here is that we do not yet know they are the same one
    const uintptr_t address = (uintptr_t)buffer;
    const uintptr_t base = (uintptr_t)arena->data;
    if (address >= base && address - base < (uintptr_t)arena->capacity) {
      if (out_index) { *out_index = index; }
      return arena;
    }
  }
  return NULL;
}

// Bytes came back to @p arena. first_open is the earliest arena that may still have room, and
// on its own it only ever walks forward -- so an arena that filled up, was passed by, and then
// gave a block back would stay out of the scan for good. Pull the marker back onto it, unless
// what it holds is under the threshold the chain writes off anyway.
static inline void multi_arena_reopen(
    arnm_multi_arena *m, const arnm_intern *arena, uint32_t index
) {
  if (index < m->first_open && !arnm_arena_is_used_up(remaining_bytes(arena), m->full_remaining)) {
    m->first_open = index;
  }
}

// true implies memory != NULL, so callers can skip their own null check
bool arnm_is_multi_arena(const arnm *m) {
  return is_multi_arena((const arnm_intern *)m);
}

static inline void multi_arena_options_fill_in_defaults(arnm_multi_arena_options *options) {
  if (!options) return;
  options->arena_capacity =
      !options->arena_capacity ? ARNM_MULTI_ARENA_DEFAULT_CAPACITY : options->arena_capacity;
  options->full_remaining =
      !options->full_remaining ? ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING : options->full_remaining;
  options->bucket_size_log2 =
      !options->bucket_size_log2 ? ARNM_MULTI_ARENA_DEFAULT_BUCKET_LOG2 : options->bucket_size_log2;
  options->index_grow_step_size = !options->index_grow_step_size
                                      ? ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE
                                      : options->index_grow_step_size;
}

arnm_result arnm_multi_arena_options_validate(arnm_multi_arena_options *options) {
  if (!options) return ARNM_ERROR_NULL_POINTER;
  // The defaults are filled in here and not by the caller, so that this function answers about
  // the values a chain would really be built on. It used to resolve only the capacity, which
  // let it pass a set _create then refused -- {arena_capacity = 32} reads as a threshold of 0
  // here and as the default 64 there, and 64 writes a 32 byte arena off at birth.
  multi_arena_options_fill_in_defaults(options);
  options->arena_capacity = arnm_align8_u32(options->arena_capacity);
  if (!options->arena_capacity) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  // A threshold that reaches the capacity would call every arena full the moment it opens: the
  // marker would walk past fresh ground and every allocation would get an arena of its own.
  // Asked through the same predicate the scan uses, on the remainder a fresh arena has -- its
  // whole capacity -- so the refusal cannot drift away from what it is meant to prevent.
  if (arnm_arena_is_used_up(options->arena_capacity, options->full_remaining)) {
    return ARNM_ERROR_INVALID_STATE;
  }
  if (options->bucket_size_log2 > 15) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  return ARNM_SUCCESS;
}

arnm *arnm_create_multi_arena(arnm_multi_arena_options *options, arnm *allocator) {
  uint8_t *buffer = NULL;
  // for allocator == NULL, arnm_alloc use default single allocation (like malloc)
  uint32_t allocation_size = sizeof(arnm) + sizeof(arnm_multi_arena);
  if (ARNM_SUCCESS != arnm_alloc((uint8_t **)&buffer, allocation_size, allocator)) return NULL;
  arnm_intern *memory = (arnm_intern *)buffer;
  // _validate resolves the defaults as well, so this is the only place the options are read
  arnm_result result = arnm_multi_arena_options_validate(options);
  if (ARNM_SUCCESS != result) {
    arnm_free(buffer, allocation_size, allocator);
    return NULL;
  }
  memory->allocation_type = options->arena_max_count ? ARNM_ALLOC_TYPE_MULTI_ARENA_FIXED
                                                     : ARNM_ALLOC_TYPE_MULTI_ARENA_DYNAMIC;
  memory->multi_arena = (arnm_multi_arena *)(buffer + sizeof(arnm));

  arnm_multi_arena *m = memory->multi_arena;
  // every field is written, none is read: uninitialized storage is a valid input
  m->arena_capacity = options->arena_capacity;
  m->arena_max_count = options->arena_max_count;
  m->full_remaining = options->full_remaining;
  m->first_open = 0;
  if (ARNM_SUCCESS != arnm_bvec_init(
                          &m->arenas, options->bucket_size_log2, options->index_grow_step_size,
                          sizeof(arnm), allocator
                      )) {
    arnm_free(buffer, allocation_size, allocator);
    return NULL;
  }

  return (arnm *)memory;
}

arnm_result arnm_multi_arena_reserve(arnm *m, uint32_t arena_count) {
  if (!m) { return ARNM_ERROR_NULL_POINTER; }
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_multi_arena(memory)) return ARNM_ERROR_INVALID_STATE;
  if (memory->multi_arena->arena_max_count && arena_count > memory->multi_arena->arena_max_count) {
    return ARNM_ERROR_RESOURCE_EXHAUSTED;
  }
  return arnm_bvec_reserve(&memory->multi_arena->arenas, arena_count);
}

arnm_result arnm_multi_arena_borrow(arnm *m, uint8_t *data, uint32_t capacity) {
  if (!m || !data) { return ARNM_ERROR_NULL_POINTER; }
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_multi_arena(memory)) return ARNM_ERROR_INVALID_STATE;
  if (memory->multi_arena->arena_max_count &&
      memory->multi_arena->arena_max_count < arnm_bvec_size(&memory->multi_arena->arenas) + 1) {
    return ARNM_ERROR_RESOURCE_EXHAUSTED;
  }
  // the borrowed block is checked by the arena itself; nothing is appended if it is unfit
  arnm arena;
  arnm_result result = arnm_init_arena_borrow(&arena, data, capacity);
  if (ARNM_SUCCESS != result) { return result; }

  return multi_arena_push(memory->multi_arena, &arena);
}

arnm_result arnm_multi_arena_shrink(arnm *m) {
  if (!m) { return ARNM_ERROR_NULL_POINTER; }
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_multi_arena(memory)) return ARNM_ERROR_INVALID_STATE;
  arnm_bvec *arenas = &memory->multi_arena->arenas;

  // youngest first, and stop at the first arena that is still holding something or that we do
  // not own. Removing from the middle would renumber the chain for nothing gained.
  arnm_intern *last_arena;
  while ((last_arena = arnm_bvec_back(arenas)) != NULL) {
    if (last_arena->last_index || multi_arena_is_borrowed(last_arena)) { break; }
    arnm_release((arnm *)last_arena);
    (void)arnm_bvec_pop(arenas);
  }

  uint32_t count = arnm_bvec_size(arenas);
  if (memory->multi_arena->first_open > count) { memory->multi_arena->first_open = count; }
  return arnm_bvec_shrink(arenas);
}

uint32_t arnm_multi_arena_arena_count(const arnm *m) {
  const arnm_intern *memory = (const arnm_intern *)m;
  return is_multi_arena(memory) ? arnm_bvec_size(&memory->multi_arena->arenas) : 0;
}

arnm_result arnm_multi_arena_measure(const arnm *m, arnm_multi_arena_stats *out) {
  if (!m || !out) { return ARNM_ERROR_NULL_POINTER; }
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_multi_arena(memory)) return ARNM_ERROR_INVALID_STATE;
  arnm_multi_arena *multi_arena = memory->multi_arena;
  arnm_bvec *arenas = &multi_arena->arenas;

  // uint64 for the sums: a single arena is measured in uint32_t, a chain of them is not
  arnm_multi_arena_stats stats = {0};
  stats.arena_count = arnm_bvec_size(arenas);

  for (uint32_t i = 0; i < stats.arena_count; ++i) {
    arnm_intern *arena = (arnm_intern *)arnm_bvec_at(arenas, i);
    if (is_single_arena(arena)) {
      stats.reserved += arena->capacity;
      stats.used += arena->last_index;
      if (!arnm_arena_is_used_up(remaining_bytes(arena), multi_arena->full_remaining)) {
        stats.open_count++;
      }
    } else {
      return ARNM_ERROR_INVALID_STATE;
    }
  }
  memcpy(out, &stats, sizeof(arnm_multi_arena_stats));
  return ARNM_SUCCESS;
}

// ********** manage memory allocations with data ptr and size explicit *******************
//

static arnm_result multi_arena_alloc(uint8_t **buffer, uint32_t aligned_size, arnm_multi_arena *m);

static arnm_result arena_alloc_aligned(uint8_t **buffer, uint32_t aligned_size, arnm *m) {
  arnm_intern *memory = (arnm_intern *)m;
  assert(is_arena(memory) && "expect arena allocator");

  // can only be happen, if caller access memory directly and mess with the state
  if (!memory->data) { return ARNM_ERROR_INVALID_STATE; }

  // align with 8 Bytes
  if (is_single_arena(memory)) {
    if (account_capacity_exceeded(aligned_size, memory)) return ARNM_ERROR_OUT_OF_MEMORY;

    // last_index is already a multiple of 8, so no padding is needed here
    *buffer = memory->data + memory->last_index;
    memory->last_index += aligned_size;
  } else if (is_multi_arena(memory)) {
    return multi_arena_alloc(buffer, aligned_size, memory->multi_arena);
  }
  return ARNM_SUCCESS;
}

static arnm_result multi_arena_alloc(uint8_t **buffer, uint32_t aligned_size, arnm_multi_arena *m) {
  if (!buffer || !m) { return ARNM_ERROR_NULL_POINTER; }
  if (!aligned_size) { return ARNM_ERROR_INVALID_PARAM; }

  const uint32_t count = arnm_bvec_size(&m->arenas);

  // First fit over the arenas still in the scan. The threshold is asked first and the fit
  // second: an arena that is used up stays written off, even for a request its remainder could
  // still have held. Giving up that tail is what the threshold buys -- without it the scan
  // would keep visiting arenas that only ever serve the smallest requests.
  // arnm_arena_is_used_up() carries where the line sits; see memory_intern.h.
  for (uint32_t i = m->first_open; i < count; ++i) {
    arnm_intern *arena = (arnm_intern *)arnm_bvec_get(&m->arenas, i);
    if (!arena) break;
    const uint32_t remaining = remaining_bytes(arena);
    if (arnm_arena_is_used_up(remaining, m->full_remaining)) {
      // the marker follows only while it is the one leading the scan; an arena closing behind
      // it says nothing about the open ones still in front
      if (i == m->first_open) { m->first_open++; }
      continue;
    }
    // above the threshold but too small for this request: walked over, not written off
    if (remaining >= aligned_size) {
      return arena_alloc_aligned(buffer, aligned_size, (arnm *)arena);
    }
  }

  if (m->arena_max_count && count >= m->arena_max_count) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }

  // nothing had room: open fresh ground. A request larger than a regular arena gets one sized
  // exactly for it -- full on arrival, and out of the way of every later request.
  uint32_t capacity = m->arena_capacity;
  if (capacity < aligned_size) { capacity = aligned_size; }

  arnm arena;
  arnm_result result = arnm_init_arena(&arena, capacity);
  if (ARNM_SUCCESS != result) { return result; }
  result = multi_arena_push(m, &arena);
  if (ARNM_SUCCESS != result) { return result; }

  // the push copied the descriptor; the copy in the vector is the owner from here on
  return arena_alloc_aligned(buffer, aligned_size, arnm_bvec_back(&m->arenas));
}

arnm_result arnm_alloc(uint8_t **buffer, uint32_t size, arnm *m) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  if (!size) { return ARNM_ERROR_INVALID_PARAM; }
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_arena(memory)) {
    uint8_t *allocated = (uint8_t *)malloc(size);
    if (!allocated) { return ARNM_ERROR_OUT_OF_MEMORY; }
    *buffer = allocated;
    return ARNM_SUCCESS;
  }

  // align with 8 Bytes
  uint32_t aligned_size = arnm_align8_u32(size);
  if (!aligned_size) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  return arena_alloc_aligned(buffer, aligned_size, m);
}

arnm_result arnm_realloc(uint8_t **buffer, uint32_t old_size, uint32_t new_size, arnm *m) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  uint32_t new_size_aligned = arnm_align8_u32(new_size);
  uint32_t old_size_aligned = arnm_align8_u32(old_size);
  if ((new_size && !new_size_aligned) || (old_size && !old_size_aligned))
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;

  // release on arnm_free's terms and with its return value, so that freeing through here and
  // calling arnm_free directly cannot drift apart. An empty buffer takes the same route.
  if (!new_size_aligned) {
    arnm_result result = arnm_free(*buffer, old_size_aligned, m);
    if (ARNM_SUCCESS == result) { *buffer = NULL; }
    return result;
  }

  // deliberately below the release check: (0, 0) means free, not "same size, nothing to do"
  if (*buffer && old_size == new_size) { return ARNM_SUCCESS; }

  arnm_intern *memory = (arnm_intern *)m;
  // realloc in non arena mode
  if (!is_arena(memory)) {
    // realloc(NULL, n) is malloc(n), so a fresh buffer works here too
    uint8_t *resized = (uint8_t *)realloc(*buffer, new_size);
    if (!resized) { return ARNM_ERROR_OUT_OF_MEMORY; }

    *buffer = resized;
    return ARNM_SUCCESS;
  }

  arnm_intern *single_arena = memory;
  uint32_t owner_index = 0;
  if (is_multi_arena(memory)) {
    single_arena = multi_arena_find_owner(memory->multi_arena, *buffer, &owner_index);
    // see arnm_free. A NULL buffer is not an address from elsewhere, it is "allocate from
    // scratch", and it keeps its route through the grow path below.
    if (*buffer && !single_arena) { return ARNM_ERROR_INVALID_PARAM; }
  }

  // an arena can only resize in place at its tail
  if (is_single_arena_reclaimable(*buffer, old_size_aligned, single_arena)) {
    // shrink: pull the bump index back over the bytes we no longer want
    if (new_size_aligned < old_size_aligned) {
      single_arena->last_index -= old_size_aligned - new_size_aligned;
      if (is_multi_arena(memory)) {
        multi_arena_reopen(memory->multi_arena, single_arena, owner_index);
      }
      return ARNM_SUCCESS;
    }

    // grow: nothing is allocated behind us, so we can just claim more
    const uint32_t additional = new_size_aligned - old_size_aligned;
    if (remaining_bytes(single_arena) >= additional) {
      single_arena->last_index += additional;
      return ARNM_SUCCESS;
    }

    // The tail cannot stretch any further. A single arena has nothing else to offer and says
    // so, the shortfall going on its overflow counter.
    if (!is_multi_arena(memory)) {
      (void)account_capacity_exceeded(additional, single_arena);
      return ARNM_ERROR_OUT_OF_MEMORY;
    }

    // A chain does have somewhere else to go, so it takes realloc()'s route: fresh block from
    // another arena, contents carried over, old one handed back. Being the tail is what makes
    // that last step exact -- nothing is left behind, so this is a SUCCESS and not the
    // "still in there" warning the buried path below returns.
    uint8_t *moved = NULL;
    arnm_result result = arnm_alloc(&moved, new_size_aligned, m);
    if (ARNM_SUCCESS != result) { return result; }
    // the owning arena holds less than `additional`, so it holds less than new_size_aligned
    // too and cannot have served this request: source and destination never overlap
    memcpy(moved, *buffer, old_size);
    // arenas live in the bucket vector's buckets, which are never moved or renumbered, so
    // single_arena still points at the same descriptor after a fresh arena was appended
    single_arena->last_index -= old_size_aligned;
    // the allocation above may well have walked first_open past this arena; it has room again
    multi_arena_reopen(memory->multi_arena, single_arena, owner_index);
    *buffer = moved;
    return ARNM_SUCCESS;
  }

  // buried: growing has to take a fresh block and abandon the old one until reset
  if (new_size_aligned > old_size_aligned) {
    uint8_t *resized = NULL;
    arnm_result result = arnm_alloc(&resized, new_size_aligned, m);
    if (ARNM_SUCCESS != result) { return result; }

    if (*buffer && old_size) { memcpy(resized, *buffer, old_size); }
    *buffer = resized;
  }

  // Reached by both buried cases: the shrink did nothing, the grow above moved the buffer
  // and left bytes behind. Either way the resize is done and the memory is not back.
  return ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

arnm_result arnm_clone(uint8_t **dst_buffer, const uint8_t *src, uint32_t size, arnm *memory) {
  if (!dst_buffer || !src) { return ARNM_ERROR_NULL_POINTER; }
  if (!size) { return ARNM_ERROR_INVALID_PARAM; }

  arnm_result result = arnm_alloc(dst_buffer, size, memory);
  if (ARNM_SUCCESS != result) { return result; }

  // copy the requested size, not what an arena reserved for it
  memcpy(*dst_buffer, src, size);
  return ARNM_SUCCESS;
}

arnm_result arnm_free(uint8_t *buffer, uint32_t size, arnm *m) {
  arnm_intern *memory = (arnm_intern *)m;
  if (!is_arena(memory)) {
    free(buffer);
    return ARNM_SUCCESS;
  }

  // a size of 0 did not overflow, it is simply nothing to give back: it falls through to the
  // warning below, the same answer a buried block gets, rather than being called arithmetic
  uint32_t aligned_size = arnm_align8_u32(size);
  if (size && !aligned_size) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }

  // see arnm_realloc: the last arena is not the one the block came from
  arnm_intern *single_arena = memory;
  uint32_t owner_index = 0;
  if (is_multi_arena(memory)) {
    single_arena = multi_arena_find_owner(memory->multi_arena, buffer, &owner_index);
    // An address no arena of this chain ever handed out is a mistake, and one the chain can
    // now tell apart from a block of its own that happens to be buried. The two used to give
    // the same answer because only the last arena was ever consulted.
    if (buffer && !single_arena) { return ARNM_ERROR_INVALID_PARAM; }
  }
  if (is_single_arena_reclaimable(buffer, aligned_size, single_arena)) {
    single_arena->last_index -= aligned_size;
    if (is_multi_arena(memory)) {
      multi_arena_reopen(memory->multi_arena, single_arena, owner_index);
    }
    return ARNM_SUCCESS;
  }

  // buried in the arena: the bytes come back on reset, not now
  return ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}
