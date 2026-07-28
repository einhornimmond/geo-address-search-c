/** @defgroup types Shared types
  *  @brief Enumerations more than one module names — each in a file of its own,
  *         so that a meaning is included where it is needed and never restated.
  */

/** @defgroup geo_status Geo status
 *  @ingroup types
 *  @brief How a client call ended — the whole vocabulary of failure the library
 *         offers, and the only one it uses.
 *
 *  The client never ends the process and never writes to a stream; every path
 *  that cannot finish returns one of these instead.  The numbers are stable, so
 *  a foreign caller may switch on them.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** How a call ended. */
typedef enum GeoStatus {
  GEO_OK = 0,           /**< Everything went through. */
  GEO_ERROR_ARGUMENT,   /**< A pointer was NULL or a size was 0. */
  GEO_ERROR_FILE,       /**< The file could not be opened or mapped. */
  GEO_ERROR_FORMAT,     /**< The file is not an index this build can read. */
  GEO_ERROR_MEMORY      /**< An allocation failed. */
} GeoStatus;

#ifdef __cplusplus
}
#endif

/** @} */
