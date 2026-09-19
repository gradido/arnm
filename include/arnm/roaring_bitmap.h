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
 * ### Three headers
 *
 * This one holds the set: building it, and reading one of them. @ref arnm_roaring_ops builds a
 * new set out of others -- and, or, andnot, a range copied out. @ref arnm_roaring_query answers
 * a question over several sets -- how many, which page, the first or the last -- without
 * building anything at all, which is what a filter over an index asks for.
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
 * Containers before the page are passed over by their stored count, two loads each; what a deep
 * @p skip pays sits inside the container it lands in, where reaching a rank in a bitmap counts
 * its words -- a page at rank 4400 of one bitmap container measured 880 ns against 19 at its
 * front, bounded by the 1024 words a container holds. A page from the end, descending without a
 * skip, starts at the last word and pays none of it.
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

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_ROARING_BITMAP_H
