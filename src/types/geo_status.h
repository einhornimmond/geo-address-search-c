/** @defgroup types Shared types
  *  @brief Meanings more than one module names — each in a file of its own, so
  *         that it is included where it is needed and never restated.
  *
  *  Nothing here holds behaviour: these are enums and plain records, and a
  *  header in this group pulls in no other module of ours.  That is what lets
  *  the client library take three of them along and leave the builder behind.
  */

/** @defgroup geo_status Geo status
 *  @ingroup types
 *  @brief How a client call ended — the whole vocabulary of failure the library
 *         offers, and the only one it uses.
 *
 *  The client never ends the process and never writes to a stream; every path
 *  that cannot finish returns one of these instead.  That is the whole reason
 *  the enum is small: a caller across a language border can switch over five
 *  values, and the numbers are stable so it may.
 *
 *  @whisper A door that closes quietly always says which one it was
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How a client call ended. */
typedef enum GeoStatus {
  GEO_OK = 0,           /**< The call did what it was asked to. */
  GEO_ERROR_ARGUMENT,   /**< A pointer was NULL or a size was 0; nothing was touched. */
  GEO_ERROR_FILE,       /**< The file would not open, stat or map. */
  GEO_ERROR_FORMAT,     /**< Opened and mapped, but not an index this build reads. */
  GEO_ERROR_MEMORY      /**< An allocation failed; what was taken is given back. */
} GeoStatus;

#ifdef __cplusplus
}
#endif

/** @} */
