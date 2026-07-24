/** @defgroup line_buffer Line buffer
  *  @ingroup data
  *  @brief A growing chamber for bytes — holding incomplete lines until they ripen.
  *  @{
  */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A resizable byte container that thinks in lines.
 *
 *  Data pours in; whenever a newline-carriage separates a complete line
 *  the buffer is ready to yield it. The structure carries its own capacity
 *  and write position so callers never track offsets by hand.
 */
typedef struct LineBuffer {
    char*  buffer;    /**< Raw byte storage on the heap. */
    size_t capacity;  /**< Allocated size of buffer in bytes. */
    size_t position;  /**< Next write offset, also the current logical length. */
} LineBuffer;

/**
 * @brief Create a fresh line buffer with a given initial capacity.
 *
 *  Allocates @p capacity bytes on the heap and returns a handle.
 *  Returns NULL if allocation fails.
 *
 *  @param capacity   Initial buffer size in bytes (must be > 0).
 *  @return           New LineBuffer pointer, or NULL on allocation failure.
 */
LineBuffer* line_buffer_create(size_t capacity);

/**
 * @brief Release a line buffer and its backing memory.
 *
 *  Frees the internal byte buffer and the LineBuffer struct itself.
 *  Safe to call with NULL.
 *
 *  @param[in] lb   Buffer to destroy (NULL is a no-op).
 */
void line_buffer_destroy(LineBuffer* lb);

/**
 * @brief Pour bytes into the buffer.
 *
 *  Appends @p len bytes from @p data to the end of the buffer,
 *  growing capacity transparently when the current space is
 *  insufficient.
 *
 *  @param[in,out] lb     Target buffer.
 *  @param[in]     data   Source bytes to copy.
 *  @param[in]     len    Number of bytes to append.
 */
void line_buffer_append(LineBuffer* lb, const char* data, size_t len);

/**
 * @brief Empty the buffer without freeing its memory.
 *
 *  Resets the write position to zero so the same allocation
 *  can be reused in a buffer pool without churning the heap.
 *
 *  @param[in,out] lb   Buffer to reset.
 */
void line_buffer_reset(LineBuffer* lb);

/**
 * @brief Harvest every complete line and feed it to the callback.
 *
 *  Walks through the buffered data, invoking @p process_line for each
 *  newline-terminated segment. Processed bytes are consumed; trailing
 *  incomplete data shifts to the front and waits for more.
 *
 *  @param[in,out] lb            Buffer to scan.
 *  @param[in]     process_line  Callback receiving (line, length) for each
 *                               complete line.
 */
void line_buffer_process(LineBuffer* lb, void (*process_line)(const char* line, size_t len));

/**
 * @brief Deliver whatever remains — complete or not.
 *
 *  Like `line_buffer_process`, but also passes any trailing bytes
 *  that lack a terminating newline. The buffer stands empty afterwards,
 *  ready for a new cycle.
 *
 *  @param[in,out] lb            Buffer to drain.
 *  @param[in]     process_line  Callback receiving (line, length) for each
 *                               segment.
 *
 *  @whisper Even the unfinished fragment finds its way out
 */
void line_buffer_flush(LineBuffer* lb, void (*process_line)(const char* line, size_t len));

/** @} */
