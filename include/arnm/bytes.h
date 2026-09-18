#ifndef ARNM_BYTES_H
#define ARNM_BYTES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads a pointer stored in the first sizeof(ptr) bytes of data.
static inline uint8_t *arnm_load_ptr(const uint8_t *data) {
  uint8_t *ptr = NULL;
  memcpy(&ptr, data, sizeof(ptr));
  return ptr;
}

#ifdef __cplusplus
}
#endif

#endif // ARNM_BYTES_H
