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

#include "arnm/arena.h"
#include "arnm/memory.h"
#include "arnm/multi_arena.h"
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
  /**
   * A realloc moved or resized a block, so the run below is no longer one accountable stretch.
   * Never set while a document is being built -- yyjson grows a mutable document by chaining
   * new chunks through @c malloc and touches @c realloc only when parsing.
   */
  bool span_broken;
  /**
   * Bytes still held by the current run, each reservation counted as the arena charges for it.
   *
   * Where the run begins is not kept here: the caller that releases it knows, because it is the
   * caller that began it. Keeping it would cost a pointer, and the whole of this context fits
   * beside @c allocator in bytes that would otherwise be padding -- which is why
   * @ref ARNM_JSON_WRITER_SIZE did not have to move for any of this.
   */
  uint32_t span_size;
} json_alc_context;

/**
 * @brief Forget the current run, keeping the allocator.
 *
 * What begins a document, and what follows a release that could not take the whole run back:
 * whatever an arena kept is no longer ours to account for, and the next document starts its own
 * run at its own address.
 */
static inline void json_alc_span_reset(json_alc_context *state) {
  state->span_size = 0;
  state->span_broken = false;
}

/** @brief Prepare a context: an allocator, no run, nothing kept. */
static inline void json_alc_context_init(json_alc_context *state, arnm *allocator) {
  state->allocator = allocator;
  state->arena_kept_bytes = false;
  json_alc_span_reset(state);
}

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

  // The run is counted as the arena charges for it -- ARNM_ALIGN8 per reservation, never once
  // over the sum, since ALIGN8(a) + ALIGN8(b) is not ALIGN8(a + b). Counting too little only
  // costs the one step release below; counting too much would hand back memory that is not
  // ours, so a sum that would not fit gives the run up instead of wrapping.
  const uint32_t charged = ARNM_ALIGN8(total);
  if (charged > UINT32_MAX - state->span_size) {
    state->span_broken = true;
  } else {
    state->span_size += charged;
  }
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

  // A grow that could not happen in place leaves the old reservation buried and adds a new one;
  // a shrink at the tail gives bytes back. Both are accountable in principle and neither is
  // worth the lines: only the reader reallocs, and only the writer releases in one step.
  state->span_broken = true;

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
  const arnm_result result = arnm_free(block, reserved, state->allocator);
  if (ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == result) {
    state->arena_kept_bytes = true;
    return; // the bytes are still held, so the run still covers them
  }
  const uint32_t charged = ARNM_ALIGN8(reserved);
  state->span_size = charged > state->span_size ? 0u : state->span_size - charged;
}

/**
 * @brief Give a whole document back in one arnm_free(), or say that it cannot be done.
 *
 * Everything yyjson took for one mutable document is one run of blocks: it grows a document by
 * chaining chunks through @c malloc and frees them through @c free, and reaches for @c realloc
 * only when parsing. So the run is a first address and a sum, and where the arena still ends at
 * that sum it comes back in a single step instead of one release per chunk.
 *
 * That the whole run really is at the tail is not assumed here -- it is asked, by handing
 * arnm_free() the pair and reading what it answers. Anything else in the arena on top of the
 * run, the written text among them, makes the sum fall short of the tail, and the answer is
 * then the warning rather than a release. Nothing is given back in that case and the caller
 * releases chunk by chunk instead.
 *
 * @param[in,out] state         Context to release the run of; not NULL.
 * @param[in]     first_allocation What the first @c malloc of the run answered -- for a
 *                                 document that is the `yyjson_mut_doc` itself, since
 *                                 `yyjson_mut_doc_new()` is what opens the run. NULL is a
 *                                 refusal, not a crash.
 * @return true when the run came back and the context is ready for the next document. false
 *         leaves everything exactly as it was, including the run itself.
 * @note Host mode is never taken: arnm_free() there frees one block and answers ARNM_SUCCESS
 *       for it, which would leave every other chunk of the run unreleased and unreachable.
 *       A chain is not taken either -- its blocks may sit in two arenas, where a sum measured
 *       against one of them means nothing.
 * @whisper The whole run recedes at once, or not at all
 */
static inline bool json_alc_release_span(json_alc_context *state, void *first_allocation) {
  if (!first_allocation || !state->span_size || state->span_broken) { return false; }
  if (!arnm_is_arena(state->allocator) || arnm_is_multi_arena(state->allocator)) { return false; }

  uint8_t *span_start = (uint8_t *)first_allocation - sizeof(json_block_header);
  if (ARNM_SUCCESS != arnm_free(span_start, state->span_size, state->allocator)) { return false; }
  json_alc_span_reset(state);
  return true;
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
