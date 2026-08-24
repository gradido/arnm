#ifndef ARNM_JSON_MEMORY_H
#define ARNM_JSON_MEMORY_H

/*
 * The one seam where yyjson's memory meets arnm's, shared by the reader and the writer.
 *
 * Not installed and not part of the public interface: it names yyjson, which no header under
 * include/arnm ever does. It lives here rather than in either .c file because both of them
 * need the same bridge, and a bridge built twice is a bridge that drifts.
 *
 * yyjson asks for memory through three function pointers and hands the size back on realloc but
 * not on free; arnm needs that size at every release. There are two ways to close that gap, and
 * which one fits depends on who ends up owning the block.
 *
 *   json_alc  -- a header of eight bytes ahead of every block, recording what was reserved.
 *                For memory yyjson allocates and frees itself: documents, string pools, value
 *                buffers. Eight and not four: arnm hands out eight byte aligned blocks and
 *                charges sizes in multiples of eight, so a header of that width leaves the
 *                payload exactly where the allocator put it.
 *   json_buffer_alc -- no header at all; the size is kept beside the allocator, because there
 *                is only ever one such block alive. For the written JSON text, which leaves
 *                through the caller's hands and has to be a plain arnm allocation they can free
 *                themselves.
 *
 * Sizes are not stored anywhere else in arnm, and they are not stored here either in the sense
 * the memory contract means -- this records what a third party interface refuses to carry, at
 * the one seam where it crosses.
 */

#include "arnm/memory.h"
#include "arnm/result.h"

#include "yyjson.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/**
 * @brief The eight bytes ahead of every block yyjson is handed.
 *
 * @c total_size counts the header itself, because that is the number arnm_free() and
 * arnm_realloc() have to be told -- the payload size alone would move an arena's index by too
 * little and hand the same bytes out twice.
 */
typedef struct json_block_header {
  uint32_t total_size; /**< Bytes reserved through arnm_alloc(), this header included. */
  uint32_t padding;    /**< Never read. Keeps the payload on the eight byte grid. */
} json_block_header;

static_assert(
    sizeof(json_block_header) == 8, "the block header has to be exactly one alignment step wide"
);

/** @brief Largest payload that still leaves room for the header inside a uint32_t. */
#define JSON_BLOCK_MAX_PAYLOAD (ARNM_MAX_ALLOC_SIZE - (uint32_t)sizeof(json_block_header))

/**
 * @brief What the headered hooks below carry as their context.
 *
 * Bound into a `yyjson_alc` by json_alc_bind(), which is why a reader or a writer may not be
 * moved once it holds a document: the document keeps its own copy of that alc and calls back
 * through it.
 */
typedef struct json_alc_context {
  arnm *allocator;       /**< Where blocks come from; NULL is the host. */
  bool arena_kept_bytes; /**< An arena could not take a block back since this was last cleared. */
} json_alc_context;

static inline void *json_block_alloc(void *context, size_t size) {
  json_alc_context *state = (json_alc_context *)context;
  if (!state) { return NULL; }
  if (0 == size || size > (size_t)JSON_BLOCK_MAX_PAYLOAD) { return NULL; }

  const uint32_t total = (uint32_t)size + (uint32_t)sizeof(json_block_header);
  uint8_t *block = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&block, total, state->allocator)) { return NULL; }

  json_block_header *header = (json_block_header *)(void *)block;
  header->total_size = total;
  header->padding = 0;
  return block + sizeof(json_block_header);
}

static inline void *json_block_realloc(void *context, void *pointer, size_t old_size, size_t size) {
  // yyjson always tells us the old payload size, and the header beside the block says the same
  // thing including its own width. The header is the one that is used -- it is what arnm was
  // told at reservation time, and the two can never disagree without the block being foreign.
  (void)old_size;

  if (!pointer) { return json_block_alloc(context, size); }
  json_alc_context *state = (json_alc_context *)context;
  if (!state) { return NULL; }
  if (0 == size || size > (size_t)JSON_BLOCK_MAX_PAYLOAD) { return NULL; }

  uint8_t *block = (uint8_t *)pointer - sizeof(json_block_header);
  uint8_t *before = block;
  const uint32_t reserved = ((const json_block_header *)(const void *)block)->total_size;
  const uint32_t total = (uint32_t)size + (uint32_t)sizeof(json_block_header);

  const arnm_result result = arnm_realloc(&block, reserved, total, state->allocator);
  if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) { return NULL; }

  // The recorded size follows the allocation and not the request. On a buried shrink an arena
  // changes nothing at all and still holds the original reservation; writing the smaller number
  // there would strand the block for good, because a size that does not match the reservation
  // never matches the arena tail again. Same reasoning as arnm_memory_block_realloc().
  if (ARNM_SUCCESS == result || before != block) {
    ((json_block_header *)(void *)block)->total_size = total;
  }
  return block + sizeof(json_block_header);
}

static inline void json_block_dispose(void *context, void *pointer) {
  if (!pointer) { return; }
  json_alc_context *state = (json_alc_context *)context;
  if (!state) { return; }

  uint8_t *block = (uint8_t *)pointer - sizeof(json_block_header);
  const uint32_t reserved = ((const json_block_header *)(const void *)block)->total_size;

  // The warning is neither success nor failure and has to reach the caller, but yyjson has no
  // way to carry it -- so it is caught here and answered by whoever let the document go.
  if (ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == arnm_free(block, reserved, state->allocator)) {
    state->arena_kept_bytes = true;
  }
}

/** @brief Point @p alc at the headered hooks, with @p context behind them. */
static inline void json_alc_bind(yyjson_alc *alc, json_alc_context *context) {
  alc->malloc = json_block_alloc;
  alc->realloc = json_block_realloc;
  alc->free = json_block_dispose;
  alc->ctx = context;
}

/**
 * @brief What the headerless hooks below carry as their context.
 *
 * One block at a time and its size beside it. That is enough for an output buffer: yyjson
 * allocates exactly one, grows it in place, and hands it over -- and what it hands over is then
 * a plain arnm allocation, freed by the caller with @ref arnm_free() and nothing of ours in
 * front of it.
 */
typedef struct json_buffer_context {
  arnm *allocator; /**< Where the block comes from; NULL is the host. */
  uint32_t size;   /**< Bytes reserved for the one block, or 0 while there is none. */
} json_buffer_context;

static inline void *json_buffer_alloc(void *context, size_t size) {
  json_buffer_context *state = (json_buffer_context *)context;
  if (!state) { return NULL; }
  if (0 == size || size > (size_t)ARNM_MAX_ALLOC_SIZE) { return NULL; }

  uint8_t *block = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&block, (uint32_t)size, state->allocator)) { return NULL; }
  state->size = (uint32_t)size;
  return block;
}

static inline void *json_buffer_realloc(
    void *context, void *pointer, size_t old_size, size_t size
) {
  (void)old_size;
  if (!pointer) { return json_buffer_alloc(context, size); }
  json_buffer_context *state = (json_buffer_context *)context;
  if (!state) { return NULL; }
  if (0 == size || size > (size_t)ARNM_MAX_ALLOC_SIZE) { return NULL; }

  uint8_t *block = (uint8_t *)pointer;
  uint8_t *before = block;
  const arnm_result result = arnm_realloc(&block, state->size, (uint32_t)size, state->allocator);
  if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) { return NULL; }

  if (ARNM_SUCCESS == result || before != block) { state->size = (uint32_t)size; }
  return block;
}

static inline void json_buffer_dispose(void *context, void *pointer) {
  json_buffer_context *state = (json_buffer_context *)context;
  if (!pointer || !state) { return; }
  (void)arnm_free((uint8_t *)pointer, state->size, state->allocator);
  state->size = 0;
}

/** @brief Point @p alc at the headerless hooks, with @p context behind them. */
static inline void json_buffer_bind(yyjson_alc *alc, json_buffer_context *context) {
  alc->malloc = json_buffer_alloc;
  alc->realloc = json_buffer_realloc;
  alc->free = json_buffer_dispose;
  alc->ctx = context;
}

#endif // ARNM_JSON_MEMORY_H
