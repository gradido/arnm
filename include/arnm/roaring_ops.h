#ifndef ARNM_ROARING_OPS_H
#define ARNM_ROARING_OPS_H

#include <stdint.h>

#include "arnm/graded_block_pool.h"
#include "arnm/result.h"
#include "arnm/roaring_bitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_roaring_ops arnm_roaring_ops
 * @brief Building one @ref arnm_roaring_bitmap out of others: and, or, andnot, a range copied.
 *
 * The operations that need memory. Each one writes into an empty set the caller provides, takes
 * every block from the pool named, and leaves that set empty again when anything is refused --
 * the inputs are only read. A result container picks its kind by its size, whatever its inputs
 * were; a result built straight from sparse inputs stays sparse, and one built from containers
 * keeps them, whatever its size.
 *
 * Every range is closed, `[min, max]`, and is applied while the inputs are read: containers
 * outside it are never touched and no range set is ever built. Pass `0` and `UINT32_MAX` for no
 * restriction.
 *
 * Ask rather than build where you can: a count, a page or the newest value of a filter is
 * @ref arnm_roaring_query's answer and costs no block at all.
 *
 * @note Nothing here is thread safe, and the pool belongs to one thread at a time.
 *
 * @whisper Two streams meet and a third is dug for what they carry together
 *
 * @{
 */

// ********** set operations *******************

/**
 * @brief @p out = @p a AND @p b, restricted to `[min, max]`.
 *
 * Only keys present in both inputs are looked at, and only those inside the range.
 *
 * @param[in,out] out  Result; not NULL, empty, neither @p a nor @p b. Empty again on failure.
 * @param[in]     a    First input; not NULL.
 * @param[in]     b    Second input; not NULL.
 * @param[in]     min  Smallest value the result may hold.
 * @param[in]     max  Largest value the result may hold.
 * @param[in,out] pool Pool for the result's blocks; not NULL.
 * @retval ARNM_SUCCESS              @p out holds the result, possibly empty.
 * @retval ARNM_ERROR_NULL_POINTER   A pointer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM  @p out is not empty, or is one of the inputs.
 * @return Any refusal of @ref arnm_graded_block_pool_alloc(); @p out is then empty.
 * @whisper Only what stands in both fields, and only inside the fence
 */
arnm_result arnm_roaring_and(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
);

/**
 * @brief @p out = @p a OR @p b, restricted to `[min, max]`.
 *
 * Parameters and results as @ref arnm_roaring_and(). Can additionally answer
 * @ref ARNM_ERROR_RESOURCE_EXHAUSTED when the union would hold more than UINT32_MAX values.
 */
arnm_result arnm_roaring_or(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
);

/**
 * @brief @p out = @p a AND NOT @p b, restricted to `[min, max]`.
 *
 * Parameters and results as @ref arnm_roaring_and(). Only the keys of @p a inside the range
 * are looked at.
 */
arnm_result arnm_roaring_andnot(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    const arnm_roaring_bitmap *b,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
);

/**
 * @brief @p out = the values of @p a inside `[min, max]`.
 *
 * Parameters and results as @ref arnm_roaring_and(), with @p a as the only input.
 */
arnm_result arnm_roaring_copy_range(
    arnm_roaring_bitmap *out,
    const arnm_roaring_bitmap *a,
    uint32_t min,
    uint32_t max,
    arnm_graded_block_pool *pool
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_ROARING_OPS_H
