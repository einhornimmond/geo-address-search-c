#include "client.h"
#include "error.h"
#include "format.h"
#include "geo_index.h"
#include "house_collector.h"
#include "json_parse.h"
#include "json_stats.h"
#include "line_buffer.h"
#include "meta_area_allocator.h"
#include "name_collector.h"
#include "parse_queue.h"
#include "progress.h"
#include "text_tokenize.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "gradido_blockchain_core/utils/converter.h"
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

/** What a walk over the dump is for. */
typedef enum ParserPass {
  PARSER_PASS_VOCABULARY, /**< Gather the words and the written spellings. */
  PARSER_PASS_DOCUMENTS,  /**< Write documents and postings against them. */
  PARSER_PASS_HOUSES      /**< Hang the house numbers on the streets they name. */
} ParserPass;

typedef struct ParserThreadArgs {
  ParseQueue *queue;
  BufferPool *pool;
  ParserPass pass;
  MetaAreaAllocator *meta_alloc; /**< Private arena — the allocator is not thread-safe. */
  NameCollector words;           /**< Folded search words of this thread. */
  NameCollector display;         /**< The same places as they are written. */
  TextTokenizer tokenizer;       /**< Folding scratch space, one per thread. */
  NameRun word_run;              /**< Words, sorted once the queue ran dry. */
  NameRun display_run;           /**< Spellings, likewise. */
  grd_result finish_result;      /**< Outcome of the thread's own sorting pass. */
  const NameSet *word_set;       /**< Second pass: where a word finds its rank. */
  const NameSet *display_set;    /**< Second pass: where a spelling finds its rank. */
  DocCollector documents;        /**< Second pass: what this thread found. */
  grd_result document_result;    /**< First failure while collecting documents. */
  const DocSet *doc_set;         /**< Third pass: where a street became a document. */
  HouseCollector houses;         /**< Third pass: the numbers this thread met. */
  grd_result house_result;       /**< First failure while collecting houses. */
  JsonStats stats;
} ParserThreadArgs;

/** Look a written form up; absent text and unknown text both mean "no rank". */
static uint32_t display_rank(const NameSet *set, PhotonString text) {
  size_t rank = 0;
  if (!text.data || !text.size) return GEO_RANK_NONE;
  if (!name_set_rank(set, text.data, text.size, &rank)) return GEO_RANK_NONE;
  return (uint32_t)rank;
}

/** Photon's weight is a fraction; the record keeps it as 1/65535 steps. */
static uint16_t quantize_importance(double importance) {
  if (!(importance > 0.0)) return 0; /* also catches NaN */
  if (importance >= 1.0) return UINT16_MAX;
  return (uint16_t)(importance * (double)UINT16_MAX + 0.5);
}

/** First pass: every text of the entry joins the vocabulary. */
static void collect_vocabulary(ParserThreadArgs *args, const PhotonPlace *place) {
  for (uint8_t i = 0; i < place->search_count; ++i) {
    size_t tokens = text_tokenize(&args->tokenizer, place->search[i].data, place->search[i].size);
    for (size_t t = 0; t < tokens; ++t) {
      const TextToken *token = &args->tokenizer.tokens[t];
      grd_result result = name_collector_add(&args->words, token->data, token->size);
      if (result != GRD_SUCCESS) {
        fatal(ERROR_MEMORY, "Failed to keep search term (grd_result %d).", (int)result);
      }
    }
  }
  /* An answer shows the street, the number, the code and the town — never the
     name of the house itself.  So a numbered entry contributes what the third
     pass must recognise by rank, and its own name goes nowhere: a planet's
     worth of building names would swell the dictionary by twenty million
     entries nobody ever reads. */
  const PhotonString written[] = {
      place->own_name, place->city, place->postcode, place->street, place->house
  };
  size_t first = place->house.data ? 1 : 0;
  size_t count = place->house.data ? sizeof(written) / sizeof(written[0]) : 3;
  for (size_t i = first; i < count; ++i) {
    grd_result result = name_collector_add(&args->display, written[i].data, written[i].size);
    if (result != GRD_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to keep display text (grd_result %d).", (int)result);
    }
  }
}

/** Third pass: the house finds its street and hangs its number there. */
static void collect_house(ParserThreadArgs *args, const PhotonPlace *place) {
  if (!place->house.data) return; /* whatever the hierarchy calls it, it is no address */

  uint32_t number = display_rank(args->display_set, place->house);
  if (number == GEO_RANK_NONE) {
    ++args->houses.without_number;
    ++args->houses.homeless;
    return;
  }
  uint32_t street = display_rank(args->display_set, place->street);
  if (street == GEO_RANK_NONE) {
    ++args->houses.unknown_street;
    ++args->houses.homeless;
    return;
  }
  int relaxed = 0;
  uint32_t document = doc_set_find_street(
      args->doc_set, street, display_rank(args->display_set, place->city),
      display_rank(args->display_set, place->postcode), place->lat_e7, place->lon_e7,
      place->has_point, &relaxed
  );
  if (document == GEO_RANK_NONE) {
    ++args->houses.unknown_key; /* the name is known, the combination is not */
    ++args->houses.homeless;
    return;
  }
  if (relaxed == 1) ++args->houses.recovered_city;
  if (relaxed == 2) ++args->houses.recovered_postcode;
  if (relaxed == 3) ++args->houses.recovered_nearest;

  grd_result result = house_collector_add(
      &args->houses, document, &args->doc_set->documents[document], number, place->lat_e7,
      place->lon_e7, place->has_point
  );
  if (result != GRD_SUCCESS && args->house_result == GRD_SUCCESS) { args->house_result = result; }
}

/** Second pass: the entry becomes a document, and its words point at it. */
static void collect_document(ParserThreadArgs *args, const PhotonPlace *place) {
  /* an address hangs on its street; a house-level entry without a number is a
     named thing, not a place one searches an address for */
  if (place->house.data || place->typeEnum == PHOTON_PLACE_TYPE_HOUSE) return;

  GeoDocument document = {
      .lat_e7 = place->lat_e7,
      .lon_e7 = place->lon_e7,
      .name_rank = display_rank(args->display_set, place->own_name),
      .city_rank = display_rank(args->display_set, place->city),
      .postcode_rank = display_rank(args->display_set, place->postcode),
      .importance = quantize_importance(place->importance),
      .type = (uint8_t)place->typeEnum,
      .flags = place->has_point ? GEO_DOCUMENT_HAS_POINT : 0u,
  };

  uint32_t number = 0;
  grd_result result = doc_collector_add_document(&args->documents, &document, &number);
  if (result != GRD_SUCCESS) {
    if (args->document_result == GRD_SUCCESS) args->document_result = result;
    return;
  }

  for (uint8_t i = 0; i < place->search_count; ++i) {
    size_t tokens = text_tokenize(&args->tokenizer, place->search[i].data, place->search[i].size);
    for (size_t t = 0; t < tokens; ++t) {
      const TextToken *token = &args->tokenizer.tokens[t];
      size_t rank = 0;
      if (!name_set_rank(args->word_set, token->data, token->size, &rank)) {
        ++args->documents.dropped_words; /* the first pass saw every word — this cannot happen */
        continue;
      }
      result = doc_collector_add_posting(&args->documents, (uint32_t)rank);
      if (result != GRD_SUCCESS && args->document_result == GRD_SUCCESS) {
        args->document_result = result;
        return;
      }
    }
  }
}

static int process_place_callback(const PhotonPlace *place, void *user_data) {
  ParserThreadArgs *args = user_data;
  switch (args->pass) {
  case PARSER_PASS_VOCABULARY:
    json_stats_count_place(&args->stats, place);
    collect_vocabulary(args, place);
    break;
  case PARSER_PASS_DOCUMENTS:
    collect_document(args, place);
    break;
  case PARSER_PASS_HOUSES:
    collect_house(args, place);
    break;
  }
  return 0;
}

static void process_batch(const ParseBatch *batch, ParserThreadArgs *args) {
  const char *line = batch->buffer->buffer;
  const char *end = line + batch->len;

  while (line < end) {
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    size_t len = newline ? (size_t)(newline - line) : (size_t)(end - line);
    if (len > 0 && line[len - 1] == '\r') { --len; }
    if (len > 0) {
      JsonParseResult result;
      json_parse_line(line, len, process_place_callback, args, &result);
      if (args->pass == PARSER_PASS_VOCABULARY) json_stats_count_document(&args->stats, &result);
    }
    line = newline ? newline + 1 : end;
  }
}

static void *parser_thread(void *arg) {
  ParserThreadArgs *args = arg;
  ParseBatch batch;
  while (parse_queue_pop(args->queue, &batch)) {
    process_batch(&batch, args);
    buffer_pool_release(args->pool, batch.buffer);
  }
  if (args->pass != PARSER_PASS_VOCABULARY) return NULL;

  /* the queue has run dry — sort what this thread gathered while the others
     do the same, so the join finds nothing but ordered runs */
  args->finish_result = name_collector_finish(&args->words, &args->word_run);
  if (args->finish_result == GRD_SUCCESS) {
    args->finish_result = name_collector_finish(&args->display, &args->display_run);
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

/**
 * @brief Decompress the whole dump once and hand every line to the queue.
 *
 *  Rewinds the file and restarts the stream, so the same dump can be walked
 *  again for the second pass.  Closes the queue when the last line is in.
 */
static void stream_dump(
    FILE *fp,
    ZSTD_DStream *dstream,
    char *inputBuffer,
    size_t inputSize,
    void *outputBuffer,
    size_t outputSize,
    ParseQueue *queue,
    BufferPool *pool
) {
  rewind(fp);
  size_t ret = ZSTD_initDStream(dstream);
  if (ZSTD_isError(ret)) { fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret)); }

  ZSTD_inBuffer input = {.src = inputBuffer, .size = 0, .pos = 0};
  ZSTD_outBuffer output = {.dst = outputBuffer, .size = outputSize, .pos = 0};
  LineBuffer *lineBuffer = buffer_pool_acquire(pool);

  while (1) {
    size_t read = fread(inputBuffer, 1, inputSize, fp);
    progress_update(ftell(fp));
    if (read == 0) { break; }
    input.size = read;
    input.pos = 0;

    while (input.pos < input.size || output.pos == output.size) {
      ret = ZSTD_decompressStream(dstream, &output, &input);
      if (ZSTD_isError(ret)) { fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret)); }

      line_buffer_append(lineBuffer, (char *)output.dst, output.pos);
      enqueue_complete_lines(queue, pool, &lineBuffer);
      output.pos = 0;

      if (0 == ret) { break; } /* fully flushed */
    }
  }

  if (lineBuffer->position > 0) {
    parse_queue_push(queue, (ParseBatch){.buffer = lineBuffer, .len = lineBuffer->position});
  } else {
    buffer_pool_release(pool, lineBuffer);
  }
  parse_queue_close(queue);
}

/**
 * @brief Run one pass: threads up, dump through, threads joined.
 *
 *  The queue lives only as long as the pass — it closes once and cannot
 *  reopen, so the second walk gets a fresh one.  Everything a thread keeps
 *  across passes sits in its @ref ParserThreadArgs and is untouched here.
 */
static void run_pass(
    ParserPass pass,
    FILE *fp,
    ZSTD_DStream *dstream,
    char *inputBuffer,
    size_t inputSize,
    void *outputBuffer,
    size_t outputSize,
    BufferPool *pool,
    ParserThreadArgs *args,
    pthread_t *threads,
    unsigned thread_count,
    uint64_t totalBytes
) {
  ParseQueue *queue = parse_queue_create();
  if (!queue) { fatal(ERROR_MEMORY, "Failed to allocate the parse queue."); }

  for (unsigned i = 0; i < thread_count; ++i) {
    args[i].queue = queue;
    args[i].pool = pool;
    args[i].pass = pass;
    if (pthread_create(&threads[i], NULL, parser_thread, &args[i]) != 0) {
      fatal(ERROR_MEMORY, "Failed to create parser thread %u.", i);
    }
  }

  progress_init(totalBytes);
  stream_dump(fp, dstream, inputBuffer, inputSize, outputBuffer, outputSize, queue, pool);
  printf("\n");
  progress_finish();

  for (unsigned i = 0; i < thread_count; ++i) { pthread_join(threads[i], NULL); }
  parse_queue_destroy(queue);
}

/**
 * @brief Walk a compressed dump and leave a finished index behind.
 *
 *  The long way round: decompress, parse, fold, sort, merge, write.  Every
 *  later start takes the short way and only maps the result.
 *
 *  @param[in] dump_path           Photon JSONL dump, zstd compressed.
 *  @param[in] index_path          Destination for the index file.
 *  @param[in] parser_thread_count Parser threads, 1 … 10.
 *  @return 0 on success; failures end the program through fatal().
 */
static int build_index(
    const char *dump_path, const char *index_path, unsigned parser_thread_count
) {
  grdu_mono_timer timeUsedAll;
  grdu_mono_timer_reset(&timeUsedAll);

  FILE *fp = fopen(dump_path, "rb");
  if (!fp) { fatal(ERROR_IO, "Cannot open '%s'.", dump_path); }

  ZSTD_DStream *dstream = ZSTD_createDStream();
  if (!dstream) { fatal(ERROR_MEMORY, "Failed to create ZSTD_DStream."); }

  const size_t inputSize = ZSTD_DStreamInSize() * 2;
  const size_t outputSize = ZSTD_DStreamOutSize() * 2;

  char *inputBuffer = malloc(inputSize);
  void *outputBuffer = malloc(outputSize);
  if (!inputBuffer || !outputBuffer) {
    fatal(ERROR_MEMORY, "Failed to allocate streaming buffers.");
  }

  pthread_t parser_threads[10];
  ParserThreadArgs parser_args[10] = {0};
  BufferPool buffer_pool;

  char inputSizeBuf[32], outputSizeBuf[32];
  format_byte_units(inputSizeBuf, sizeof(inputSizeBuf), inputSize, 2);
  format_byte_units(outputSizeBuf, sizeof(outputSizeBuf), outputSize, 2);
  printf("inputSize: %s, outputSize: %s\n", inputSizeBuf, outputSizeBuf);

  buffer_pool_init(&buffer_pool, outputSize * 2);
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    /* One arena per thread — meta_area_alloc() bumps without a lock, so it must
       not be shared. The arenas outlive the threads: they hold the text bytes. */
    parser_args[i].meta_alloc = meta_area_allocator_create();
    if (!parser_args[i].meta_alloc) {
      fatal(ERROR_MEMORY, "Failed to create meta area allocator for parser thread %u.", i);
    }
    if (name_collector_init(&parser_args[i].words, parser_args[i].meta_alloc) != GRD_SUCCESS ||
        name_collector_init(&parser_args[i].display, parser_args[i].meta_alloc) != GRD_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init collectors for parser thread %u.", i);
    }
    text_tokenizer_init(&parser_args[i].tokenizer);
  }

  fseek(fp, 0, SEEK_END);
  uint64_t totalBytes = ftell(fp);

  grdu_mono_timer timeUsed;
  char timeUsedBuffer[32];

  /* =======================================================================
   *  First pass: what words exist at all
   * ======================================================================= */

  printf("Pass 1 of 3: gathering the vocabulary\n");
  run_pass(
      PARSER_PASS_VOCABULARY, fp, dstream, inputBuffer, inputSize, outputBuffer, outputSize,
      &buffer_pool, parser_args, parser_threads, parser_thread_count, totalBytes
  );
  grdu_mono_timer_reset(&timeUsed);

  JsonStats stats = {0};
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    json_stats_add(&stats, &parser_args[i].stats);
  }
  json_stats_print(&stats);

  /* --- the sorted per-thread runs flow together and lose their last doubles --- */
  const NameRun *word_runs[10];
  const NameRun *display_runs[10];
  size_t meta_allocated = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].finish_result != GRD_SUCCESS) {
      fatal(
          ERROR_MEMORY, "Parser thread %u failed to sort its texts (grd_result %d).", i,
          (int)parser_args[i].finish_result
      );
    }
    word_runs[i] = &parser_args[i].word_run;
    display_runs[i] = &parser_args[i].display_run;
    meta_allocated += meta_area_total_allocated(parser_args[i].meta_alloc);
  }

  NameSet words, display;
  grd_result merge_result =
      name_run_merge(&words, word_runs, parser_thread_count, parser_thread_count);
  if (merge_result == GRD_SUCCESS) {
    merge_result = name_run_merge(&display, display_runs, parser_thread_count, parser_thread_count);
  }
  if (merge_result != GRD_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to merge the collected texts (grd_result %d).", (int)merge_result);
  }

  char nameBytesBuffer[32];
  format_byte_units(nameBytesBuffer, sizeof(nameBytesBuffer), meta_allocated, 2);
  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  uint64_t inputs = 0, repeated = 0, dropped = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    inputs += parser_args[i].tokenizer.inputs;
    repeated += parser_args[i].tokenizer.repeated;
    dropped += parser_args[i].tokenizer.dropped;
  }
  printf("\nTexts: %" PRIu64 " offered, %" PRIu64 " skipped as repetitions\n", inputs, repeated);
  printf("Words: %zu seen, %zu distinct\n", words.total, words.count);
  printf("Spellings: %zu seen, %zu distinct\n", display.total, display.count);
  printf("  text memory: %s, sorted and joined in %s\n", nameBytesBuffer, timeUsedBuffer);
  if (dropped) { printf("  dropped (no room): %" PRIu64 "\n", dropped); }
  char treeBytesBuffer[32];
  format_byte_units(
      treeBytesBuffer, sizeof(treeBytesBuffer), prefix_tree_memory(&words.prefixes), 2
  );
  printf(
      "Prefix groups: %zu (index tree: depth %u, %zu levels, %s)\n", words.group_count,
      NAME_PREFIX_DEPTH, words.prefixes.levels, treeBytesBuffer
  );

  /* the runs have handed their words to the merged sets */
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    name_run_free(&parser_args[i].word_run);
    name_run_free(&parser_args[i].display_run);
  }

  /* =======================================================================
   *  Second pass: places, and which words point at them
   * ======================================================================= */

  printf("\nPass 2 of 3: documents and posting lists\n");
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    parser_args[i].word_set = &words;
    parser_args[i].display_set = &display;
    parser_args[i].document_result = GRD_SUCCESS;
    if (doc_collector_init(&parser_args[i].documents) != GRD_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init document collector for parser thread %u.", i);
    }
    /* every occurrence counts now — a posting belongs to its document even
       when the same text came by a moment ago */
    parser_args[i].tokenizer.repetition_filter = 0;
  }

  run_pass(
      PARSER_PASS_DOCUMENTS, fp, dstream, inputBuffer, inputSize, outputBuffer, outputSize,
      &buffer_pool, parser_args, parser_threads, parser_thread_count, totalBytes
  );
  grdu_mono_timer_reset(&timeUsed);

  DocCollector *doc_collectors[10];
  uint64_t unknown_words = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].document_result != GRD_SUCCESS) {
      fatal(
          ERROR_MEMORY, "Parser thread %u failed to collect documents (grd_result %d).", i,
          (int)parser_args[i].document_result
      );
    }
    doc_collectors[i] = &parser_args[i].documents;
    unknown_words += parser_args[i].documents.dropped_words;
  }

  DocSet documents;
  grd_result doc_result =
      doc_collector_merge(&documents, doc_collectors, parser_thread_count, words.count);
  if (doc_result != GRD_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to join the documents (grd_result %d).", (int)doc_result);
  }
  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf(
      "Documents: %zu from %zu segments, postings: %zu — joined in %s\n", documents.document_count,
      documents.segment_count, documents.posting_count, timeUsedBuffer
  );
  if (unknown_words) {
    printf("  words without a rank: %" PRIu64 " (should be 0)\n", unknown_words);
  }
  grdu_mono_timer_reset(&timeUsed);

  /* =======================================================================
   *  Third pass: the house numbers, onto the streets that now exist
   * ======================================================================= */

  printf("\nPass 3 of 3: house numbers\n");
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    parser_args[i].doc_set = &documents;
    parser_args[i].house_result = GRD_SUCCESS;
    if (house_collector_init(&parser_args[i].houses) != GRD_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init house collector for parser thread %u.", i);
    }
  }

  run_pass(
      PARSER_PASS_HOUSES, fp, dstream, inputBuffer, inputSize, outputBuffer, outputSize,
      &buffer_pool, parser_args, parser_threads, parser_thread_count, totalBytes
  );
  grdu_mono_timer_reset(&timeUsed);

  HouseCollector *house_collectors[10];
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].house_result != GRD_SUCCESS) {
      fatal(
          ERROR_MEMORY, "Parser thread %u failed to collect houses (grd_result %d).", i,
          (int)parser_args[i].house_result
      );
    }
    house_collectors[i] = &parser_args[i].houses;
  }

  HouseSet houses;
  grd_result house_result = house_collector_merge(
      &houses, house_collectors, parser_thread_count, documents.document_count
  );
  if (house_result != GRD_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to join the houses (grd_result %d).", (int)house_result);
  }
  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf(
      "House numbers: %zu on %zu streets — ordered in %s\n", houses.house_count,
      documents.street_count, timeUsedBuffer
  );
  if (houses.homeless) {
    printf(
        "  without a street in the index: %" PRIu64 " (%.2f %%)\n", houses.homeless,
        100.0 * (double)houses.homeless / (double)(houses.homeless + houses.house_count)
    );
    printf(
        "    of those without a number: %" PRIu64 ", street name unknown: %" PRIu64
        ", combination unknown: %" PRIu64 "\n",
        houses.without_number, houses.unknown_street, houses.unknown_key
    );
  }
  if (houses.recovered_city || houses.recovered_postcode || houses.recovered_nearest) {
    printf(
        "  found only without the postal code: %" PRIu64 ", only by name and town: %" PRIu64
        ", only by proximity: %" PRIu64 "\n",
        houses.recovered_city, houses.recovered_postcode, houses.recovered_nearest
    );
  }
  if (houses.pointless) {
    printf("  without a coordinate of their own: %" PRIu64 "\n", houses.pointless);
  }
  grdu_mono_timer_reset(&timeUsed);

  /* --- the result lies down in the shape it will be read in --- */
  grd_result write_result =
      geo_index_write(index_path, &words, &display, &documents, &houses, words.total);
  if (write_result != GRD_SUCCESS) {
    fatal(ERROR_IO, "Failed to write index '%s' (grd_result %d).", index_path, (int)write_result);
  }
  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  printf("Index written to '%s' in %s\n", index_path, timeUsedBuffer);

  printf("Cleaning up...\n");

  house_set_free(&houses);
  doc_set_free(&documents);
  name_set_free(&words);
  name_set_free(&display);
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    house_collector_free(&parser_args[i].houses);
    doc_collector_free(&parser_args[i].documents);
    name_collector_free(&parser_args[i].words);
    name_collector_free(&parser_args[i].display);
    meta_area_allocator_destroy(parser_args[i].meta_alloc);
  }
  buffer_pool_destroy(&buffer_pool);
  free(outputBuffer);
  free(inputBuffer);
  ZSTD_freeDStream(dstream);
  fclose(fp);

  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsedAll);
  printf("All finished in %s.\n", timeUsedBuffer);
  return 0;
}

/* =========================================================================
 *  The short way: a finished index
 * ========================================================================= */

/**
 * @brief Map a finished index and report what it holds.
 *
 *  Mapping is constant time — the numbers below appear before the disk has
 *  been touched beyond the header.
 *
 *  @param[in] index_path  File written by a previous build.
 *  @return 0 on success; a failed open ends the program through fatal().
 */
/** Print one borrowed text, or a placeholder when the entry never carried it. */
static void print_text(const char *text, size_t size) {
  if (!text || !size) {
    printf("—");
    return;
  }
  printf("%.*s", (int)size, text);
}

/** Show what a query found: the place as it is written, and where it lies. */
static void print_results(const GeoAddress *found, size_t count, const char *query) {
  if (!count) {
    printf("No results for '%s'.\n", query);
    return;
  }
  printf("Results for '%s':\n", query);
  for (size_t i = 0; i < count; ++i) {
    const GeoAddress *address = &found[i];
    printf("  ");
    print_text(address->name, address->name_size);
    if (address->number) {
      printf(" ");
      print_text(address->number, address->number_size);
    }
    printf(", ");
    print_text(address->postcode, address->postcode_size);
    printf(" ");
    print_text(address->city, address->city_size);
    if (address->has_point) {
      printf("  →  %.7f, %.7f", address->latitude, address->longitude);
    } else {
      printf("  →  no coordinate");
    }
    printf("  (kind %u, weight %u)\n", address->kind, address->importance);
  }
}

/**
 * @brief Write a count with its thousands set apart: 1850000 becomes "1 850 000".
 *
 *  @param[out] buffer  Destination; 27 bytes hold the longest count there is —
 *                      twenty digits, six separators and the NUL.  A shorter
 *                      one is filled as far as it reaches and terminated.
 *  @param[in]  size    Capacity of @p buffer, at least 1.
 *  @param[in]  value   Count to spell out.
 */
static void format_grouped(char *buffer, size_t size, uint64_t value) {
  char digits[24];
  size_t length = grdu_uint64_to_string(digits, sizeof(digits), value);
  size_t written = 0;
  for (size_t i = 0; i < length && written + 2 < size; ++i) {
    if (i && (length - i) % 3 == 0) buffer[written++] = ' ';
    buffer[written++] = digits[i];
  }
  buffer[written] = '\0';
}

/** One line of the query report: a label, and its count in a column of its own. */
static void print_count(const char *label, uint64_t value) {
  char grouped[32];
  format_grouped(grouped, sizeof(grouped), value);
  printf("  %-22s %13s\n", label, grouped);
}

/**
 * @brief Show what the search had to touch on its way to the answer.
 *
 *  A duration says only that a query was slow; these counts say where it went.
 *  A beginning that covers thousands of dictionary words opens thousands of
 *  posting lists, and the documents in them are read before a single word has
 *  narrowed anything — that sum, set against the handful that survives the
 *  narrowing, is where a slow query usually shows itself.
 *
 *  Counts of the whole search stand beside counts of one pass: the sums hold
 *  every reading the query needed, while the words that narrowed and what was
 *  left afterwards describe the reading that answered.
 *
 *  @param[in] stats     Counts of the search just run.
 *  @param[in] duration  How long it took, already formatted.
 */
static void print_query_stats(const GeoQueryStats *stats, const char *duration) {
  printf("\nSearched in %s\n", duration);
  print_count("passes:", stats->passes);
  print_count("words that narrowed:", stats->groups);
  print_count("prefix terms seen:", stats->prefix_terms);
  print_count("beginnings refused:", stats->prefix_refused);
  print_count("posting lists read:", stats->posting_lists);
  print_count("documents in them:", stats->posting_documents);
  print_count("left after narrowing:", stats->narrowed);
  print_count("candidates ranked:", stats->weighed);
  print_count("results:", stats->results);
}

/**
 * @brief Map a finished index and report what it holds.
 *
 *  Everything here goes through the client library — the same door a server
 *  would use, so what the command line shows is what an embedder gets.
 *
 *  @param[in] index_path    File written by a previous build.
 *  @param[in] query         Free text to search for, or NULL for counts only.
 *  @param[in] result_limit  Most results to show.
 *  @param[in] prefix        Read the last word as a beginning as well.
 *  @return 0 on success; a failed open ends the program through fatal().
 */
static int open_index(const char *index_path, const char *query, size_t result_limit, bool prefix) {
  grdu_mono_timer timeUsed;
  grdu_mono_timer_init();
  grdu_mono_timer_reset(&timeUsed);

  GeoClient *client = NULL;
  GeoStatus status = geo_client_open(&client, index_path);
  if (status != GEO_OK) {
    fatal(ERROR_IO, "Cannot open index '%s' (GeoStatus %d).", index_path, (int)status);
  }

  GeoClientInfo info;
  geo_client_info(client, &info);

  char timeUsedBuffer[32], sizeBuffer[32];
  grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  format_byte_units(sizeBuffer, sizeof(sizeBuffer), info.file_size, 2);

  printf("Index '%s' opened in %s\n", index_path, timeUsedBuffer);
  printf("  file:           %s (format %u)\n", sizeBuffer, info.format);
  printf("  words:          %" PRIu64 "\n", info.words);
  printf("  spellings:      %" PRIu64 "\n", info.spellings);
  printf("  documents:      %" PRIu64 "\n", info.documents);
  printf("  house numbers:  %" PRIu64 "\n", info.houses);
  printf("  postings:       %" PRIu64 "\n", info.postings);

  if (query) {
    GeoAddress found[64];
    if (result_limit > sizeof(found) / sizeof(found[0])) {
      result_limit = sizeof(found) / sizeof(found[0]);
    }

    /* the counts are gathered while the query runs; asking for them is what
       makes this a debugging tool rather than only a search */
    GeoQueryStats stats;
    grdu_mono_timer queryTime;
    grdu_mono_timer_reset(&queryTime);
    size_t count =
        geo_client_search_stats(client, query, strlen(query), prefix, found, result_limit, &stats);
    grdu_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), queryTime);

    printf("\n");
    print_results(found, count, query);
    print_query_stats(&stats, timeUsedBuffer);
  }

  geo_client_close(client);
  return 0;
}

/* =========================================================================
 *  Command line
 * ========================================================================= */

static bool has_extension(const char *path, const char *extension) {
  size_t path_size = strlen(path);
  size_t extension_size = strlen(extension);
  if (path_size < extension_size) return false;
  return strcasecmp(path + path_size - extension_size, extension) == 0;
}

/**
 * @brief Derive the index path from the dump path.
 *
 *  `planet.jsonl.zst` becomes `planet.gdx` — the known dump suffixes fall
 *  away, everything else keeps its name.
 */
static void derive_index_path(char *out, size_t size, const char *dump_path) {
  static const char *const suffixes[] = {".jsonl.zst", ".json.zst", ".zst", ".jsonl"};
  size_t length = strlen(dump_path);
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    size_t suffix_size = strlen(suffixes[i]);
    if (length >= suffix_size && strcasecmp(dump_path + length - suffix_size, suffixes[i]) == 0) {
      length -= suffix_size;
      break;
    }
  }
  if (length + strlen(GEO_INDEX_EXTENSION) + 1 > size) {
    fatal(ERROR_USAGE, "Path '%s' is too long to derive an index name from.", dump_path);
  }
  memcpy(out, dump_path, length);
  strcpy(out + length, GEO_INDEX_EXTENSION);
}

/** How many results are shown unless the caller asks for another number. */
#define DEFAULT_RESULT_LIMIT 10

static void print_usage(const char *program) {
  fatal(
      ERROR_USAGE,
      "Usage:\n"
      "  %s <photon_dump.jsonl.zst> [index%s] [parser_threads: 1-10]\n"
      "      Builds the search index from the dump and writes it as a binary file.\n"
      "      Without a destination the name comes from the dump\n"
      "      (planet.jsonl.zst -> planet%s). parser_threads defaults to 4.\n"
      "\n"
      "  %s <index%s> [\"query\"] [max_results]\n"
      "      Maps a finished index, shows its counts and — when a query is given —\n"
      "      the places that carry all of its words, in any order.\n"
      "      The last word counts as still being typed and is read as a beginning\n"
      "      as well: \"Marienpl\" finds \"Marienplatz\". A trailing space or comma\n"
      "      closes it and searches it exactly as it stands.\n"
      "      max_results defaults to %d.\n"
      "\n"
      "The path decides which way it goes: a file ending in %s is loaded,\n"
      "anything else is built.\n"
      "\n"
      "Examples:\n"
      "  %s planet.jsonl.zst 8\n"
      "  %s planet%s \"Berlin, Superstrasse\"\n"
      "  %s planet%s \"15328 Bleyen\" 5\n"
      "  %s planet%s \"Berlin Marienpl\"     (still typing)\n"
      "  %s planet%s \"Berlin Marienplatz \" (finished)",
      program, GEO_INDEX_EXTENSION, GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION,
      DEFAULT_RESULT_LIMIT, GEO_INDEX_EXTENSION, program, program, GEO_INDEX_EXTENSION, program,
      GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION
  );
}

/** Read a positive count, or end the program saying what was wrong. */
static unsigned parse_count(const char *text, unsigned low, unsigned high, const char *name) {
  char *end;
  unsigned long value = strtoul(text, &end, 10);
  if (*text == '\0' || *end != '\0' || value < low || value > high) {
    fatal(ERROR_USAGE, "%s must be between %u and %u.", name, low, high);
  }
  return (unsigned)value;
}

int main(int argc, char *argv[]) {
  if (argc < 2 || argc > 4) { print_usage(argv[0]); }

  const char *input = argv[1];
  if (has_extension(input, GEO_INDEX_EXTENSION)) {
    const char *query = argc >= 3 ? argv[2] : NULL;
    unsigned limit = argc == 4 ? parse_count(argv[3], 1, 64, "max_treffer") : DEFAULT_RESULT_LIMIT;

    /* A word is still being typed unless something closed it: a trailing space
       or comma says the writer is done with it, and then it is read as it
       stands rather than as a beginning. */
    bool prefix = true;
    if (query && *query) {
      char last = query[strlen(query) - 1];
      prefix = last != ' ' && last != ',' && last != ';';
    }
    return open_index(input, query, limit, prefix);
  }

  /* `dump.jsonl.zst 8` means eight threads, not a file called "8" */
  bool second_is_count = argc == 3 && argv[2][strspn(argv[2], "0123456789")] == '\0';

  char derived[4096];
  const char *index_path;
  if (argc >= 3 && !second_is_count) {
    index_path = argv[2];
  } else {
    derive_index_path(derived, sizeof(derived), input);
    index_path = derived;
  }
  unsigned parser_thread_count = 4;
  if (second_is_count) {
    parser_thread_count = parse_count(argv[2], 1, 10, "parser_threads");
  } else if (argc == 4) {
    parser_thread_count = parse_count(argv[3], 1, 10, "parser_threads");
  }

  grdu_mono_timer_init();
  return build_index(input, index_path, parser_thread_count);
}
