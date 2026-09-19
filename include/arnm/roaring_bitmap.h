#ifndef ARNM_ROARING_BITMAP_H
#define ARNM_ROARING_BITMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "arnm/graded_block_pool.h"
#include "arnm/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_roaring_bitmap arnm_roaring_bitmap
 * @brief A compressed set of uint32_t values that only grows upwards, in the roaring layout.
 *
 * Made for sets of sequence numbers -- the transactions of one address, of one type -- that are
 * built once in ascending order and then intersected, counted and paged through, most often
 * inside a range of numbers.
 *
 * ### Layout
 *
 * A value splits into a 16 bit key (`value >> 16`) and a 16 bit low part. All values of one key
 * live in one container, and a set is a directory of containers sorted by key. A container is
 * either
 * - an **array**: the low parts sorted, 2 bytes each, for at most
 *   @ref ARNM_ROARING_ARRAY_MAX values; or
 * - a **bitmap**: one bit per possible low part, 8 KiB, for everything above.
 *
 * That is roaring, with two restrictions that the use above allows:
 * - **Append only.** A value below the largest one already in the set is refused. So only the
 *   last container ever changes: an array is appended to and turns into a bitmap once it passes
 *   @ref ARNM_ROARING_ARRAY_MAX values, and never back.
 * - **No run containers.** Numbers of one address or type are scattered; the one set that would
 *   be a single run -- every number of a date range -- is never built, the range is applied
 *   while the others are read instead.
 *
 * Nothing here reads or writes CRoaring's serialised format.
 *
 * ### Sparse sets
 *
 * A set of up to @ref ARNM_ROARING_SPARSE_MAX values has no containers at all: it is one sorted
 * array of the values themselves, 4 bytes each, in a single block. Containers pay off where a
 * key holds many values; a set whose few values are spread over many keys -- the transactions of
 * an ordinary address on a long chain -- would get one container per value or two, some 30
 * bytes each and a block of its own to fetch. As one array its newest values are its last
 * entries. A set built with @ref arnm_roaring_add() turns into containers once it passes the
 * limit. The result of a set operation is sparse where it comes straight from sparse inputs and
 * keeps containers otherwise, whatever its size -- turning a small result into an array would
 * cost every query an allocation and a copy. Which form a set is in never changes an answer.
 *
 * ### Memory
 *
 * Every block -- a container's data and the directory -- comes from an
 * @ref arnm_graded_block_pool passed to each call that allocates, and goes back to it. An array
 * starts at 16 bytes and doubles, the directory starts at one entry and doubles; the pool hands
 * a block that was outgrown to the next set that needs its grade. All sets of one pool share
 * it, and the set itself holds no pointer to it: 24 bytes per set, however many sets there are.
 *
 * The pool must hand out blocks of 8 KiB for bitmap containers, and blocks of 16 bytes per
 * container for the directory -- 1 MiB for the largest set possible. The defaults of
 * @ref arnm_graded_block_pool_options cover both.
 *
 * @warning A set must always meet the same pool. Freeing it through another puts its blocks on
 *          lists they did not come from, and nothing can tell.
 *
 * ### Ranges
 *
 * Every range is closed: `[min, max]`, both ends included, and `min > max` is an empty range.
 * Pass `0` and `UINT32_MAX` for no restriction. A range is applied while a set is read, so the
 * containers outside it are never touched and no range set is ever built.
 *
 * ### Set operations
 *
 * @ref arnm_roaring_and(), @ref arnm_roaring_or(), @ref arnm_roaring_andnot() and
 * @ref arnm_roaring_copy_range() build their result into an empty set the caller provides. A
 * result container picks its kind by its size, whatever its inputs were. On failure the result
 * set is left empty, its blocks back in the pool, and the inputs are untouched.
 *
 * @note A zeroed @ref arnm_roaring_bitmap is an empty set; @ref arnm_roaring_init() is the same
 *       thing spelled out.
 * @note Nothing here is thread safe. Reading one set from several threads is fine as long as no
 *       thread adds to it; the pool belongs to one thread at a time.
 *
 * @whisper Numbers settle in drawers of sixty-five thousand, sparse ones in a list, dense ones
 *          as a field of lights
 *
 * @{
 */

/** @brief Most values an array container holds; one more and it becomes a bitmap. */
#define ARNM_ROARING_ARRAY_MAX 4096u

/** @brief Most values a set keeps as one plain sorted array before it turns into containers. */
#define ARNM_ROARING_SPARSE_MAX 1024u

/** @brief 64 bit words of a bitmap container: one bit for each of the 65536 low parts. */
#define ARNM_ROARING_BITMAP_WORDS 1024u

/** @brief Kind of a container. */
typedef enum arnm_roaring_kind {
  ARNM_ROARING_ARRAY = 0, /**< Sorted uint16_t low parts. */
  ARNM_ROARING_BITMAP = 1 /**< @ref ARNM_ROARING_BITMAP_WORDS words of bits. */
} arnm_roaring_kind;

/** @brief One container: the values of one key. 16 bytes. */
typedef struct arnm_roaring_container {
  uint8_t *data;        /**< uint16_t[] for an array, uint64_t[1024] for a bitmap. */
  uint32_t cardinality; /**< Values held, 1 to 65536. */
  uint16_t key;         /**< The upper 16 bits every value here shares. */
  uint8_t block_log2;   /**< The block @c data lies in is 2^block_log2 bytes. */
  uint8_t kind;         /**< @ref arnm_roaring_kind. */
} arnm_roaring_container;

/**
 * @brief A set. Zeroed is empty. 24 bytes.
 *
 * Two forms, told apart by @c count: with containers it is above 0 and @c containers is the
 * directory; sparse it is 0 and @c values is the sorted array (NULL while empty). The fields are
 * public to be read and written only by the calls below.
 */
typedef struct arnm_roaring_bitmap {
  union {
    arnm_roaring_container *containers; /**< With containers: the directory, sorted by key. */
    uint32_t *values;                   /**< Sparse: the values, sorted; NULL while empty. */
  };
  uint32_t cardinality;   /**< Values in the set. */
  uint32_t maximum;       /**< Largest value; meaningless while empty. */
  uint32_t count;         /**< Containers in use, 1 to 65536; 0 for a sparse or empty set. */
  uint8_t directory_log2; /**< The block of the directory, or of the values, is 2^this bytes. */
} arnm_roaring_bitmap;

// ********** building *******************

/**
 * @brief Make @p set an empty set. Reads nothing; the same as zeroing it.
 * @param[out] set Set to empty; NULL is a no-op.
 */
void arnm_roaring_init(arnm_roaring_bitmap *set);

/**
 * @brief Give every block of @p set back to @p pool and leave it empty.
 *
 * The counterpart of building a set: afterwards it holds nothing, and a local one may go out
 * of scope or be built again.
 *
 * @param[in,out] set  Set to empty; NULL is a no-op, and so is an empty set.
 * @param[in,out] pool The pool every block of @p set came from; not NULL unless @p set is
 *                     empty.
 */
void arnm_roaring_free(arnm_roaring_bitmap *set, arnm_graded_block_pool *pool);

/**
 * @brief Add @p value, which must not be below the largest value already in the set.
 *
 * @param[in,out] set   Set to add to; not NULL. Unchanged on failure.
 * @param[in]     value Value to add. Equal to the largest is a no-op and a success.
 * @param[in,out] pool  Pool for a new or grown block; not NULL.
 * @retval ARNM_SUCCESS                    Added, or it was already the largest.
 * @retval ARNM_ERROR_NULL_POINTER         @p set or @p pool is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM        @p value is below the largest value in the set.
 * @retval ARNM_ERROR_RESOURCE_EXHAUSTED   The set already holds UINT32_MAX values, the most its
 *                                         count can say.
 * @return Any refusal of @ref arnm_graded_block_pool_alloc() for a new block.
 * @whisper Each new number lands at the far end, where the drawer is still open
 */
arnm_result arnm_roaring_add(
    arnm_roaring_bitmap *set, uint32_t value, arnm_graded_block_pool *pool
);

// ********** reading *******************

/**
 * @brief Values in @p set. 0 for NULL.
 */
static inline uint32_t arnm_roaring_cardinality(const arnm_roaring_bitmap *set) {
  return set ? set->cardinality : 0u;
}

/**
 * @brief The smallest value.
 * @param[in]  set Set to read; may be NULL.
 * @param[out] out Receives the value; not NULL. Untouched when the answer is false.
 * @return false when @p set is NULL or empty.
 */
bool arnm_roaring_minimum(const arnm_roaring_bitmap *set, uint32_t *out);

/**
 * @brief The largest value.
 * @param[in]  set Set to read; may be NULL.
 * @param[out] out Receives the value; not NULL. Untouched when the answer is false.
 * @return false when @p set is NULL or empty.
 */
bool arnm_roaring_maximum(const arnm_roaring_bitmap *set, uint32_t *out);

/**
 * @brief Whether @p value is in @p set. A binary search over the keys, then one bit or a binary
 *        search over the low parts.
 * @param[in] set Set to read; NULL answers false.
 */
bool arnm_roaring_contains(const arnm_roaring_bitmap *set, uint32_t value);

/**
 * @brief Values of @p set inside `[min, max]`.
 *
 * A container wholly inside the range is counted from its stored cardinality; only the two at
 * the edges are looked into.
 *
 * @param[in] set Set to read; NULL answers 0.
 * @return The count; 0 for an empty range.
 */
uint32_t arnm_roaring_range_cardinality(const arnm_roaring_bitmap *set, uint32_t min, uint32_t max);

/**
 * @brief The value at 0 based @p rank in ascending order.
 *
 * Walks the containers by their cardinality, so the cost grows with the number of containers
 * before the one that holds the answer.
 *
 * @param[in]  set  Set to read; may be NULL.
 * @param[in]  rank 0 for the smallest value.
 * @param[out] out  Receives the value; not NULL. Untouched when the answer is false.
 * @return false when @p rank is not below the cardinality, or @p set is NULL.
 */
bool arnm_roaring_select(const arnm_roaring_bitmap *set, uint32_t rank, uint32_t *out);

/**
 * @brief Up to @p size values into @p out, after skipping @p skip: ascending from the smallest,
 *        or descending from the largest.
 *
 * One page of a listing without building anything. The next page is the same call with
 * @p skip grown by @p size.
 *
 * @param[in]  set        Set to read; NULL answers 0.
 * @param[in]  skip       Values to pass over first, from the end the listing starts at.
 * @param[in]  size       Most values to write.
 * @param[in]  descending Start from the largest value and go down.
 * @param[out] out        Room for @p size values; not NULL unless @p size is 0.
 * @return Values written, @p size or fewer when the set runs out.
 */
uint32_t arnm_roaring_page(
    const arnm_roaring_bitmap *set, uint32_t skip, uint32_t size, bool descending, uint32_t *out
);

// ********** reading a union without building it *******************

/** @brief Most sets @ref arnm_roaring_union_cardinality() and @ref arnm_roaring_union_page()
 *         read at once. */
#define ARNM_ROARING_UNION_MAX 8u

/**
 * @brief Values in the union of @p sets inside `[min, max]`, counted without building the union.
 *
 * The sets are walked key by key inside the range. A key only one set has is counted from that
 * container, a key several sets share from their arrays merged or, where a bitmap is among them,
 * from one bitmap on the stack. Nothing is allocated. "Every transaction an address is involved
 * in" is such a union -- balance, signer and other -- and this is its count.
 *
 * @param[in]  sets  The sets; not NULL. An entry may be NULL and counts as empty.
 * @param[in]  count Sets in @p sets, 0 to @ref ARNM_ROARING_UNION_MAX.
 * @param[in]  min   Smallest value counted.
 * @param[in]  max   Largest value counted.
 * @param[out] out   Receives the count; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS             Counted; 0 for an empty range or no sets.
 * @retval ARNM_ERROR_NULL_POINTER  @p sets or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p count is past @ref ARNM_ROARING_UNION_MAX.
 * @whisper Heads counted where the streams meet, without pouring them into one basin
 */
arnm_result arnm_roaring_union_cardinality(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    uint64_t *out
);

/**
 * @brief A page of the union of @p sets inside `[min, max]`, without building the union.
 *
 * As @ref arnm_roaring_page() over the union restricted to the range: up to @p size values
 * after skipping @p skip, ascending from the smallest value in the range or descending from the
 * largest. Keys are passed over whole by their count while @p skip lasts; only the key the page
 * starts in and the ones it runs through are read value by value. "The newest 20 transactions
 * of an address" is this call with @p descending and a @p skip of 0: it reads the top key and
 * stops.
 *
 * @param[in]  sets       The sets; not NULL. An entry may be NULL and counts as empty.
 * @param[in]  count      Sets in @p sets, 0 to @ref ARNM_ROARING_UNION_MAX.
 * @param[in]  min        Smallest value the page may hold.
 * @param[in]  max        Largest value the page may hold.
 * @param[in]  skip       Values of the range to pass over first, from the end the page starts at.
 * @param[in]  size       Most values to write.
 * @param[in]  descending Start from the largest value in the range and go down.
 * @param[out] out        Room for @p size values; not NULL unless @p size is 0.
 * @param[out] written    Receives how many were written; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS             @p *written values are in @p out, fewer than @p size when the
 *                                  range runs out.
 * @retval ARNM_ERROR_NULL_POINTER  @p sets, @p written, or @p out with a @p size, is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p count is past @ref ARNM_ROARING_UNION_MAX.
 * @whisper The newest drops are taken from where the streams meet, the rest left to flow
 */
arnm_result arnm_roaring_union_page(
    const arnm_roaring_bitmap *const *sets,
    uint32_t count,
    uint32_t min,
    uint32_t max,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written
);

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

#endif // ARNM_ROARING_BITMAP_H
