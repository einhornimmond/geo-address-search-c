/** @defgroup io I/O and display
 *  @brief Streams, progress bars, and the visible rhythm of long-running work.
 *
 *  Parent group for modules that communicate with the outside world
 *  or render the program's pulse to the operator.
 */

/** @defgroup progress Progress tracking
 *  @ingroup io
 *  @brief A heartbeat for long-running streams — measuring what has passed
 *         and what remains.
 *
 *  One walk is open at a time, and every walk is announced before it begins:
 *  the name of the step is written the moment it starts, so nothing the
 *  program does happens behind a silent screen.  While it runs, a bar keeps
 *  the line alive from a thread of its own — a step that blocks in a merge or
 *  in the kernel still shows that it is moving.  When it ends, the bar gives
 *  way to the one line that outlives it: how long that step took.
 *
 *  Only that: how long *that step* took.  The whole run is summed up once, by
 *  the caller, at the very end — a total repeated after every stage says
 *  nothing about either.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Where an open walk reads its count from.
 *
 *  Some work cannot report on itself — a merge does not come up for air, and a
 *  file being written knows nothing of the program watching it.  A poll is
 *  read from outside instead, every few hundred milliseconds, by whoever draws.
 *
 *  @param user_data  Whatever was handed to progress_begin_polled().
 *  @return The count so far, in the unit of the walk (bytes).
 */
typedef uint64_t (*ProgressPoll)(void *user_data);

/**
 * @brief Announce a step and start watching it.
 *
 *  The name is written at once — before any of the work — so the reader always
 *  knows what the program is doing, even for the seconds before the first bar
 *  appears.  Work that settles quickly never draws one at all.
 *
 *  @param what         What is about to happen, in one line.
 *  @param total_bytes  The whole the step works through, or 0 when the end is
 *                      not known ahead of time — then the bar only shows that
 *                      something is still moving.
 */
void progress_begin(const char *what, uint64_t total_bytes);

/**
 * @brief Announce a step whose count has to be fetched rather than reported.
 *
 *  Same as progress_begin(), except that the current count is read from
 *  @p poll while the step runs. For work that never returns to the caller
 *  until it is done.
 *
 *  @param what         What is about to happen, in one line.
 *  @param total_bytes  The whole, or 0 when it is not known.
 *  @param poll         Read for the current count; must be safe to call from
 *                      another thread.
 *  @param user_data    Handed back to @p poll unchanged.
 */
void progress_begin_polled(
    const char *what, uint64_t total_bytes, ProgressPoll poll, void *user_data
);

/**
 * @brief Mark another stride along the path.
 *
 *  Stores the count; the drawing happens elsewhere, on its own clock, so
 *  calling this often costs nothing but a store.
 *
 *  @param currentBytes   Bytes processed so far (monotonically increasing).
 */
void progress_update(uint64_t currentBytes);

/**
 * @brief Close the step — it has settled.
 *
 *  Takes the bar off the screen and leaves one line in its place: how long
 *  this step took, and what it moved. The next thing the program does
 *  announces itself.
 *
 *  @whisper The rhythm ends — the count is whole
 */
void progress_end(void);

/** @} */
