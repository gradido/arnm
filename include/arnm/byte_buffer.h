#ifndef ARNM_BYTE_BUFFER_H
#define ARNM_BYTE_BUFFER_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arnm/memory.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup arnm_byte_buffer arnm_byte_buffer
 *  @brief One block of bytes, filled from the front, never grown.
 *
 *  A place to put things that are produced one after another and consumed all at once: a run of
 *  JSON documents packed back to back, a log the process writes out at the end of a tick, a
 *  frame assembled from several pieces before it goes to a socket. Every @ref
 *  arnm_byte_buffer_copy() lands where the last one ended, and @ref arnm_byte_buffer_access()
 *  hands the whole run to `write()` or `fwrite()` as the single pointer and length they want.
 *
 *  @code
 *  arnm_byte_buffer log;
 *  arnm_byte_buffer_init(&log, 64 * 1024, memory);
 *
 *  arnm_byte_buffer_copy(&log, text.data, text_length);   // one record
 *  arnm_byte_buffer_copy(&log, "\n", 1);                  // and its separator
 *
 *  const uint8_t *bytes = NULL;
 *  uint32_t length = 0;
 *  if (ARNM_SUCCESS == arnm_byte_buffer_access(&log, &bytes, &length)) { write(fd, bytes, length);
 * } arnm_byte_buffer_clear(&log);                          // the room stays, the content goes
 *  @endcode
 *
 *  ### The room is taken once
 *
 *  @ref arnm_byte_buffer_init() asks for every byte the buffer will ever hold, and nothing after
 *  it asks again. A copy that does not fit is refused with @ref ARNM_ERROR_RESOURCE_EXHAUSTED
 *  and writes nothing at all -- not the part that would have fit, because half a record in a
 *  packed stream is worse than no record, and the reader on the far side cannot tell the two
 *  apart. The peak is therefore the number handed to init, and a producer that outruns its
 *  buffer finds out at the call that did it rather than in the allocator.
 *
 *  This is the whole difference to @ref arnm_bucket_vector, which answers a burst by growing,
 *  and it is deliberate: a buffer that grows while being filled cannot promise that what it
 *  already holds stays at one address, and packing bytes together is the entire point here.
 *
 *  ### Two lengths, and only one of them is the content
 *
 *  @c size is what was asked of the allocator and what @ref arnm_byte_buffer_free() has to give
 *  back; @c last_index is how much of it is written. That is also why the access call does not
 *  answer an @ref arnm_memory_block: a block's @c size means the allocated size everywhere else
 *  in arnm, and handing out one whose size meant the content length would be the same type
 *  saying two different things.
 *
 *  @note Every field is readable; none is yours to write. @c last_index in particular is the
 *  one invariant the copy path leans on (`last_index <= size`) and the one place a wrong value
 *  becomes a write past the end.
 *
 *  @note Nothing here is thread safe. One buffer belongs to one thread at a time.
 *
 *  @whisper Grains settle in the vessel, layer on layer, until it is poured out
 *  @{
 */

/** @brief A block, the size it was allocated with, and how far it is filled.
 *
 *  A zeroed buffer (@c arnm_byte_buffer b = {0};) holds nothing: every copy refuses it with
 *  @ref ARNM_ERROR_NOT_INITIALIZED, which is also the state @ref arnm_byte_buffer_free() leaves
 *  behind. @ref arnm_byte_buffer_init() writes all three fields and reads none, so
 *  uninitialized storage is a valid input as well.
 */
typedef struct arnm_byte_buffer {
  /** Start of the block, or NULL before init and after free. 8 byte aligned in arena mode. */
  uint8_t *data;
  /** Bytes reserved at init -- what the allocator was asked for, and what it is given back. */
  uint32_t size;
  /** Bytes written so far, and where the next copy lands. Never greater than @c size. */
  uint32_t last_index;
} arnm_byte_buffer;

/** @brief Reserve @p size bytes, all of them, now.
 *
 *  @param[out]    buffer    Buffer to initialize; not NULL. Need not be zeroed.
 *  @param[in]     size      Bytes to reserve; must be > 0. This is the ceiling, for good.
 *  @param[in,out] allocator Where the block comes from, or NULL for malloc.
 *  @retval ARNM_SUCCESS            Reserved and empty.
 *  @retval ARNM_ERROR_NULL_POINTER @p buffer is NULL.
 *  @retval Anything arnm_alloc() can return; @p buffer is untouched then.
 *  @note The bytes are not zeroed and hold whatever the previous tenant left. Only the first
 *        @c last_index of them ever mean anything.
 *  @warning Calling this on a buffer that already holds a block leaks it. Use
 *           arnm_byte_buffer_free() first.
 *  @warning @p allocator is not remembered. arnm_byte_buffer_free() has to be handed the same
 *           one, the way every size in arnm comes back at the call that releases it.
 *  @whisper The vessel is shaped once, and its measure is settled from then on
 */
static inline arnm_result arnm_byte_buffer_init(
    arnm_byte_buffer *buffer, uint32_t size, arnm *allocator
) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  uint8_t *data = NULL;
  arnm_result result = arnm_alloc(&data, size, allocator);
  if (ARNM_SUCCESS != result) { return result; }
  buffer->data = data;
  buffer->size = size;
  buffer->last_index = 0;
  return result;
}

/** @brief Give the block back and leave the descriptor empty.
 *
 *  The descriptor is only cleared when the bytes really came back; an arena that could not
 *  reclaim leaves it pointing at storage that stays valid until arnm_reset(), so the buffer is
 *  then neither released nor safe to keep indefinitely.
 *
 *  @param[in,out] buffer    Buffer to release; not NULL. One that holds no block is already
 *                           there and simply answers whatever the allocator says of a NULL
 *                           block -- ARNM_SUCCESS from the host, where free(NULL) is a no-op,
 *                           and the warning below from an arena, where NULL is never the tail.
 *  @param[in,out] allocator The allocator the block came from, or NULL for free().
 *  @retval ARNM_SUCCESS            Released, descriptor zeroed.
 *  @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Not the arena's tail, so nothing came back
 *                                  and the descriptor is unchanged.
 *  @retval ARNM_ERROR_NULL_POINTER @p buffer is NULL.
 *  @whisper The vessel returns to the ground it was carved from
 */
static inline arnm_result arnm_byte_buffer_free(arnm_byte_buffer *buffer, arnm *allocator) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  arnm_result result = arnm_free(buffer->data, buffer->size, allocator);
  // clear only where the bytes truly went back; anything else would forget memory still held
  if (ARNM_SUCCESS == result) {
    buffer->data = NULL;
    buffer->size = 0;
    buffer->last_index = 0;
  }
  return result;
}

/** @brief Forget the content, keep the room.
 *
 *  What the next round writes over. The block itself is untouched, so a buffer that is filled,
 *  written out and cleared once per tick asks the allocator exactly once, at init.
 *
 *  @param[in,out] buffer Buffer to empty; NULL is a no-op.
 *  @warning Everything arnm_byte_buffer_access() handed out before is stale afterwards.
 *  @whisper Poured out, the vessel is as wide as it ever was
 */
static inline void arnm_byte_buffer_clear(arnm_byte_buffer *buffer) {
  if (buffer) { buffer->last_index = 0; }
}

/** @brief Bytes still free, which is what the next copy may be at most.
 *
 *  @param[in] buffer Buffer to measure; may be NULL, and answers 0 then, as does one that was
 *                    never initialized.
 *  @return `size - last_index`.
 *  @whisper How much of the vessel the next handful may fill
 */
static inline uint32_t arnm_byte_buffer_available(const arnm_byte_buffer *buffer) {
  if (!buffer) { return 0; }
  return buffer->size - buffer->last_index;
}

/** @brief Copy @p size bytes to where the last copy ended, and move the mark along.
 *
 *  The one way bytes get in. @p src is read once, here, and nothing of it is remembered
 *  afterwards -- the caller's buffer is free the moment this returns.
 *
 *  @param[in,out] buffer Buffer to append to; not NULL and initialized.
 *  @param[in]     src    Bytes to copy; not NULL, and at least @p size of them.
 *  @param[in]     size   Bytes to copy. 0 copies nothing and moves nothing, as memcpy() does
 *                        at that length -- a field that may be empty needs no `if` around this.
 *  @retval ARNM_SUCCESS                 Copied, @c last_index advanced by @p size.
 *  @retval ARNM_ERROR_NULL_POINTER      @p buffer or @p src is NULL. @p src has to be a valid
 *                                       pointer even where @p size is 0, which is memcpy()'s
 *                                       own requirement at every length.
 *  @retval ARNM_ERROR_NOT_INITIALIZED   @p buffer holds no block.
 *  @retval ARNM_ERROR_RESOURCE_EXHAUSTED @p size is more than arnm_byte_buffer_available().
 *                                       Nothing was written and @c last_index did not move; the
 *                                       host was never asked, so this is not out of memory.
 *  @note @p size is taken at its word and is never read out of @p src. Too large reads past the
 *        source, exactly as memcpy() would.
 *  @whisper A handful poured onto what is already there
 */
static inline arnm_result arnm_byte_buffer_copy(
    arnm_byte_buffer *buffer, const void *src, uint32_t size
) {
  if (!buffer || !src) { return ARNM_ERROR_NULL_POINTER; }
  if (!buffer->data) { return ARNM_ERROR_NOT_INITIALIZED; }
  // subtraction and not last_index + size: the sum can wrap a uint32_t, the difference cannot,
  // because last_index <= size holds from init onwards and this is the only line that moves it
  if (size > buffer->size - buffer->last_index) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  memcpy(buffer->data + buffer->last_index, src, size);
  buffer->last_index += size;
  return ARNM_SUCCESS;
}

/** @brief @ref arnm_byte_buffer_copy() with the checks moved into the debug build.
 *
 *  The same bytes and the same mark, and no answer to read: what the safe call refuses, this one
 *  asserts where assertions are on and does not look at where they are not. Inspired by yyjson's
 *  unsafe_* pair, and here for the same reason -- a caller that already asked
 *  @ref arnm_byte_buffer_available() has taken the branch this would take again.
 *
 *  Ask once for a run of writes rather than once per write; that is where the pair pays:
 *
 *  @code
 *  if (arnm_byte_buffer_available(&log) >= record_length + 1u) {
 *    unsafe_arnm_byte_buffer_copy(&log, record, record_length);
 *    unsafe_arnm_byte_buffer_push(&log, '\n');
 *  }
 *  @endcode
 *
 *  @param[in,out] buffer Buffer to append to; not NULL, initialized, and holding room for
 *                        @p size more bytes.
 *  @param[in]     src    Bytes to copy; at least @p size of them, and not NULL even where
 *                        @p size is 0 -- C requires a valid pointer of memcpy() at every
 *                        length, and this is a thin wrapper over it.
 *  @param[in]     size   Bytes to copy; at most @ref arnm_byte_buffer_available(). 0 copies
 *                        nothing and moves nothing, exactly as in the checked call.
 *  @note The assertions belong to the caller and not to the library. This is an inline function,
 *        so whether they run is decided by NDEBUG in the translation unit that calls it -- a
 *        consumer's debug build checks these even against a release build of arnm, and a
 *        consumer's release build checks nothing even against a debug one.
 *  @warning With NDEBUG not one of those preconditions is looked at. A size past the end writes
 *           past the end, and the buffer being static is what makes that a fixed address rather
 *           than an allocator's business.
 *  @whisper The same handful, poured by a hand that already looked
 */
static inline void unsafe_arnm_byte_buffer_copy(
    arnm_byte_buffer *buffer, const void *src, uint32_t size
) {
  // The mistakes arnm_byte_buffer_copy() answers with a result code, spoken in a build that has
  // no result code to read. A size of 0 is not among them there and is not one here.
  assert(buffer && "unsafe_arnm_byte_buffer_copy: buffer is NULL");
  assert(src && "unsafe_arnm_byte_buffer_copy: src is NULL, which memcpy() forbids at any size");
  assert(buffer->data && "unsafe_arnm_byte_buffer_copy: buffer holds no block");
  assert(
      size <= buffer->size - buffer->last_index &&
      "unsafe_arnm_byte_buffer_copy: more bytes than the buffer has room for"
  );
  memcpy(buffer->data + buffer->last_index, src, size);
  buffer->last_index += size;
}

/** @brief Append one byte, and move the mark along by one.
 *
 *  @ref arnm_byte_buffer_copy() for the case it is called with most often: the separator between
 *  two records, a brace, a quote. Same refusals and same all or nothing, only there is no
 *  pointer to read and no length to trust -- so the branch on the size, the call through memcpy
 *  and the read of the source all fall away, and what is left is a compare and a store.
 *
 *  @param[in,out] buffer Buffer to append to; not NULL and initialized.
 *  @param[in]     value  The byte. A character literal is one: `arnm_byte_buffer_push(&b, '\n')`.
 *  @retval ARNM_SUCCESS                  Written, @c last_index advanced by one.
 *  @retval ARNM_ERROR_NULL_POINTER       @p buffer is NULL.
 *  @retval ARNM_ERROR_NOT_INITIALIZED    @p buffer holds no block.
 *  @retval ARNM_ERROR_RESOURCE_EXHAUSTED The buffer is full. Nothing was written and
 *                                        @c last_index did not move.
 *  @note uint8_t and not char, because this writes a byte and half of them are not a char: with
 *        a signed char parameter -- which is what gcc and clang give it on x86 and ARM -- a
 *        plain `push(&b, 0xFF)` is a narrowing conversion, and gcc says so under -Wconversion
 *        while clang stays quiet. A character literal passes as it always did.
 *  @whisper One grain, laid where the last one came to rest
 */
static inline arnm_result arnm_byte_buffer_push(arnm_byte_buffer *buffer, uint8_t value) {
  if (!buffer) { return ARNM_ERROR_NULL_POINTER; }
  if (!buffer->data) { return ARNM_ERROR_NOT_INITIALIZED; }
  // the subtraction cannot wrap, because last_index <= size holds from init onwards; a full
  // buffer is the one case where it answers 0
  if (buffer->size == buffer->last_index) { return ARNM_ERROR_RESOURCE_EXHAUSTED; }
  buffer->data[buffer->last_index++] = value;
  return ARNM_SUCCESS;
}

/** @brief @ref arnm_byte_buffer_push() with the checks moved into the debug build.
 *
 *  As @ref unsafe_arnm_byte_buffer_copy(), for the single byte -- the separator that follows a
 *  record whose room was measured together with it.
 *
 *  @param[in,out] buffer Buffer to append to; not NULL, initialized, and not full.
 *  @param[in]     value  The byte.
 *  @note The assertions are the calling translation unit's; see @ref
 *        unsafe_arnm_byte_buffer_copy().
 *  @warning With NDEBUG a full buffer is written past rather than answered.
 *  @whisper One grain, laid without looking, where the looking was already done
 */
static inline void unsafe_arnm_byte_buffer_push(arnm_byte_buffer *buffer, uint8_t value) {
  assert(buffer && "unsafe_arnm_byte_buffer_push: buffer is NULL");
  assert(buffer->data && "unsafe_arnm_byte_buffer_push: buffer holds no block");
  assert(buffer->last_index < buffer->size && "unsafe_arnm_byte_buffer_push: the buffer is full");
  buffer->data[buffer->last_index++] = value;
}

/** @brief The start of the content and how much of it there is.
 *
 *  The pair a stream call wants. Both stay valid until the next copy, clear or free.
 *
 *  @param[in]  buffer Buffer to read; not NULL and initialized.
 *  @param[out] data   Receives the start of the block; not NULL.
 *  @param[out] size   Receives the bytes written so far, which is 0 for an empty buffer; not
 *                     NULL. This is the content length, not what was allocated -- @c
 *                     buffer->size is that.
 *  @retval ARNM_SUCCESS               Both written.
 *  @retval ARNM_ERROR_NULL_POINTER    @p buffer, @p data or @p size is NULL.
 *  @retval ARNM_ERROR_NOT_INITIALIZED @p buffer holds no block; the outputs are untouched.
 *  @note An empty buffer is a success and not an error: writing nothing is a thing a caller may
 *        legitimately do, and it costs the caller one `if` either way.
 *  @whisper What was gathered, offered in one hand
 */
static inline arnm_result arnm_byte_buffer_access(
    const arnm_byte_buffer *buffer, const uint8_t **data, uint32_t *size
) {
  if (!buffer || !data || !size) { return ARNM_ERROR_NULL_POINTER; }
  if (!buffer->data) { return ARNM_ERROR_NOT_INITIALIZED; }
  *data = buffer->data;
  *size = buffer->last_index;
  return ARNM_SUCCESS;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_BYTE_BUFFER_H
