#ifndef ARNM_RESULT_H
#define ARNM_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup arnm_result arnm_result
 *
 *  @{
 */

/** @brief Outcome of an operation: success, a warning, or an error.
 *
 *  The order of the enumerators is part of the contract. Success is 0, warnings
 *  follow, and every error comes after @ref ARNM_ERROR_NOT_IMPLEMENTED_YET.
 *  Keep new warnings in the warning block and new errors
 *  after it. Please handle warnings explicit in the code.
 */
typedef enum arnm_result {
  ARNM_SUCCESS = 0,

  // warnings: the operation was carried out, with a caveat worth reporting
  ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED,
  ARNM_WARNING_USED_DYNAMIC_ALLOCATION_FALLBACK,

  // errors: the operation was not carried out
  ARNM_ERROR_NOT_IMPLEMENTED_YET,
  ARNM_ERROR_NOT_INITIALIZED,
  ARNM_ERROR_INVALID_PARAM,     // if parameter validation failed
  ARNM_ERROR_INVALID_ENUM_TYPE, // enum type invalid for function call
  ARNM_ERROR_INVALID_STATE,
  ARNM_ERROR_NULL_POINTER,
  ARNM_ERROR_ARITHMETIC_OVERFLOW,
  ARNM_ERROR_OUT_OF_MEMORY,
  ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS,

  // a ressource manager like a pool handed out everything it had; the request is sound, the supply
  // is not
  ARNM_ERROR_RESOURCE_EXHAUSTED,
  // fixed size exceed
  ARNM_ERROR_RESOURCE_SIZE_EXCEED,
  // something is still lent out, so what was asked for would pull it away from its holder
  ARNM_ERROR_RESOURCE_IN_USE,

  ARNM_ERROR_DECODE_FAILED,
  ARNM_ERROR_ENCODE_FAILED,
  ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL,

  // enum
  ARNM_ERROR_ENUM_UNHANDLED,
  ARNM_ERROR_ENUM_UNKNOWN,

  /** First value reserved for the embedding project.
   *
   *  Everything below this belongs to arnm and may gain members between releases. A host
   *  that needs codes of its own counts up from here, and the two ranges cannot drift into
   *  each other:
   *  @code
   *  enum { MY_ERROR_DECODE = ARNM_ERROR_USER_BASE, MY_ERROR_SIGNATURE };
   *  @endcode
   */
  ARNM_ERROR_USER_BASE = 1000
} arnm_result;

const char *arnm_result_to_string(arnm_result result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_RESULT_H
