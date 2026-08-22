#ifndef HOSTMEM_RESULT_H
#define HOSTMEM_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup hostmem_result hostmem_result
 *
 *  @{
 */

/** @brief Outcome of an operation: success, a warning, or an error.
 *
 *  The order of the enumerators is part of the contract. Success is 0, warnings
 *  follow, and every error comes after @ref HOSTMEM_ERROR_NOT_IMPLEMENTED_YET.
 *  Keep new warnings in the warning block and new errors
 *  after it. Please handle warnings explicit in the code.
 */
typedef enum hostmem_result {
  HOSTMEM_SUCCESS = 0,

  // warnings: the operation was carried out, with a caveat worth reporting
  HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED,
  HOSTMEM_WARNING_USED_DYNAMIC_ALLOCATION_FALLBACK,

  // errors: the operation was not carried out
  HOSTMEM_ERROR_NOT_IMPLEMENTED_YET,
  HOSTMEM_ERROR_NOT_INITIALIZED,
  HOSTMEM_ERROR_INVALID_PARAM,     // if parameter validation failed
  HOSTMEM_ERROR_INVALID_ENUM_TYPE, // enum type invalid for function call
  HOSTMEM_ERROR_INVALID_STATE,
  HOSTMEM_ERROR_NULL_POINTER,
  HOSTMEM_ERROR_ARITHMETIC_OVERFLOW,
  HOSTMEM_ERROR_OUT_OF_MEMORY,
  HOSTMEM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS,

  // a ressource manager like a pool handed out everything it had; the request is sound, the supply
  // is not
  HOSTMEM_ERROR_RESOURCE_EXHAUSTED,
  // something is still lent out, so what was asked for would pull it away from its holder
  HOSTMEM_ERROR_RESOURCE_IN_USE,

  HOSTMEM_ERROR_DECODE_FAILED,
  HOSTMEM_ERROR_ENCODE_FAILED,
  HOSTMEM_ERROR_DESTINATION_BUFFER_TO_SMALL,

  // enum
  HOSTMEM_ERROR_ENUM_UNHANDLED,
  HOSTMEM_ERROR_ENUM_UNKNOWN,

  /** First value reserved for the embedding project.
   *
   *  Everything below this belongs to hostmem and may gain members between releases. A host
   *  that needs codes of its own counts up from here, and the two ranges cannot drift into
   *  each other:
   *  @code
   *  enum { MY_ERROR_DECODE = HOSTMEM_ERROR_USER_BASE, MY_ERROR_SIGNATURE };
   *  @endcode
   */
  HOSTMEM_ERROR_USER_BASE = 1000
} hostmem_result;

const char *hostmem_result_to_string(hostmem_result result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // HOSTMEM_RESULT_H
