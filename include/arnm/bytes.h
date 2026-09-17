#include <memory.h>
#include <stdint.h>

// Loads a pointer stored in the first sizeof(ptr) bytes of data.
static inline uint8_t *load_ptr(const uint8_t *data) {
  uint8_t *ptr = NULL;
  memcpy(&ptr, data, sizeof(ptr));
  return ptr;
}
