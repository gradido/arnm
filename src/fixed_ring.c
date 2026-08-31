#include "arnm/fixed_ring.h"

#include "arnm/memory.h"
#include "arnm/result.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * One allocation holds the whole ring:
 *
 *   [ slot 0 ][ slot 1 ] ... [ slot capacity-1 ]
 *
 * and the queue is the run of `size` slots that begins at `head` and wraps once at the end.
 * The back is not stored, because `head + size` already names it and a stored tail index would
 * leave a full ring and an empty one looking alike -- the ambiguity that costs either a spare
 * slot or a flag everywhere.
 *
 * Every index step is an addition and a comparison rather than a modulo, which is what lets the
 * capacity be exactly what the caller asked for: head is below the capacity and an index into
 * the queue is at most the capacity, so their sum passes the end at most once and one
 * subtraction brings it back. A power of two capacity would buy a mask instead of that
 * comparison and cost a queue of 1000 the 24 slots up to 1024, reserved for the whole run of
 * the process.
 */

/** Bytes the single block occupies. Recomputed rather than stored; the inputs never change. */
static inline uint32_t block_bytes(const arnm_fixed_ring *ring) {
  return ring->capacity * (uint32_t)ring->element_size;
}

/** The state a ring is in before init and after free: holding nothing, promising nothing. */
static void forget_everything(arnm_fixed_ring *ring) {
  ring->allocator = NULL;
  ring->slots = NULL;
  ring->capacity = 0;
  ring->head = 0;
  ring->size = 0;
  ring->element_size = 0;
}

// ********** manage the ring itself *******************

arnm_result arnm_fixed_ring_init(
    arnm_fixed_ring *ring, uint32_t capacity, size_t element_size, arnm *allocator
) {
  if (!ring) { return ARNM_ERROR_NULL_POINTER; }
  if (!capacity || !element_size || element_size > UINT16_MAX) { return ARNM_ERROR_INVALID_PARAM; }
  // asked as a division so that nothing has to be computed that could already have wrapped
  if (capacity > ARNM_MAX_ALLOC_SIZE / (uint32_t)element_size) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }

  const uint32_t bytes = capacity * (uint32_t)element_size;
  uint8_t *slots = NULL;
  const arnm_result result = arnm_alloc(&slots, bytes, allocator);
  if (ARNM_SUCCESS != result) { return result; }

  // written only now, so a refused allocation leaves whatever the caller had
  ring->allocator = allocator;
  ring->slots = slots;
  ring->capacity = capacity;
  ring->head = 0;
  ring->size = 0;
  ring->element_size = (uint16_t)element_size; // bounded by the UINT16_MAX check above
  return ARNM_SUCCESS;
}

arnm_result arnm_fixed_ring_free(arnm_fixed_ring *ring) {
  if (!ring) { return ARNM_ERROR_NULL_POINTER; }
  if (!ring->slots) {
    forget_everything(ring);
    return ARNM_SUCCESS;
  }

  // read out before the descriptor is emptied: the size is recomputed from what the ring holds
  // and the allocator is the one init was handed, so neither can be the caller's to remember
  uint8_t *block = ring->slots;
  const uint32_t bytes = block_bytes(ring);
  arnm *allocator = ring->allocator;

  // emptied first: whatever the allocator answers, the ring has let go of the block and must
  // not be left pointing at it
  forget_everything(ring);
  return arnm_free(block, bytes, allocator);
}

void arnm_fixed_ring_clear(arnm_fixed_ring *ring) {
  if (!ring) { return; }
  // the block keeps whatever it held; only the two counters say it is no longer the queue
  ring->head = 0;
  ring->size = 0;
}

uint32_t arnm_fixed_ring_reserved(const arnm_fixed_ring *ring) {
  if (!ring || !ring->slots) { return 0; }
  return block_bytes(ring);
}

// ********** what enters and what leaves *******************

arnm_result arnm_fixed_ring_emplace(arnm_fixed_ring *ring, void **out_slot) {
  if (!ring || !out_slot) { return ARNM_ERROR_NULL_POINTER; }
  if (!ring->slots) { return ARNM_ERROR_NOT_INITIALIZED; }
  // the ring is the size it is; this says so rather than pretending the request was wrong, and
  // rather than dropping an element whose cleanup only its owner knows
  if (ring->size == ring->capacity) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }

  // one past the back is the first free slot, and it is inside the block while the ring is not
  // full -- the same wrap the read path walks, asked one step further
  *out_slot = arnm_fixed_ring_get(ring, ring->size);
  ring->size++;
  return ARNM_SUCCESS;
}

arnm_result arnm_fixed_ring_push_ptr(arnm_fixed_ring *ring, const void *value) {
  if (!ring || !value) { return ARNM_ERROR_NULL_POINTER; }

  void *slot = NULL;
  const arnm_result result = arnm_fixed_ring_emplace(ring, &slot);
  if (ARNM_SUCCESS != result) { return result; }

  memcpy(slot, value, ring->element_size);
  return ARNM_SUCCESS;
}

arnm_result arnm_fixed_ring_pop(arnm_fixed_ring *ring) {
  if (!ring) { return ARNM_ERROR_NULL_POINTER; }
  // a ring that was never initialized holds nothing, which is the same answer and the true one
  if (!ring->size) { return ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS; }

  // the vacated slot is not cleared: it is the last one the next pushes will reach, and
  // whatever it holds is overwritten before it is read again
  ring->head++;
  if (ring->head == ring->capacity) { ring->head = 0; }
  ring->size--;
  return ARNM_SUCCESS;
}

arnm_result arnm_fixed_ring_pop_copy(arnm_fixed_ring *ring, void *dst) {
  if (!ring || !dst) { return ARNM_ERROR_NULL_POINTER; }
  if (!ring->size) { return ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS; }

  // copied before the front moves, so nothing here can fail half way
  memcpy(dst, arnm_fixed_ring_get(ring, 0), ring->element_size);
  return arnm_fixed_ring_pop(ring);
}

arnm_result arnm_fixed_ring_copy_to(const arnm_fixed_ring *ring, void *dst, uint32_t dst_capacity) {
  if (!ring || !dst) { return ARNM_ERROR_NULL_POINTER; }
  if (dst_capacity < ring->size) { return ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL; }
  if (!ring->size) { return ARNM_SUCCESS; }

  // from the front to the end of the block, and the remainder from the start of it -- two runs
  // at most, whatever the queue's length, because the wrap happens once
  const uint32_t to_end = ring->capacity - ring->head;
  const uint32_t first_run = ring->size < to_end ? ring->size : to_end;
  const size_t element_size = ring->element_size;

  memcpy(dst, ring->slots + (size_t)ring->head * element_size, (size_t)first_run * element_size);
  if (first_run < ring->size) {
    memcpy(
        (uint8_t *)dst + (size_t)first_run * element_size, ring->slots,
        (size_t)(ring->size - first_run) * element_size
    );
  }
  return ARNM_SUCCESS;
}
