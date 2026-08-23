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
  /** The operation was carried out, with nothing to report. Always 0, so `!result` is a valid
   *  success test and the only one that stays right as the enum grows. */
  ARNM_SUCCESS = 0,

  // warnings: the operation was carried out, with a caveat worth reporting

  /** An arena could not take a block back, so it stays reserved until the arena resets. The
   *  resize or release itself was carried out; only the memory did not come back. */
  ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED,
  /** The request was served from the host instead of from the allocator that was asked. */
  ARNM_WARNING_USED_DYNAMIC_ALLOCATION_FALLBACK,

  // errors: the operation was not carried out

  /** The call is part of the interface but has no implementation yet. */
  ARNM_ERROR_NOT_IMPLEMENTED_YET,
  /** The object was never initialized, or was released and not set up again. */
  ARNM_ERROR_NOT_INITIALIZED,
  /** An argument is outside what the call accepts -- a size of 0, a misaligned address, a
   *  count past a documented bound. The arguments alone decided it. */
  ARNM_ERROR_INVALID_PARAM,
  /** An enum argument holds no value this call knows. */
  ARNM_ERROR_INVALID_ENUM_TYPE,
  /** The arguments were sound but the object is in no condition to serve them. Where
   *  @ref ARNM_ERROR_INVALID_PARAM blames the call, this blames what it was called on. */
  ARNM_ERROR_INVALID_STATE,
  /** A pointer the call needs is NULL. Nothing was read through it. */
  ARNM_ERROR_NULL_POINTER,
  /** A size or count could not be computed without wrapping, so nothing was attempted.
   *  Typically a request past @ref ARNM_MAX_ALLOC_SIZE, or one that a counter cannot hold. */
  ARNM_ERROR_ARITHMETIC_OVERFLOW,
  /** No memory was to be had: the host refused, or an arena has no room left. */
  ARNM_ERROR_OUT_OF_MEMORY,
  /** An index addresses no element. */
  ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS,

  // a ressource manager like a pool handed out everything it had; the request is sound, the supply
  // is not

  /** A pool or a capped allocator handed out everything it has. The request was sound, the
   *  supply is not -- unlike @ref ARNM_ERROR_OUT_OF_MEMORY, the host was never asked. */
  ARNM_ERROR_RESOURCE_EXHAUSTED,
  /** The request is larger than the fixed size the resource was built with, so no amount of
   *  waiting or returning would make it servable. */
  ARNM_ERROR_RESOURCE_SIZE_EXCEED,
  /** Something is still lent out, so what was asked for would pull it away from its holder.
   *  Return it and ask again. */
  ARNM_ERROR_RESOURCE_IN_USE,

  /** Input did not hold what the format promised. */
  ARNM_ERROR_DECODE_FAILED,
  /** The value could not be rendered in the target format. */
  ARNM_ERROR_ENCODE_FAILED,
  /** The destination holds fewer bytes than the result needs. Nothing was written. */
  ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL,

  // enum

  /** A value that is valid for the enum, but that this call has no branch for. */
  ARNM_ERROR_ENUM_UNHANDLED,
  /** A value that belongs to no enumerator at all. */
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

/**
 * @brief The enumerator's own spelling, for logs and assertion messages.
 *
 * @param[in] result Value to name.
 * @return A static string equal to the identifier -- `arnm_result_to_string(ARNM_SUCCESS)` is
 *         `"ARNM_SUCCESS"` -- or `"ARNM_ERROR_UNKNOWN"` for anything arnm does not define,
 *         which includes every code at or above @ref ARNM_ERROR_USER_BASE. Never NULL, and
 *         never owned by the caller.
 * @whisper Each outcome, called by its name
 */
const char *arnm_result_to_string(arnm_result result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_RESULT_H
