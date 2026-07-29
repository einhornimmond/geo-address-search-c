/** @defgroup parse_queue Parse queue
 *  @ingroup parser
 *  @brief A bounded channel between decompressor and parser threads — handoff
 *         without collision.
 *  @{
 */

#pragma once

#include <stddef.h>

#include "foundation/line_buffer.h"

/** Maximum number of batches the queue can hold at once. */
enum { PARSE_QUEUE_CAPACITY = 8 };

/**
 * @brief A parcel of work handed from reader to parser.
 *
 *  Carries a line buffer containing one or more complete JSON lines
 *  together with the precise byte length to consume.
 */
typedef struct ParseBatch {
  LineBuffer *buffer; /**< Buffer holding the raw bytes. */
  size_t len;         /**< Number of bytes to process from buffer. */
} ParseBatch;

/** Opaque handle for a fixed-capacity, thread-safe work queue. */
typedef struct ParseQueue ParseQueue;

/**
 * @brief Allocate and initialise a new parse queue.
 *
 *  @return   New ParseQueue pointer, or NULL on allocation failure.
 */
ParseQueue *parse_queue_create(void);

/**
 * @brief Tear down the queue and free its resources.
 *
 *  Safe to call with NULL. Does not free the LineBuffers that were
 *  pushed — those belong to the buffer pool.
 *
 *  @param[in] queue   Queue to destroy (NULL is a no-op).
 */
void parse_queue_destroy(ParseQueue *queue);

/**
 * @brief Enqueue a batch, blocking if the queue is full.
 *
 *  Blocks the caller until space opens. Ownership of the batch's
 *  LineBuffer transfers to the consumer.
 *
 *  @param[in,out] queue   Target queue.
 *  @param[in]     batch   Work parcel to enqueue (moved, not copied).
 */
void parse_queue_push(ParseQueue *queue, ParseBatch batch);

/**
 * @brief Dequeue a batch, blocking until work arrives or the queue closes.
 *
 *  @param[in,out] queue   Source queue.
 *  @param[out]    batch   Receives the next work parcel.
 *  @return                1 if a batch was dequeued, 0 if the queue has closed.
 */
int parse_queue_pop(ParseQueue *queue, ParseBatch *batch);

/**
 * @brief Signal that no more work will arrive.
 *
 *  Wakes all blocked consumers so they can exit cleanly. After
 *  closing, `parse_queue_pop` returns 0 immediately.
 *
 *  @param[in,out] queue   Queue to close.
 *
 *  @whisper The last drop has fallen — the stream is still
 */
void parse_queue_close(ParseQueue *queue);

/** @} */
