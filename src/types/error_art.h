/** @defgroup error_art Error art
 *  @ingroup types
 *  @brief The domain a fatal condition arose in — a quiet taxonomy of failure,
 *         named where every module can reach it.
 *
 *  A log line carries its context in this enum rather than in a string the
 *  reader has to parse back.  That is the whole of its scope: it reaches exactly
 *  one function, fatal(), which reads it to choose a banner and then forgets it.
 *
 *  Valid values are the enumerators named below and nothing else.  fatal()
 *  switches over them without a default branch, on purpose — adding a variant
 *  without a banner should be a compiler warning rather than a silent blank — so
 *  a value from outside the set loses its banner and still ends the process with
 *  the message and exit status it came for.
 *
 *  The numbers themselves carry no weight.  Nothing writes them to disk, nothing
 *  reads them back, and no caller of ours ever sees one: this header is not among
 *  those the client library installs, so the type never leaves the build.  They
 *  may therefore be reordered, renumbered or removed as freely as the code that
 *  names them allows.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where a failure came from, for the last line the program writes. */
typedef enum ErrorArt {
    ERROR_USAGE,           /**< Wrong invocation — the caller asked for the impossible. */
    ERROR_IO,              /**< A file refused to open, to read, or to yield its contents. */
    ERROR_JSON,            /**< Malformed JSON — the structure collapsed under its own weight. */
    ERROR_ZSTD,            /**< A Zstandard stream broke mid-flow. */
    ERROR_ASSERT,          /**< An invariant gave way — the program's own promise, broken. */
    ERROR_MEMORY,          /**< Memory exhausted — the well ran dry. */
    ERROR_HASH_COLLISION,  /**< Two different address keys folded onto one hash: either a
                                coincidence of a kind that does not happen, or a bug. */
    ERROR_INFO,            /**< Not a failure — a note in passing, carried by the same road. */
} ErrorArt;

#ifdef __cplusplus
}
#endif

/** @} */
