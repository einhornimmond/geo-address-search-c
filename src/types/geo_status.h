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
 *  the enum is small: five values are something a caller across a language border
 *  can switch over exhaustively, and the numbers are stable so that it may.
 *
 *  Stable means written down twice.  Every enumerator carries its number here,
 *  and `bindings/kinds.js` mirrors the same five for Bun and Node.  Nothing
 *  checks that the two agree — a value slipped into the middle would leave the C
 *  build green and every binding naming the wrong failure — so a new one is
 *  appended with the next number, and both places are changed together.
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
  GEO_OK = 0,             /**< The call did what it was asked to. */
  GEO_ERROR_ARGUMENT = 1, /**< A pointer was NULL or a size was 0; nothing was touched. */
  GEO_ERROR_FILE = 2,     /**< The file would not open, stat or map. */
  GEO_ERROR_FORMAT = 3,   /**< Opened and mapped, but not an index this build reads. */
  GEO_ERROR_MEMORY = 4    /**< An allocation failed; what was taken is given back. */
} GeoStatus;

#ifdef __cplusplus
}
#endif

/** @} */
