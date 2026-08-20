/** @defgroup error_art Error art
 *  @ingroup types
 *  @brief The domain a fatal condition arose in — a quiet taxonomy of failure,
 *         named where every module can reach it.
 *
 *  A log line carries its context in this enum rather than in a string the
 *  reader has to parse back.  The values live for the length of the process and
 *  no longer: nothing on disk and nothing a caller sees depends on them, so they
 *  may be reordered freely.
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
