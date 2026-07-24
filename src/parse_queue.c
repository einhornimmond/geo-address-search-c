#include "parse_queue.h"
#include "error.h"

#include <pthread.h>
#include <stdlib.h>

struct ParseQueue {
  ParseBatch batches[PARSE_QUEUE_CAPACITY];
  size_t head;
  size_t tail;
  size_t count;
  int closed;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
};

ParseQueue *parse_queue_create(void) {
  ParseQueue *queue = calloc(1, sizeof(*queue));
  if (!queue) { return NULL; }
  if (pthread_mutex_init(&queue->mutex, NULL) || pthread_cond_init(&queue->not_empty, NULL) ||
      pthread_cond_init(&queue->not_full, NULL)) {
    free(queue);
    return NULL;
  }
  return queue;
}

void parse_queue_destroy(ParseQueue *queue) {
  if (!queue) { return; }
  pthread_cond_destroy(&queue->not_full);
  pthread_cond_destroy(&queue->not_empty);
  pthread_mutex_destroy(&queue->mutex);
  free(queue);
}

void parse_queue_push(ParseQueue *queue, ParseBatch batch) {
  pthread_mutex_lock(&queue->mutex);
  while (queue->count == PARSE_QUEUE_CAPACITY) {
    pthread_cond_wait(&queue->not_full, &queue->mutex);
  }
  queue->batches[queue->tail] = batch;
  queue->tail = (queue->tail + 1) % PARSE_QUEUE_CAPACITY;
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
}

int parse_queue_pop(ParseQueue *queue, ParseBatch *batch) {
  pthread_mutex_lock(&queue->mutex);
  while (queue->count == 0 && !queue->closed) {
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
  }
  if (queue->count == 0) {
    pthread_mutex_unlock(&queue->mutex);
    return 0;
  }
  *batch = queue->batches[queue->head];
  queue->head = (queue->head + 1) % PARSE_QUEUE_CAPACITY;
  --queue->count;
  pthread_cond_signal(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return 1;
}

void parse_queue_close(ParseQueue *queue) {
  pthread_mutex_lock(&queue->mutex);
  queue->closed = 1;
  pthread_cond_broadcast(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
}
