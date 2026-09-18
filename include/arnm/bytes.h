#ifndef ARNM_BYTES_H
#define ARNM_BYTES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_bytes arnm_bytes
 * @brief Values read out of raw bytes without assuming they are aligned for them.
 *
 * A block on a free list keeps the link to the next in its own first bytes. Reading it through
 * a cast would claim an alignment and a type the bytes never promised; a memcpy of the exact
 * size promises neither and compiles to the same single load.
 *
 * @{
 */

/**
 * @brief The pointer stored in the first `sizeof(uint8_t *)` bytes of @p data.
 * @param[in] data Bytes to read; not NULL, at least `sizeof(uint8_t *)` of them, any alignment.
 * @return The pointer as it was stored.
 */
static inline uint8_t *arnm_load_ptr(const uint8_t *data) {
  uint8_t *ptr = NULL;
  memcpy(&ptr, data, sizeof(ptr));
  return ptr;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_BYTES_H
