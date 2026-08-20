/** @defgroup parse_queue Parse queue
 *  @ingroup parser
 *  @brief A bounded channel between the decompressor and the parser threads —
 *         handoff without collision.
 *
 *  One thread pulls blocks out of the zstd stream, several parse them, and this
 *  is where they meet.  The bound is the point: a reader faster than its parsers
 *  would otherwise pull the whole dump into memory, so it is made to wait
 *  instead, and the queue becomes the throttle that keeps a 24 GB stream inside
 *  a handful of buffers.
 *  @{
 */

#pragma once

#include <stddef.h>

#include "foundation/line_buffer.h"

/** Batches the queue holds before a producer has to wait. */
enum { PARSE_QUEUE_CAPACITY = 8 };

/**
 * @brief A parcel of work handed from reader to parser.
 *
 *  The buffer holds one or more complete JSON lines; @c len says how far into it
 *  the lines reach.  The parcel is passed by value, but the buffer behind it is
 *  not copied — it travels on to the consumer, which gives it back to the pool
 *  when it is done.
 */
typedef struct ParseBatch {
  LineBuffer *buffer; /**< Buffer holding the raw bytes; borrowed from the pool. */
  size_t len;         /**< Bytes to read from @c buffer. */
} ParseBatch;

/** Opaque handle for a fixed-capacity, thread-safe work queue. */
typedef struct ParseQueue ParseQueue;

/**
 * @brief Open an empty queue.
 *
 *  @return A new queue, or NULL when the allocation or one of the pthread
 *          primitives could not be set up.
 */
ParseQueue *parse_queue_create(void);

/**
 * @brief Give the queue's own memory back.
 *
 *  Its own and nothing else: the ring is freed, and whatever batches were still
 *  standing in it are simply gone.  A buffer one of them carried is dropped with
 *  it — the pool it came from handed it out and stopped tracking it, so the pool
 *  can no longer free it either, and this call never knew it was there.  That is a leak, not a
 *  transfer: nothing here is arena memory that a later reset would sweep up.
 *
 *  So the precondition is a quiet queue: closed, every producer and consumer
 *  joined, nothing left inside.  That is not a rule a caller has to remember —
 *  it falls out of parse_queue_pop(), which reports the end only once the queue
 *  is closed *and* empty.  A consumer that runs to that point has, by
 *  definition, taken everything; joining it is what makes the queue safe to
 *  destroy.
 *
 *  @param[in] queue  Queue to tear down; NULL is a no-op.
 *  @note Not enforced.  A test that fills a queue and never drains it is doing
 *        something reasonable — its batches carry no buffers — and an assertion
 *        here would refuse it for a leak that is not happening.
 */
void parse_queue_destroy(ParseQueue *queue);

/**
 * @brief Hand a batch over, waiting while the queue is full.
 *
 *  The wait is the throttle: a producer that cannot place its batch stops
 *  reading, and the stream slows to the pace the parsers set.
 *
 *  @param[in,out] queue  Target queue; not NULL.
 *  @param[in]     batch  The parcel; its buffer passes to whoever pops it.
 *  @warning A producer waiting here is not woken by parse_queue_close().  Only
 *           the thread that fills the queue may close it, and it is not inside
 *           push() when it does.
 */
void parse_queue_push(ParseQueue *queue, ParseBatch batch);

/**
 * @brief Take the next batch, waiting until one arrives or the queue runs dry.
 *
 *  Closing does not discard what is already queued.  A consumer keeps receiving
 *  batches after parse_queue_close() and is told the stream has ended only once
 *  the last of them has been handed out — which is what lets the reader close
 *  the queue the moment it is done, without waiting for the parsers.
 *
 *  @param[in,out] queue  Source queue; not NULL.
 *  @param[out]    batch  Receives the parcel; untouched when 0 comes back.
 *  @return 1 when a batch was handed over, 0 when the queue is both closed and
 *          empty — and will stay that way.
 */
int parse_queue_pop(ParseQueue *queue, ParseBatch *batch);

/**
 * @brief Say that nothing more will arrive.
 *
 *  Wakes every waiting consumer so that those with nothing left to do can leave.
 *  What is still in the queue is still handed out; see parse_queue_pop().
 *
 *  @param[in,out] queue  Queue to close; not NULL.
 *
 *  @whisper The last drop has fallen — the stream is still
 */
void parse_queue_close(ParseQueue *queue);

/** @} */
