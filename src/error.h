/** @defgroup core Core
  *  @brief Fundamental types and terminal messages — the ground the program stands on.
  *
  *  Parent group for basic infrastructure shared across all other modules.
  */

/** @defgroup error Error handling
  *  @ingroup core
  *  @brief Terminal and diagnostic messages — where the program speaks its last words or
  *         leaves a quiet note in passing.
  *  @{
  */

#pragma once

#include <stdarg.h>

/** @brief The kind of error — a quiet taxonomy of failure.
 *
 *  Each variant names the domain where a fatal condition arose,
 *  so the log carries context without the caller needing to parse strings.
 */
typedef enum ErrorArt {
    ERROR_USAGE,   /**< Wrong invocation — the caller asked for the impossible. */
    ERROR_IO,      /**< A file refused to open, read, or yield its contents. */
    ERROR_JSON,    /**< Malformed JSON — the structure collapsed under its own weight. */
    ERROR_ZSTD,    /**< Zstandard compression stream broke. */
    ERROR_ASSERT,  /**< An invariant was violated — the program's own promise broken. */
    ERROR_MEMORY,  /**< Memory exhausted — the well ran dry. */
    ERROR_INFO,    /**< Not an error — gentle informational note. */
} ErrorArt;

/**
 * @brief Deliver a final message and exit.
 *
 *  Formats a printf-style message, writes it to stderr, and calls `exit(1)`.
 *  This function never returns — it is the last breath of a doomed path.
 *
 *  @param art   The category of the error (used for future routing).
 *  @param fmt   printf-style format string.
 *  @param ...   Format arguments.
 *
 *  @whisper Every ending names its season
 */
_Noreturn void fatal(ErrorArt art, const char* fmt, ...);

/**
 * @brief Whisper a diagnostic without disturbing the flow.
 *
 *  Like `fatal` without the exit — prints an informational message
 *  to stderr so the operator can observe the inner movement.
 *
 *  @param fmt   printf-style format string.
 *  @param ...   Format arguments.
 *
 *  @whisper A quiet note on passing water
 */
void info(const char* fmt, ...);

/** @} */
