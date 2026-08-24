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

/**
 * @defgroup arnm_memory arnm_memory
 * @brief One handle, four calls, three strategies behind them.
 *
 * Every allocation in arnm goes through @ref arnm and the four calls at the bottom of this
 * file. Which strategy answers them is decided when the handle is made, and callers never
 * branch on it:
 *
 * | handle                          | made by                                                  |
 * what @ref arnm_alloc() does                     |
 * |---------------------------------|----------------------------------------------------------|-------------------------------------------------|
 * | `NULL`, or a zeroed @ref arnm   | nothing, or @ref arnm_create()                           |
 * hands the request to the host (malloc/free)     | | arena                           | @ref
 * arnm_init_arena(), @ref arnm_init_arena_borrow()    | bumps an index inside one fixed block | |
 * chain                           | @ref arnm_create_multi_arena()                           |
 * bumps an index in the first arena with room     |
 *
 * A function that takes an `arnm *` therefore works against all three, and a caller with no
 * opinion passes NULL and gets the host. That is the point of the handle being one type.
 *
 * ### Sizes come back with the pointer
 *
 * @ref arnm_free() and @ref arnm_realloc() are told the size the block was allocated with.
 * Nothing here writes a header next to your memory, so an arena costs exactly what you asked
 * for rounded up to 8 -- and the price is that you keep the size. A wrong one is not detected
 * in arena mode.
 *
 * ### Giving memory back to an arena is best effort
 *
 * An arena releases only from its tail. Freeing the block it handed out last moves the index
 * back; freeing anything buried under a later one cannot, and answers
 * @ref ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED -- the call was carried out, the bytes simply
 * wait for @ref arnm_reset(). That is a warning and not an error -- code that reads every
 * non-zero result as failure will get this wrong.
 *
 * @note Every block an arena hands out is 8 byte aligned, and so is every size it charges for.
 * @note Nothing here is thread safe. One handle belongs to one thread at a time.
 * @{
 */

/**
 * @brief Round @p x up to a multiple of 8 at compile time.
 *
 * The figure an arena actually reserves for a request of @p x bytes. Type generic and usable in
 * a constant expression; `arnm_align8_u32()` is the checked runtime form.
 *
 * @warning No overflow check. Feeding it a value within 7 of its type's maximum wraps.
 */
// Written as /8*8 rather than the usual & ~7 so the operand keeps its own type. `~7` is an int,
// which turns every use into a signed-to-unsigned conversion, and the obvious repair `~7u` is
// worse than what it replaces: it is 32 bits wide, so a 64-bit operand loses its upper half --
// ARNM_ALIGN8((size_t)0x100000007) would answer 8. Callers pass uint32_t, size_t and uintptr_t,
// and fixed_arena_pool.c needs it inside a static_assert, so it has to stay type-generic and a
// constant expression -- which rules out an inline function. gcc and clang both still emit
// lea/and for it, the same two instructions as the mask; nothing here costs a division.
#define ARNM_ALIGN8(x) ((((x) + 7u) / 8u) * 8u)

/**
 * @brief Largest request any call in arnm accepts -- `UINT32_MAX - 7`.
 *
 * One byte more could not be rounded up to 8 inside the uint32_t every size is carried in.
 */
#define ARNM_MAX_ALLOC_SIZE (UINT32_MAX - 7u)

/**
 * @brief Round @p size up to 8, refusing what would wrap.
 *
 * @param[in] size Bytes wanted.
 * @return @p size rounded up to a multiple of 8, or 0 if it exceeds @ref ARNM_MAX_ALLOC_SIZE.
 * @note A @p size of 0 answers 0 as well: "nothing to round" and "cannot be rounded" are the
 *       same answer here, which costs nothing because every caller rejects a size of 0 on its
 *       own terms first.
 * @whisper Every request settles onto the same eight byte grid
 */
static inline uint32_t arnm_align8_u32(uint32_t size) {
  if (size > ARNM_MAX_ALLOC_SIZE) { return 0; }
  return ARNM_ALIGN8(size);
}

/**
 * @brief An allocator, whichever kind. 32 bytes, opaque, yours to place.
 *
 * Sized rather than hidden behind a pointer, so it can live on the stack, in a struct, or in
 * memory another allocator handed out -- and so `sizeof` works without asking arnm for it. The
 * bytes are none of a caller's business; read them through @ref arnm_is_arena(),
 * @ref arnm_is_multi_arena() and @ref arnm_multi_arena_measure().
 *
 * All zeroes is a valid, usable state: the host allocator. Everything else comes from
 * @ref arnm_init_arena(), @ref arnm_init_arena_borrow() or @ref arnm_create_multi_arena().
 */
typedef struct arnm {
  uint8_t bytes[32]; /**< Opaque. Read it through the predicates above, never directly. */
} arnm;

// ********** manage memory allocator themself *******************

/**
 * @brief Carve a handle out of @p allocator and leave it in host mode.
 *
 * For code that wants the allocator struct itself to come from somewhere it controls, rather
 * than being the one allocation that escaped to malloc. The result is zeroed, so it is already
 * usable and passes every request to the host; @ref arnm_init_arena() turns it into an arena.
 *
 * @param[in,out] allocator Where the handle's 32 bytes come from, or NULL for malloc.
 * @return The new handle, or NULL if @p allocator had no room.
 * @note Give it back with @ref arnm_destroy(), naming the same @p allocator.
 * @whisper A vessel shaped from the ground it will stand on
 */
arnm *arnm_create(arnm *allocator);

/**
 * @brief Hand everything back and start over, keeping the memory.
 *
 * An arena moves its index to 0; a chain resets every arena it holds and starts its search at
 * the front again. No memory is returned to the host -- that is the point: the next round of
 * work reuses ground that is already warm. Host mode has nothing to reset.
 *
 * @param[in,out] memory Allocator to empty; NULL is a no-op.
 * @warning Every block ever handed out by @p memory is dangling afterwards.
 * @whisper The ground is swept, not carried away
 */
void arnm_reset(arnm *memory);

/**
 * @brief Give the memory back to the host, keep the handle.
 *
 * An owned arena frees its block; a borrowed one simply lets go, leaving the caller's buffer
 * untouched. A chain releases every arena it opened and its descriptor vector. The handle
 * itself survives and can be initialized again.
 *
 * @param[in,out] memory Allocator to empty out; NULL is a no-op.
 * @warning Every block ever handed out by @p memory is dangling afterwards.
 * @whisper What was borrowed returns, what was owned is let go
 */
void arnm_release(arnm *memory);

/**
 * @brief @ref arnm_release(), then the handle's own 32 bytes.
 *
 * @param[in,out] memory    Handle to dispose of. NULL is @ref ARNM_SUCCESS -- nothing was ever
 *                          handed out, so nothing stayed behind.
 * @param[in,out] allocator The allocator @p memory was carved from, or NULL for malloc.
 * @retval ARNM_SUCCESS Handle released and given back.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Everything @p memory held was released; its
 *                     own bytes were buried in @p allocator and wait for that arena's reset.
 * @warning @p allocator is not remembered from @ref arnm_create() and has to match. Naming a
 *          different arena is caught and costs only the 32 bytes; naming NULL for memory an
 *          arena gave is not caught at all.
 * @whisper Last of all, the vessel itself
 */
arnm_result arnm_destroy(arnm *memory, arnm *allocator);

// ********** manage memory allocations with data ptr and size explicit *******************

/**
 * @brief Allocate @p size bytes.
 *
 * @param[out]    buffer Receives the block; not NULL. Untouched unless the call succeeds.
 * @param[in]     size   Bytes wanted; must be > 0. An arena reserves `ARNM_ALIGN8(size)`.
 * @param[in,out] memory Allocator to draw from, or NULL for the host.
 * @retval ARNM_SUCCESS                   Allocated.
 * @retval ARNM_ERROR_NULL_POINTER        @p buffer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       @p size is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p size exceeds @ref ARNM_MAX_ALLOC_SIZE (arena and
 *                                        chain only; the host is handed the size untouched).
 * @retval ARNM_ERROR_INVALID_STATE       Arena mode without a block -- released, or never
 *                                        initialized.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The host said no, or the arena is full. An arena adds
 *                                        the shortfall to @ref arnm_arena_overflow_total().
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED  A chain capped by `arena_max_count` may open no more.
 * @note The memory is not zeroed and holds whatever the previous tenant left.
 * @whisper Raw earth shaped by the hand of need
 */
arnm_result arnm_alloc(uint8_t **buffer, uint32_t size, arnm *memory);

/**
 * @brief Resize a block, in place where the allocator can.
 *
 * Outside arena mode this is `realloc()`. In arena mode only the block at an arena's tail can
 * move the index, so:
 *
 * | block    | direction | what happens                                    | returns |
 * |----------|-----------|-------------------------------------------------|---------|
 * | tail     | shrink    | index moves back, the bytes are reusable        | SUCCESS |
 * | tail     | grow      | index moves on, the address does not change     | SUCCESS |
 * | tail     | grow, no room in *this* arena, chain only | fresh block elsewhere in the chain,
 * contents copied, old block given back | SUCCESS | | non tail | grow      | fresh block, @p
 * old_size bytes copied, old one abandoned | WARNING | | non tail | shrink    | nothing at all,
 * address and bytes kept          | WARNING |
 *
 * @param[in,out] buffer   Not NULL, but may point to NULL to allocate from scratch. Updated
 *                         when the block moves.
 * @param[in]     old_size The size @p *buffer was allocated with; how the tail is recognized.
 * @param[in]     new_size Bytes wanted, or 0 to release through @ref arnm_free().
 * @param[in,out] memory   The allocator the block came from, or NULL for the host.
 * @retval ARNM_SUCCESS                   Resized, or the sizes were already equal.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Per the table; on @p new_size 0 it means the
 *                                        block was not released at all.
 * @retval ARNM_ERROR_NULL_POINTER        @p buffer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM       Chain only: @p *buffer belongs to no arena of it.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW A non-zero size exceeds @ref ARNM_MAX_ALLOC_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       No room to grow.
 * @whisper The vessel widens or narrows, the water within untouched
 */
arnm_result arnm_realloc(uint8_t **buffer, uint32_t old_size, uint32_t new_size, arnm *memory);

/**
 * @brief @ref arnm_alloc() plus a copy of @p size bytes from @p src.
 *
 * @param[out]    dst_buffer Receives the copy; not NULL. Untouched unless the call succeeds.
 * @param[in]     src        Bytes to copy; not NULL. Must hold at least @p size of them.
 * @param[in]     size       Bytes to copy; must be > 0.
 * @param[in,out] memory     Allocator to draw from, or NULL for the host.
 * @retval ARNM_SUCCESS Copied.
 * @retval ARNM_ERROR_NULL_POINTER @p dst_buffer or @p src is NULL.
 * @return Otherwise whatever @ref arnm_alloc() answered.
 * @note Copies @p size, not what an arena reserved for it -- the padding stays untouched.
 * @whisper The same shape, set down twice
 */
arnm_result arnm_clone(uint8_t **dst_buffer, const uint8_t *src, uint32_t size, arnm *memory);

/**
 * @brief Release a block.
 *
 * Outside arena mode this is `free()`. An arena takes back only the block at its tail; see
 * "Giving memory back to an arena is best effort" above.
 *
 * @param[in,out] buffer Block to release. NULL is a no-op.
 * @param[in]     size   The size @p buffer was allocated with. 0 releases nothing.
 * @param[in,out] memory The allocator @p buffer came from, or NULL for the host.
 * @retval ARNM_SUCCESS                   Released, and an arena moved its index back.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED @p buffer is buried in its arena, or
 *                                        @p size is 0. The bytes come back on
 *                                        @ref arnm_reset().
 * @retval ARNM_ERROR_INVALID_PARAM       Chain only: @p buffer belongs to no arena of it. A
 *                                        single arena cannot tell and does not try.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p size exceeds @ref ARNM_MAX_ALLOC_SIZE.
 * @warning Naming the wrong @p size in arena mode moves the index by that amount. Nothing
 *          checks it, and the next allocation overlaps a block still in use.
 * @whisper Returned to the ground it was taken from, if the ground can still take it
 */
arnm_result arnm_free(uint8_t *buffer, uint32_t size, arnm *memory);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_MEMORY_H
