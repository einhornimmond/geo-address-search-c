/** @defgroup foundation Foundation
 *  @brief The ground the program stands on — buffers, arenas, durations,
 *         progress and last words.
 *
 *  Parent group for the modules that carry no knowledge of places, dumps or
 *  indexes.  Everything above rests on them; they rest on nothing of ours,
 *  which is why they can be read, moved and tested without a dump in reach.
 */

/** @defgroup error Error handling
 *  @ingroup foundation
 *  @brief Terminal and diagnostic messages — where the program speaks its last words or
 *         leaves a quiet note in passing.
 *  @{
 */

#pragma once

#include <stdarg.h>

#include "types/error_art.h"

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
_Noreturn void fatal(ErrorArt art, const char *fmt, ...);

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
void info(const char *fmt, ...);

/** @} */
