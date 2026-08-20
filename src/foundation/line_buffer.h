/** @defgroup line_buffer Line buffer
 *  @ingroup foundation
 *  @brief A growing chamber for bytes — holding incomplete lines until they ripen.
 *
 *  A decompressor hands out whatever block it happens to finish, and the last
 *  line in that block is almost never whole.  This buffer keeps the remainder
 *  standing, lets the next block join it, and gives lines to the caller only
 *  once they are complete.  Nothing is copied twice: the fragment moves to the
 *  front, and the callback reads out of the buffer itself.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A resizable byte container that thinks in lines.
 *
 *  Bytes pour in through line_buffer_append() and leave through
 *  line_buffer_process(), which hands out every line it can see and keeps what
 *  is left.  The fields are the buffer's own bookkeeping — read them if it
 *  helps, but the API moves them.
 */
typedef struct LineBuffer {
  char *buffer;    /**< Raw byte storage on the heap. */
  size_t capacity; /**< Bytes allocated; always at least one above @c position. */
  size_t position; /**< Next write offset, and the length currently held. */
} LineBuffer;

/**
 * @brief Open a buffer with room for @p capacity bytes.
 *
 *  The capacity is a starting point, not a limit — line_buffer_append() grows
 *  past it as needed.  Choosing it well only saves the first few growths.
 *
 *  @param capacity  Bytes to reserve up front; must be > 0.
 *  @return A new buffer, or NULL when either allocation failed.
 */
LineBuffer *line_buffer_create(size_t capacity);

/**
 * @brief Release a buffer and the bytes behind it.
 *
 *  @param[in] lb  Buffer to close; NULL is a no-op.
 */
void line_buffer_destroy(LineBuffer *lb);

/**
 * @brief Pour bytes into the buffer, making room where there is none.
 *
 *  Copies @p len bytes to the end.  When they do not fit, the capacity grows by
 *  half again, as often as it takes, and the growth is offered to info() — a
 *  buffer that keeps growing means the size it was opened with was guessed
 *  wrong, and that is worth saying.  Whether it is heard is info()'s business,
 *  and today it is not.  One byte above the contents always stays free, which is
 *  where line_buffer_flush() puts its terminator.
 *
 *  @param[in,out] lb    Target buffer; not NULL.
 *  @param[in]     data  Bytes to copy; read, never held.
 *  @param[in]     len   How many.
 *  @warning There is no failure return.  A growth that cannot be served ends the
 *           process through fatal() — this sits in the read loop of a build that
 *           has no way to continue without its input.
 */
void line_buffer_append(LineBuffer *lb, const char *data, size_t len);

/**
 * @brief Drop the contents, keep the memory.
 *
 *  Sets the length back to zero.  The allocation stays, which is the whole point:
 *  a buffer pool hands the same chamber round instead of returning it to the heap
 *  between blocks.
 *
 *  @param[in,out] lb  Buffer to empty; not NULL.
 */
void line_buffer_reset(LineBuffer *lb);

/**
 * @brief Hand out every complete line and keep the rest.
 *
 *  Walks to each `\n`, passes what stands before it to @p process_line, and
 *  moves on.  A `\r` immediately before the newline is left out, so a file with
 *  either line ending reads the same.  Neither separator reaches the callback,
 *  and an empty line arrives as a length of 0 rather than being skipped.
 *
 *  What follows the last newline is not a line yet.  It is moved to the front and
 *  waits there for the bytes that finish it.
 *
 *  @param[in,out] lb            Buffer to walk; not NULL.
 *  @param[in]     process_line  Called once per complete line, with a pointer into
 *                               the buffer and its length.
 *  @note The line is **not** terminated — it points into the buffer and the byte
 *        after it is the separator.  Read it by the length, or use
 *        line_buffer_flush(), which does terminate.
 */
void line_buffer_process(LineBuffer *lb, void (*process_line)(const char *line, size_t len));

/**
 * @brief Deliver whatever remains — complete or not.
 *
 *  For the end of a stream, where the last line may have no newline behind it.
 *  What is held is passed on as one piece and the buffer stands empty afterwards.
 *  Unlike line_buffer_process(), this one writes a terminator behind the bytes
 *  first, in the byte line_buffer_append() always keeps free.
 *
 *  @param[in,out] lb            Buffer to drain; not NULL.
 *  @param[in]     process_line  Called once, unless nothing was held.
 *
 *  @whisper Even the unfinished fragment finds its way out
 */
void line_buffer_flush(LineBuffer *lb, void (*process_line)(const char *line, size_t len));

/** @} */
