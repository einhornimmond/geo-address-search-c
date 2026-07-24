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
  *  @{
  */

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Set the horizon for a progress walk.
 *
 *  Tells the progress tracker how many bytes await in the stream
 *  so it can measure each step against the whole. Call once before
 *  any `progress_update`.
 *
 *  @param totalBytes   Total number of bytes to process.
 */
void progress_init(uint64_t totalBytes);

/**
 * @brief Mark another stride along the path.
 *
 *  Advances the visible progress by the delta since the last update
 *  and refreshes the display. Thread-safe for a single writer.
 *
 *  @param currentBytes   Bytes processed so far (monotonically increasing).
 */
void progress_update(uint64_t currentBytes);

/**
 * @brief Close the circle — the stream has settled.
 *
 *  Prints the final progress line and releases internal state.
 *  Call exactly once when all bytes have been consumed.
 *
 *  @whisper The rhythm ends — the count is whole
 */
void progress_finish(void);

/** @} */
