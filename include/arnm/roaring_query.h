#ifndef ARNM_ROARING_QUERY_H
#define ARNM_ROARING_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#include "arnm/result.h"
#include "arnm/roaring_bitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_roaring_query arnm_roaring_query
 * @brief Answers over several @ref arnm_roaring_bitmap sets, without building one.
 *
 * What a filter over an index asks: how many values are left, which page of them, which is the
 * newest. The sets are walked key by key and each key is answered the cheapest way its parts
 * allow; nothing is allocated, so no pool is named here.
 *
 * Every range is closed, `[min, max]`. Pass `0` and `UINT32_MAX` for no restriction.
 *
 * @note Nothing here is thread safe against a set being added to at the same time.
 *
 * @whisper Counted where the streams meet, without pouring them into one basin
 *
 * @{
 */

/** @brief Most sets a query takes in each of its three lists. */
#define ARNM_ROARING_QUERY_MAX 8u

/**
 * @brief Which values a query matches: in every set of @c all, in at least one of @c any, in
 *        none of @c none, and inside `[min, max]`.
 *
 * A list is empty when its count is 0, and an empty @c any places no condition. At least one of
 * @c all and @c any has to hold a set; a query with neither matches nothing. A NULL entry in a
 * list counts as an empty set: it makes @c all and @c any match nothing and leaves @c none
 * without effect.
 *
 * The filters of a transaction index read off directly: the sets of one address in @c any, the
 * transaction type in @c all, a foreign coin community in @c none, the transaction numbers a
 * date range covers as @c min and @c max.
 */
typedef struct arnm_roaring_query {
  const arnm_roaring_bitmap *const *all;  /**< every one of them holds the value (intersection) */
  uint32_t all_count;                     /**< sets in @c all, 0 to @ref ARNM_ROARING_QUERY_MAX */
  const arnm_roaring_bitmap *const *any;  /**< at least one of them holds it (union) */
  uint32_t any_count;                     /**< sets in @c any, 0 to @ref ARNM_ROARING_QUERY_MAX */
  const arnm_roaring_bitmap *const *none; /**< none of them holds it (difference) */
  uint32_t none_count;                    /**< sets in @c none, 0 to @ref ARNM_ROARING_QUERY_MAX */
  uint32_t min;                           /**< smallest value that may match */
  uint32_t max;                           /**< largest value that may match */
} arnm_roaring_query;

/**
 * @brief How many values @p query matches, counted without building the result.
 *
 * The sets are walked key by key, and only where they meet: a key missing from one set of
 * @c all is passed over in every other. Inside a key the cheapest way its parts allow is taken
 * -- the values of the smallest set looked up in the others while there are few of them, the
 * words of the key combined and counted once there are many.
 *
 * @param[in]  query The query; not NULL. Nothing in it is changed.
 * @param[out] out   Receives the count; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS             Counted; 0 for an empty range or a query that matches nothing.
 * @retval ARNM_ERROR_NULL_POINTER  @p query or @p out is NULL, or a list with a count is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM A list is longer than @ref ARNM_ROARING_QUERY_MAX.
 * @whisper Counted where the streams meet, without pouring them into one basin
 */
arnm_result arnm_roaring_query_cardinality(const arnm_roaring_query *query, uint64_t *out);

/**
 * @brief A page of what @p query matches: up to @p size values after passing over @p skip,
 *        ascending from the smallest or descending from the largest.
 *
 * Keys are passed over whole by their count while @p skip lasts; only the key the page starts in
 * and the ones it runs through are read value by value, and the walk stops as soon as the page is
 * full. The newest match is therefore a page of one, descending, with no skip -- which is what a
 * validation asks for, and it reads a single key.
 *
 * @param[in]  query      The query; not NULL.
 * @param[in]  skip       Values to pass over first, from the end the page starts at.
 * @param[in]  size       Most values to write.
 * @param[in]  descending Start from the largest matching value and go down.
 * @param[out] out        Room for @p size values; not NULL unless @p size is 0.
 * @param[out] written    Receives how many were written; not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS             @p *written values are in @p out, fewer than @p size when the
 *                                  matches run out.
 * @retval ARNM_ERROR_NULL_POINTER  @p query, @p written, or @p out with a @p size, is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM A list is longer than @ref ARNM_ROARING_QUERY_MAX.
 * @whisper The newest drops are taken from where the streams meet, the rest left to flow
 */
arnm_result arnm_roaring_query_page(
    const arnm_roaring_query *query,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written
);

/**
 * @brief One listing: how many values @p query matches, and one page of them, in a single walk.
 *
 * What a listing over an index asks for -- "how many, and show me these twenty" -- answered
 * without reading the sets twice. Asking @ref arnm_roaring_query_cardinality() and
 * @ref arnm_roaring_query_page() one after the other walks everything the query touches twice;
 * here every key is counted once, and only the key the page starts in, and the ones it runs
 * through, are read value by value on top of that.
 *
 * A @p size of 0 asks for the count alone, and @p out may then be NULL.
 *
 * @param[in]  query       The query; not NULL.
 * @param[in]  skip        Values to pass over first, from the end the page starts at.
 * @param[in]  size        Most values to write.
 * @param[in]  descending  Start from the largest matching value and go down.
 * @param[out] out         Room for @p size values; not NULL unless @p size is 0.
 * @param[out] written     Receives how many were written; not NULL. Untouched on failure.
 * @param[out] cardinality Receives the matches in the whole range, however small the page is;
 *                         not NULL. Untouched on failure.
 * @retval ARNM_SUCCESS             @p *written values are in @p out and @p *cardinality is the
 *                                  count.
 * @retval ARNM_ERROR_NULL_POINTER  @p query, @p written, @p cardinality, or @p out with a
 *                                  @p size, is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM A list is longer than @ref ARNM_ROARING_QUERY_MAX.
 * @whisper How many drops there are, and the twenty newest, counted in one pass of the hand
 */
arnm_result arnm_roaring_query_listing(
    const arnm_roaring_query *query,
    uint32_t skip,
    uint32_t size,
    bool descending,
    uint32_t *out,
    uint32_t *written,
    uint64_t *cardinality
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_ROARING_QUERY_H
