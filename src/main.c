#include "error.h"
#include "format.h"
#include "json_parse.h"
#include "json_stats.h"
#include "line_buffer.h"
#include "meta_area_allocator.h"
#include "parse_queue.h"
#include "progress.h"
#include "sql_export.h"
#include "storage_stats.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "gradido_blockchain_core/utils/mono_timer.h"

enum { BUFFER_POOL_CAPACITY = PARSE_QUEUE_CAPACITY + 4 };

typedef struct BufferPool {
  LineBuffer *buffers[BUFFER_POOL_CAPACITY];
  size_t count;
  pthread_mutex_t mutex;
  pthread_cond_t available;
} BufferPool;

static void buffer_pool_init(BufferPool *pool, size_t buffer_size) {
  memset(pool, 0, sizeof(*pool));
  if (pthread_mutex_init(&pool->mutex, NULL) || pthread_cond_init(&pool->available, NULL)) {
    fatal(ERROR_MEMORY, "Failed to initialize the parser buffer pool.");
  }
  for (size_t i = 0; i < BUFFER_POOL_CAPACITY; ++i) {
    LineBuffer *buffer = line_buffer_create(buffer_size);
    if (!buffer) { fatal(ERROR_MEMORY, "Failed to allocate parser buffer %zu.", i); }
    pool->buffers[pool->count++] = buffer;
  }
}

static LineBuffer *buffer_pool_acquire(BufferPool *pool) {
  pthread_mutex_lock(&pool->mutex);
  while (pool->count == 0) { pthread_cond_wait(&pool->available, &pool->mutex); }
  LineBuffer *buffer = pool->buffers[--pool->count];
  pthread_mutex_unlock(&pool->mutex);
  return buffer;
}

static void buffer_pool_release(BufferPool *pool, LineBuffer *buffer) {
  line_buffer_reset(buffer);
  pthread_mutex_lock(&pool->mutex);
  pool->buffers[pool->count++] = buffer;
  pthread_cond_signal(&pool->available);
  pthread_mutex_unlock(&pool->mutex);
}

static void buffer_pool_destroy(BufferPool *pool) {
  for (size_t i = 0; i < pool->count; ++i) { line_buffer_destroy(pool->buffers[i]); }
  pthread_cond_destroy(&pool->available);
  pthread_mutex_destroy(&pool->mutex);
}

typedef struct {
  JsonStats *stats;
  StorageStats *storage;
} ProcessLineCtx;

static void process_place_callback(const PhotonPlace *place, void *user_data) {
  ProcessLineCtx *ctx = user_data;
  json_stats_count_place(ctx->stats, place);
  storage_stats_record(ctx->storage, place);
}

static void process_json_line(
    const char *line,
    size_t len,
    JsonStats *stats,
    StorageStats *storage_stats,
    MetaAreaAllocator *alloc
) {
  ProcessLineCtx ctx = {stats, storage_stats};
  JsonParseResult result;
  json_parse_line(line, len, process_place_callback, &ctx, alloc, &result);
  json_stats_count_document(stats, &result);
}

static void process_batch(
    const ParseBatch *batch, JsonStats *stats, StorageStats *storage_stats, MetaAreaAllocator *alloc
) {
  const char *line = batch->buffer->buffer;
  const char *end = line + batch->len;

  while (line < end) {
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    size_t len = newline ? (size_t)(newline - line) : (size_t)(end - line);
    if (len > 0 && line[len - 1] == '\r') { --len; }
    if (len > 0) { process_json_line(line, len, stats, storage_stats, alloc); }
    line = newline ? newline + 1 : end;
  }
}

typedef struct ParserThreadArgs {
  ParseQueue *queue;
  BufferPool *pool;
  MetaAreaAllocator *meta_alloc;
  JsonStats stats;
  StorageStats *storage_stats;
} ParserThreadArgs;

static void *parser_thread(void *arg) {
  ParserThreadArgs *args = arg;
  ParseBatch batch;
  while (parse_queue_pop(args->queue, &batch)) {
    process_batch(&batch, &args->stats, args->storage_stats, args->meta_alloc);
    buffer_pool_release(args->pool, batch.buffer);
  }
  return NULL;
}

static void enqueue_complete_lines(
    ParseQueue *queue, BufferPool *pool, LineBuffer **active_buffer
) {
  LineBuffer *current = *active_buffer;
  size_t complete_len = 0;
  char *nl = memrchr(current->buffer, '\n', current->position);
  if (!nl) return;
  complete_len = nl - current->buffer + 1;

  LineBuffer *next = buffer_pool_acquire(pool);
  size_t remaining = current->position - complete_len;
  if (remaining) { line_buffer_append(next, current->buffer + complete_len, remaining); }
  parse_queue_push(queue, (ParseBatch){.buffer = current, .len = complete_len});
  *active_buffer = next;
}

int main(int argc, char *argv[]) {
  grdu_mono_timer timeUsedAll;
  grdu_mono_timer_reset(&timeUsedAll);

  if (argc < 3 || argc > 4) {
    fatal(
        ERROR_USAGE, "Usage: %s <photon_dump.jsonl.zst> <output.sql> [parser_threads: 1-2]", argv[0]
    );
  }

  grdu_mono_timer_init();

  const char *output_filename = argv[2];

  unsigned parser_thread_count = 4;
  if (argc == 4) {
    char *end;
    unsigned long value = strtoul(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || value < 1 || value > 10) {
      fatal(ERROR_USAGE, "parser_threads must between 1 and 10.");
    }
    parser_thread_count = (unsigned)value;
  }

  FILE *fp = fopen(argv[1], "rb");
  if (!fp) { fatal(ERROR_IO, "Cannot open '%s'.", argv[1]); }

  ZSTD_DStream *dstream = ZSTD_createDStream();
  if (!dstream) { fatal(ERROR_MEMORY, "Failed to create ZSTD_DStream."); }

  size_t ret = ZSTD_initDStream(dstream);
  if (ZSTD_isError(ret)) { fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret)); }

  const size_t inputSize = ZSTD_DStreamInSize() * 2;
  const size_t outputSize = ZSTD_DStreamOutSize() * 2;

  char *inputBuffer = malloc(inputSize);
  void *outputBuffer = malloc(outputSize);
  ParseQueue *parse_queue = parse_queue_create();
  MetaAreaAllocator *meta_alloc = meta_area_allocator_create();
  if (!meta_alloc) fatal(ERROR_MEMORY, "Failed to create meta area allocator.");
  pthread_t parser_threads[10];
  ParserThreadArgs parser_args[10] = {0};
  BufferPool buffer_pool;

  char inputSizeBuf[32], outputSizeBuf[32];
  format_byte_units(inputSizeBuf, sizeof(inputSizeBuf), inputSize, 2);
  format_byte_units(outputSizeBuf, sizeof(outputSizeBuf), outputSize, 2);
  printf("inputSize: %s, outputSize: %s\n", inputSizeBuf, outputSizeBuf);

  if (!inputBuffer || !outputBuffer) {
    fatal(ERROR_MEMORY, "Failed to allocate streaming buffers.");
  }
  if (!parse_queue) { fatal(ERROR_MEMORY, "Failed to allocate parsing buffers."); }
  buffer_pool_init(&buffer_pool, outputSize * 2);
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    parser_args[i].queue = parse_queue;
    parser_args[i].pool = &buffer_pool;
    parser_args[i].meta_alloc = meta_alloc;
    parser_args[i].storage_stats = storage_stats_create(meta_alloc);
    if (!parser_args[i].storage_stats)
      fatal(ERROR_MEMORY, "Failed to allocate storage statistics.");
    if (pthread_create(&parser_threads[i], NULL, parser_thread, &parser_args[i]) != 0) {
      fatal(ERROR_MEMORY, "Failed to create parser thread %u.", i);
    }
  }

  fseek(fp, 0, SEEK_END);
  uint64_t totalBytes = ftell(fp);
  rewind(fp);

  progress_init(totalBytes);

  ZSTD_inBuffer input = {
      .src = inputBuffer,
      .size = 0,
      .pos = 0,
  };

  ZSTD_outBuffer output = {
      .dst = outputBuffer,
      .size = outputSize,
      .pos = 0,
  };

  LineBuffer *lineBuffer = buffer_pool_acquire(&buffer_pool);

  while (1) {
    size_t read = fread(inputBuffer, 1, inputSize, fp);
    progress_update(ftell(fp));
    if (read == 0) { break; }
    input.size = read;
    input.pos = 0;

    while (input.pos < input.size || output.pos == output.size) {
      ret = ZSTD_decompressStream(dstream, &output, &input);

      if (ZSTD_isError(ret)) { fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret)); }

      // Process decompressed data
      char *data = (char *)output.dst;
      size_t dataSize = output.pos;

      line_buffer_append(lineBuffer, data, dataSize);
      enqueue_complete_lines(parse_queue, &buffer_pool, &lineBuffer);
      output.pos = 0;

      // if fully flushed
      if (0 == ret) { break; }
    }
  }
  printf("\n");
  progress_finish();
  printf("Reading and decompress file finished, wait for parser threads to finish...\n");
  grdu_mono_timer timeUsed;
  grdu_mono_timer_reset(&timeUsed);
  char timeUsedBuffer[32];

  if (lineBuffer->position > 0) {
    parse_queue_push(parse_queue, (ParseBatch){.buffer = lineBuffer, .len = lineBuffer->position});
  } else {
    buffer_pool_release(&buffer_pool, lineBuffer);
  }
  parse_queue_close(parse_queue);
  for (unsigned i = 0; i < parser_thread_count; ++i) { pthread_join(parser_threads[i], NULL); }

  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf("All parser threads have finished in %s. Joining stats...\n", timeUsedBuffer);
  grdu_mono_timer_reset(&timeUsed);

  JsonStats stats = {0};
  StorageStats *storage_stats = storage_stats_create(meta_alloc);
  if (!storage_stats) fatal(ERROR_MEMORY, "Failed to allocate merged storage statistics.");
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    json_stats_add(&stats, &parser_args[i].stats);
    storage_stats_merge(storage_stats, parser_args[i].storage_stats);
    storage_stats_destroy(parser_args[i].storage_stats);
  }

  json_stats_print(&stats);
  storage_stats_print(storage_stats);

  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf("Joining stats finished in %s, writing SQL file...\n", timeUsedBuffer);
  grdu_mono_timer_reset(&timeUsed);

  // storage_stats_write_sql(storage_stats, output_filename);

  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf("Writing SQL file finished in %s.\n", timeUsedBuffer);
  grdu_mono_timer_reset(&timeUsed);

  printf("Cleaning up...\n");
  storage_stats_destroy(storage_stats);

  meta_area_allocator_destroy(meta_alloc);
  parse_queue_destroy(parse_queue);
  buffer_pool_destroy(&buffer_pool);
  free(outputBuffer);
  free(inputBuffer);

  ZSTD_freeDStream(dstream);

  fclose(fp);

  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsedAll);
  printf("All finished in %s.\n", timeUsedBuffer);
  return 0;
}
