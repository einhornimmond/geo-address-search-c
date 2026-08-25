/** @defgroup foundation Foundation
 *  @brief The ground the program stands on — buffers, durations, progress and
 *         last words.
 *
 *  Parent group for the modules that carry no knowledge of places, dumps or
 *  indexes.  Everything above rests on them; they rest on nothing of ours,
 *  which is why they can be read, moved and tested without a dump in reach.
 */

/** @defgroup error Error handling
 *  @ingroup foundation
 *  @brief Where the program speaks its last words, or leaves a quiet note in
 *         passing.
 *
 *  Two functions, and the difference between them is whether the program goes
 *  on afterwards.  Both belong to the builder alone: the client library ends no
 *  process and writes to no stream — it answers with a @ref GeoStatus instead.
 *  @{
 */

#pragma once

#include <stdarg.h>

#include "types/error_art.h"

/**
 * @brief Deliver a final message and end the process.
 *
 *  Writes a banner for @p art, then the formatted message, then leaves through
 *  `exit(EXIT_FAILURE)`.  The message goes to stderr, and so does every banner
 *  but @ref ERROR_INFO's, which goes to stdout.
 *
 *  This is for the paths where continuing would be a lie: a dump that will not
 *  open, an arena that cannot be grown, an invariant that has already given
 *  way.  Anything a caller could reasonably handle returns a
 *  @c arnm_result instead and lets them decide.
 *
 *  @param art  Which failure this is, choosing the banner.
 *  @param fmt  printf-style format for the line under it.
 *  @param ...  Arguments for @p fmt.
 *  @note @ref ERROR_INFO is accepted like any other art and ends the process
 *        just the same — the friendly banner does not make the call gentler.
 *  @whisper Every ending names its season
 */
_Noreturn void fatal(ErrorArt art, const char *fmt, ...);

/**
 * @brief Whisper a diagnostic without disturbing the flow.
 *
 *  The counterpart to fatal(): something worth mentioning happened and the
 *  program carries on regardless — a buffer that had to grow, a fallback that
 *  was taken.
 *
 *  @param fmt  printf-style format.
 *  @param ...  Arguments for @p fmt.
 *  @warning Currently silent: the body returns before it formats or writes anything.
 *           Silent is not free — the arguments are evaluated at the call site like
 *           any other call, and whatever was done to prepare them is done whether or
 *           not a line appears.  The one live caller spends two format_byte_units()
 *           calls on a note nobody sees.  The call sites are written as though it
 *           printed: read them that way, and do not take a missing line as a sign
 *           that nothing happened.
 *  @whisper A quiet note on passing water
 */
void info(const char *fmt, ...);

/** @} */
