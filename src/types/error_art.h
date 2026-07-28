/** @defgroup error_art Error art
 *  @ingroup types
 *  @brief The kind of error — a quiet taxonomy of failure, named where every
 *         module can reach it.
 *
 *  Each variant names the domain a fatal condition arose in, so a log line
 *  carries context without the reader having to parse strings.  The values
 *  travel no further than the process; nothing on disk depends on them.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The kind of error — a quiet taxonomy of failure.
 *
 *  Each variant names the domain where a fatal condition arose,
 *  so the log carries context without the caller needing to parse strings.
 */
typedef enum ErrorArt {
    ERROR_USAGE,           /**< Wrong invocation — the caller asked for the impossible. */
    ERROR_IO,              /**< A file refused to open, read, or yield its contents. */
    ERROR_JSON,            /**< Malformed JSON — the structure collapsed under its own weight. */
    ERROR_ZSTD,            /**< Zstandard compression stream broke. */
    ERROR_ASSERT,          /**< An invariant was violated — the program's own promise broken. */
    ERROR_MEMORY,          /**< Memory exhausted — the well ran dry. */
    ERROR_HASH_COLLISION,  /**< Two different address keys folded into the same hash —
                                a cosmic-level coincidence, or a bug in the hash function. */
    ERROR_INFO,            /**< Not an error — gentle informational note. */
} ErrorArt;

#ifdef __cplusplus
}
#endif

/** @} */
