/** @file
 *  @brief The builder and the command line — the one artifact with an entry point.
 *
 *  Two programs share this file, and the extension of the first argument decides
 *  which one runs: a dump goes in and an index comes out, or an index goes in and
 *  an answer comes out.  Everything below is the plumbing between the modules —
 *  the buffer pool the reader draws from, the threads that parse, the three walks
 *  over the dump, and the printing of what was found.
 *
 *  The client library links none of it.  What leaves the house is @ref client.
 */

/** @cond INTERNAL */

#include "foundation/error.h"
#include "foundation/format.h"
#include "foundation/line_buffer.h"
#include "foundation/progress.h"
#include "parser/json_parse.h"
#include "parser/json_stats.h"
#include "parser/parse_queue.h"
#include "parser/place_cache.h"
#include "search/client.h"
#include "search/geo_cell.h"
#include "search/geo_index.h"
#include "search/house_collector.h"
#include "search/name_collector.h"
#include "search/text_tokenize.h"

#include "arnm/multi_arena.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <zstd.h>

/* The parser and the file must mean the same thing by a language tag, or the
   builder would write eight bytes the reader cuts at seven. */
static_assert(PHOTON_LANGUAGE_TAG_MAX == GEO_LANGUAGE_TAG_MAX, "language tag width drifted");
static_assert(PHOTON_LANGUAGE_MAX <= GEO_LANGUAGE_MAX, "more languages than a file may hold");

#include "arnm/converter.h"
#include "arnm/duration.h"
#include "arnm/mono_timer.h"

/** Buffers in the pool: what the queue can hold, plus the few in flight around it. */
enum { BUFFER_POOL_CAPACITY = PARSE_QUEUE_CAPACITY + 4 };

/**
 * @brief The line buffers, lent out and handed back rather than allocated.
 *
 *  A dump of 24 GB moves through a fixed handful of chambers.  The reader takes
 *  one, fills it, sends it down the queue, and the parser that empties it puts it
 *  back — so the heap is touched once at startup and never again on the hot path.
 */
typedef struct BufferPool {
  LineBuffer *buffers[BUFFER_POOL_CAPACITY]; /**< The free ones; a stack, not a queue. */
  size_t count;                              /**< How many of them are free. */
  pthread_mutex_t mutex;                     /**< Guards @c buffers and @c count. */
  pthread_cond_t available;                  /**< A borrower waits here when none is free. */
} BufferPool;

/** Fill the pool with @ref BUFFER_POOL_CAPACITY buffers of @p buffer_size bytes each. */
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

/** Take a buffer, waiting while every one of them is out. */
static LineBuffer *buffer_pool_acquire(BufferPool *pool) {
  pthread_mutex_lock(&pool->mutex);
  while (pool->count == 0) { pthread_cond_wait(&pool->available, &pool->mutex); }
  LineBuffer *buffer = pool->buffers[--pool->count];
  pthread_mutex_unlock(&pool->mutex);
  return buffer;
}

/** Give a buffer back, emptied and ready for the next block. */
static void buffer_pool_release(BufferPool *pool, LineBuffer *buffer) {
  line_buffer_reset(buffer);
  pthread_mutex_lock(&pool->mutex);
  pool->buffers[pool->count++] = buffer;
  pthread_cond_signal(&pool->available);
  pthread_mutex_unlock(&pool->mutex);
}

/** Close the pool down.  Every buffer must be back, or it is not freed. */
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

/** How a pass gets its entries. */
typedef enum PassSource {
  PASS_FROM_DUMP, /**< Unpack the dump and parse it, as it has always been. */
  PASS_FROM_CACHE /**< Read the binary cache each thread wrote for itself. */
} PassSource;

typedef struct ParserThreadArgs {
  ParseQueue *queue;
  BufferPool *pool;
  ParserPass pass;
  const PhotonLanguages *languages; /**< Readings this build keeps; never NULL. */
  PassSource source;                /**< Where this pass takes its entries from. */
  unsigned thread;                  /**< This thread's number, and its cache file's. */
  const char *cache_directory;      /**< Where the cache lies, or NULL. */
  PlaceCacheWriter *cache_write;    /**< Set while the first pass fills the cache. */
  arnm_result cache_result;         /**< First failure of the cache, either way. */
  _Atomic uint64_t cache_bytes;     /**< Cache bytes this thread has read, for the progress. */
  _Atomic bool cache_finished;      /**< Set when this thread has read its last record. */
  arnm *names;                      /**< Private arena chain — a chain is not thread-safe. */
  NameCollector words;              /**< Folded search words of this thread. */
  NameCollector display;            /**< The same places as they are written. */
  TextTokenizer tokenizer;          /**< Folding scratch space, one per thread. */
  NameRun word_run;                 /**< Words, sorted once the queue ran dry. */
  NameRun display_run;              /**< Spellings, likewise. */
  arnm_result finish_result;        /**< Outcome of the thread's own sorting pass. */
  const NameSet *word_set;          /**< Second pass: where a word finds its rank. */
  const NameSet *display_set;       /**< Second pass: where a spelling finds its rank. */
  DocCollector documents;           /**< Second pass: what this thread found. */
  arnm_result document_result;      /**< First failure while collecting documents. */
  const DocSet *doc_set;            /**< Third pass: where a street became a document. */
  HouseCollector houses;            /**< Third pass: the numbers this thread met. */
  arnm_result house_result;         /**< First failure while collecting houses. */
  JsonStats stats;
} ParserThreadArgs;

/** Look a written form up; absent text and unknown text both mean "no rank". */
/** A text's place in the display dictionary, or GEO_RANK_NONE where it has none. */
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

/**
 * @brief Will this entry become a document of its own?
 *
 *  A house hangs on its street and is no answer by itself; everything else is.
 *  Both passes have to agree on this, or the second would look for a word the
 *  first never wrote — so they ask the same question here rather than each
 *  spelling out its own.
 */
static bool place_becomes_document(const PhotonPlace *place) {
  return !place->house.data && place->typeEnum != PHOTON_PLACE_TYPE_HOUSE;
}

/**
 * @brief The word that says where a place stands, or nothing for a place adrift.
 *
 *  Only a document that will be searched for gets one, and only where the dump
 *  gave it a coordinate — a cell word on an entry nobody can find is a posting
 *  spent on nothing.
 *
 *  @param[out] buffer  At least @ref GEO_CELL_TOKEN_SIZE bytes.
 *  @return Bytes written, or 0 when this place carries no position.
 */
static size_t place_cell_token(char *buffer, const PhotonPlace *place) {
  if (!place->has_point || !place_becomes_document(place)) return 0;
  return geo_cell_token(buffer, geo_cell_of(place->lat_e7, place->lon_e7));
}

/** First pass: every text of the entry joins the vocabulary. */
static void collect_vocabulary(ParserThreadArgs *args, const PhotonPlace *place) {
  for (uint8_t i = 0; i < place->search_count; ++i) {
    size_t tokens = text_tokenize(&args->tokenizer, place->search[i].data, place->search[i].size);
    for (size_t t = 0; t < tokens; ++t) {
      const TextToken *token = &args->tokenizer.tokens[t];
      arnm_result result = name_collector_add(&args->words, token->data, token->size);
      if (result != ARNM_SUCCESS) {
        fatal(ERROR_MEMORY, "Failed to keep search term (arnm_result %d).", (int)result);
      }
    }
  }
  /* The place's cell joins the search words like any other — it is the one word
     nobody types and every position asks for. */
  char cell[GEO_CELL_TOKEN_SIZE];
  size_t cell_size = place_cell_token(cell, place);
  if (cell_size) {
    arnm_result result = name_collector_add(&args->words, cell, cell_size);
    if (result != ARNM_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to keep cell word (arnm_result %d).", (int)result);
    }
  }

  /* An answer shows the street, the number, the code and the town — never the
     name of the house itself.  So a numbered entry contributes what the third
     pass must recognise by rank, and its own name goes nowhere: a planet's
     worth of building names would swell the dictionary by twenty million
     entries nobody ever reads.
     What decides this is whether the entry becomes a document at all, not
     whether it carries a number.  A house-level entry the dump gave no number
     becomes neither document nor door, and its name is read by nobody either —
     on the German dump alone that was 770 000 spellings and 18 MB of index
     that nothing ever pointed at. */
  const PhotonString written[] = {
      place->own_name, place->city, place->postcode, place->street, place->house
  };
  size_t first = place_becomes_document(place) ? 0 : 1;
  size_t count = place->house.data ? sizeof(written) / sizeof(written[0]) : 3;
  for (size_t i = first; i < count; ++i) {
    arnm_result result = name_collector_add(&args->display, written[i].data, written[i].size);
    if (result != ARNM_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to keep display text (arnm_result %d).", (int)result);
    }
  }

  /* The other readings join the same dictionary — "Prague" is a spelling like
     "Praha", and a spelling two languages share costs one entry, not two.
     Only a place that becomes a document can show them. */
  if (place_becomes_document(place)) {
    for (uint8_t v = 0; v < place->variant_count; ++v) {
      const PhotonString reading[] = {place->variants[v].name, place->variants[v].city};
      for (size_t i = 0; i < sizeof(reading) / sizeof(reading[0]); ++i) {
        arnm_result result = name_collector_add(&args->display, reading[i].data, reading[i].size);
        if (result != ARNM_SUCCESS) {
          fatal(ERROR_MEMORY, "Failed to keep localized text (arnm_result %d).", (int)result);
        }
      }
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

  arnm_result result = house_collector_add(
      &args->houses, document, &args->doc_set->documents[document], number, place->lat_e7,
      place->lon_e7, place->has_point
  );
  if (result != ARNM_SUCCESS && args->house_result == ARNM_SUCCESS) { args->house_result = result; }
}

/** Second pass: the entry becomes a document, and its words point at it. */
static void collect_document(ParserThreadArgs *args, const PhotonPlace *place) {
  /* an address hangs on its street; a house-level entry without a number is a
     named thing, not a place one searches an address for */
  if (!place_becomes_document(place)) return;

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
  arnm_result result = doc_collector_add_document(&args->documents, &document, &number);
  if (result != ARNM_SUCCESS) {
    if (args->document_result == ARNM_SUCCESS) args->document_result = result;
    return;
  }

  /* The readings beside the default one hang on the document just opened.  The
     default itself is already in the record above, which is why the language at
     place 0 never gathers a variant — a caller asking for it is answered by the
     record, and the side table stays as small as the data makes it. */
  for (uint8_t v = 0; v < place->variant_count; ++v) {
    int language = photon_languages_index(args->languages, place->variants[v].tag);
    if (language <= 0) continue;
    result = doc_collector_add_variant(
        &args->documents, (uint32_t)language,
        display_rank(args->display_set, place->variants[v].name),
        display_rank(args->display_set, place->variants[v].city)
    );
    if (result != ARNM_SUCCESS && args->document_result == ARNM_SUCCESS) {
      args->document_result = result;
      return;
    }
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
      if (result != ARNM_SUCCESS && args->document_result == ARNM_SUCCESS) {
        args->document_result = result;
        return;
      }
    }
  }

  /* and the ground it stands on points at it too */
  char cell[GEO_CELL_TOKEN_SIZE];
  size_t cell_size = place_cell_token(cell, place);
  if (cell_size) {
    size_t rank = 0;
    if (name_set_rank(args->word_set, cell, cell_size, &rank)) {
      result = doc_collector_add_posting(&args->documents, (uint32_t)rank);
      if (result != ARNM_SUCCESS && args->document_result == ARNM_SUCCESS) {
        args->document_result = result;
      }
    } else {
      ++args->documents.dropped_words; /* the first pass wrote it — this cannot happen */
    }
  }
}

/** The parser's callback: hand the entry to whichever pass is running. */
static int process_place_callback(const PhotonPlace *place, void *user_data) {
  ParserThreadArgs *args = user_data;
  switch (args->pass) {
  case PARSER_PASS_VOCABULARY:
    json_stats_count_place(&args->stats, place);
    collect_vocabulary(args, place);
    /* The first pass is the only one that ever sees the whole dump, so it is
       the only one that can write the cache down.  What it writes is what the
       two passes behind it will read instead of unpacking the file again. */
    if (args->cache_write) {
      arnm_result result = place_cache_write(args->cache_write, place);
      if (result != ARNM_SUCCESS && args->cache_result == ARNM_SUCCESS) {
        args->cache_result = result;
      }
    }
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

/** Parse every line of one batch, counting the documents as they go by. */
static void process_batch(const ParseBatch *batch, ParserThreadArgs *args) {
  const char *line = batch->buffer->buffer;
  const char *end = line + batch->len;

  while (line < end) {
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    size_t len = newline ? (size_t)(newline - line) : (size_t)(end - line);
    if (len > 0 && line[len - 1] == '\r') { --len; }
    if (len > 0) {
      JsonParseResult result;
      json_parse_line(line, len, args->languages, process_place_callback, args, &result);
      if (args->pass == PARSER_PASS_VOCABULARY) json_stats_count_document(&args->stats, &result);
    }
    line = newline ? newline + 1 : end;
  }
}

/**
 * @brief Replay one thread's share of the cache instead of the dump.
 *
 *  The reader hands out a @ref PhotonPlace whose strings live in its own buffer
 *  and last until the next record — the same promise the JSON parser makes, so
 *  the callbacks below it cannot tell the two apart.
 *
 *  The first pass wants both halves, because the vocabulary is made of all of
 *  them; the later two want one each.
 */
static void replay_cache(ParserThreadArgs *args) {
  PlaceCacheKind kinds[2];
  size_t count = 0;
  switch (args->pass) {
  case PARSER_PASS_VOCABULARY:
    kinds[count++] = PLACE_CACHE_DOCUMENTS;
    kinds[count++] = PLACE_CACHE_HOUSES;
    break;
  case PARSER_PASS_DOCUMENTS:
    kinds[count++] = PLACE_CACHE_DOCUMENTS;
    break;
  case PARSER_PASS_HOUSES:
    kinds[count++] = PLACE_CACHE_HOUSES;
    break;
  }

  uint64_t behind = 0; /* the files before this one, so the progress only rises */
  for (size_t k = 0; k < count; ++k) {
    PlaceCacheReader reader;
    arnm_result result =
        place_cache_reader_open(&reader, args->cache_directory, args->thread, kinds[k]);
    if (result != ARNM_SUCCESS) {
      if (args->cache_result == ARNM_SUCCESS) args->cache_result = result;
      args->cache_finished = true;
      return;
    }
    PhotonPlace place;
    uint64_t seen = 0;
    while (place_cache_read(&reader, &place)) {
      process_place_callback(&place, args);
      /* the progress is read from another thread; telling it every few
         thousand records keeps the line moving without a lock per entry */
      if (++seen % 4096 == 0) { args->cache_bytes = behind + reader.bytes; }
    }
    behind += reader.bytes;
    args->cache_bytes = behind;
    /* a walk that stopped short of the end read half a cache, and half a cache
       builds an index nobody can tell from a whole one */
    if (reader.broken && args->cache_result == ARNM_SUCCESS) {
      args->cache_result = ARNM_ERROR_DECODE_FAILED;
    }
    place_cache_reader_close(&reader);
  }
  args->cache_finished = true;
}

/**
 * @brief One parser thread: take work until there is none, then settle what it gathered.
 *
 *  The sorting at the end belongs to the first pass alone, and it happens here
 *  rather than in the joining thread — every thread sorts its own run at the same
 *  moment, so the wall clock barely notices it happened.
 */
static void *parser_thread(void *arg) {
  ParserThreadArgs *args = arg;

  if (args->source == PASS_FROM_CACHE) {
    replay_cache(args);
  } else {
    ParseBatch batch;
    while (parse_queue_pop(args->queue, &batch)) {
      process_batch(&batch, args);
      buffer_pool_release(args->pool, batch.buffer);
    }
  }
  if (args->pass != PARSER_PASS_VOCABULARY) return NULL;

  /* the queue has run dry — sort what this thread gathered while the others
     do the same, so the join finds nothing but ordered runs */
  args->finish_result = name_collector_finish(&args->words, &args->word_run);
  if (args->finish_result == ARNM_SUCCESS) {
    args->finish_result = name_collector_finish(&args->display, &args->display_run);
  }
  return NULL;
}

/** Send whatever whole lines the buffer holds down the queue, and keep the rest. */
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
 * @brief What the threads are still doing after the last entry was handed over.
 *
 *  The stream ends before the work does: the batches already queued are still
 *  being parsed, and the first pass sorts everything it gathered before it lets
 *  go.  On a planet that is a minute in which the bar stands at the end and
 *  nothing else is said — which reads exactly like a program that has died.
 *  So the wait gets a name of its own.
 */
static const char *settle_label(ParserPass pass) {
  return pass == PARSER_PASS_VOCABULARY ? "Each thread sorts the words it gathered"
                                        : "The parser threads finish their last batches";
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
    const char *label,
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
    args[i].source = PASS_FROM_DUMP;
    if (pthread_create(&threads[i], NULL, parser_thread, &args[i]) != 0) {
      fatal(ERROR_MEMORY, "Failed to create parser thread %u.", i);
    }
  }

  progress_begin(label, totalBytes);
  stream_dump(fp, dstream, inputBuffer, inputSize, outputBuffer, outputSize, queue, pool);
  progress_end();

  progress_begin(settle_label(pass), 0);
  for (unsigned i = 0; i < thread_count; ++i) { pthread_join(threads[i], NULL); }
  progress_end();
  parse_queue_destroy(queue);
}

/**
 * @brief Run one pass out of the cache: no queue, no stream, one file per thread.
 *
 *  Every thread reads back what it wrote, so nothing has to be handed around
 *  and nothing has to be locked.  What the main thread does meanwhile is watch:
 *  the progress line is the sum of what the threads report, polled, because
 *  there is no single stream whose position could stand for the whole any more.
 *
 *  @param[in] totalBytes  Bytes the cache files of this pass hold together.
 */
static void run_cached_pass(
    ParserPass pass,
    const char *label,
    ParserThreadArgs *args,
    pthread_t *threads,
    unsigned thread_count,
    uint64_t totalBytes
) {
  for (unsigned i = 0; i < thread_count; ++i) {
    args[i].pass = pass;
    args[i].source = PASS_FROM_CACHE;
    args[i].cache_bytes = 0;
    /* the mark of the pass before this one would end this one's watch at once:
       the bar would fill for a tenth of a second and then stand still for a
       minute, while the reading it was meant to show went on behind it */
    args[i].cache_finished = false;
    if (pthread_create(&threads[i], NULL, parser_thread, &args[i]) != 0) {
      fatal(ERROR_MEMORY, "Failed to create parser thread %u.", i);
    }
  }

  progress_begin(label, totalBytes);
  for (bool running = true; running;) {
    /* short enough that a pass which is over in a moment is not reported as
       having taken the length of one look */
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    nanosleep(&pause, NULL);
    uint64_t done = 0;
    running = false;
    for (unsigned i = 0; i < thread_count; ++i) {
      done += args[i].cache_bytes;
      if (!args[i].cache_finished) running = true;
    }
    progress_update(done);
  }
  progress_end();

  /* the readers are through, but the first pass still has its sorting to do */
  progress_begin(settle_label(pass), 0);
  for (unsigned i = 0; i < thread_count; ++i) { pthread_join(threads[i], NULL); }
  progress_end();
}

/**
 * @brief How far a file being written has got.
 *
 *  The writer never comes back to say — it is inside one long call — so the
 *  file itself is asked, from outside, while it grows.  What the operating
 *  system reports lags behind by whatever still sits in the stream's buffer,
 *  which for a progress line is close enough.
 */
static uint64_t file_bytes(void *user_data) {
  struct stat status;
  if (stat((const char *)user_data, &status) != 0) return 0;
  return (uint64_t)status.st_size;
}

/**
 * @brief End the build on a collector that ran out of buckets, saying which one.
 *
 *  A per-thread vector holds @ref GEO_VEC_CEILING elements and no more — the bucket
 *  index inside arnm is a uint16, so no exponent buys another one.  What divides the
 *  work is the parser thread count, and the figure needed to choose it is exactly the
 *  two numbers below: what the vector held, against what it holds.
 *
 *  @param what      The pass that was running, as it reads in the message.
 *  @param thread    Which thread refused.
 *  @param threads   How many were running.
 *  @param limit     What house_collector_limit() or doc_collector_limit() answered.
 *  @param result    The code the collector came back with.
 */
_Noreturn static void fatal_collector(
    const char *what,
    unsigned thread,
    unsigned threads,
    const CollectorLimit *limit,
    arnm_result result
) {
  if (!limit->vector) {
    fatal(
        ERROR_MEMORY, "Parser thread %u failed to collect %s (arnm_result %d).", thread, what,
        (int)result
    );
  }
  /* One thread short of the ceiling means every thread is; the share each would carry
     with one more thread is what says how many more are needed. */
  const unsigned needed = (unsigned)((limit->held * threads + limit->ceiling - 1) / limit->ceiling);
  fatal(
      ERROR_MEMORY,
      "Parser thread %u ran out of room for %s: %s holds %zu entries per thread and\n"
      "this dump gave it more. That ceiling is fixed — a bucket vector addresses %zu\n"
      "elements and no exponent buys another one.\n"
      "\n"
      "What divides the work is the thread count. You gave %u; this dump needs at\n"
      "least %u. Pass a larger number as the third argument and build again — the\n"
      "place cache is written per thread count, so it is filled afresh.",
      thread, what, limit->vector, limit->ceiling, limit->ceiling, threads,
      needed > threads ? needed : threads + 1
  );
}

/** Parser threads a build may be given; the collectors are sized for this many. */
#define PARSER_THREADS_MAX 10

/**
 * @brief Refuse a thread count the second pass could not hold, before it runs.
 *
 *  Every word the first pass saw becomes a posting in the second, and the postings of
 *  one thread go into one bucket vector — @ref GEO_VEC_CEILING of them at the most.
 *
 *  The count is an upper bound on what the second pass produces: it tokenizes the same
 *  texts and only drops repetitions inside a single document, so it can go down from
 *  here and never up.  An eighth of the ceiling is left unclaimed on top of that,
 *  because the threads take their work batch by batch and the busiest ends up with more
 *  than the mean.  Being a thread too careful here costs a thread; being one too few
 *  costs the whole second pass.
 *
 *  @param postings Word occurrences the first pass counted.
 *  @param threads  Parser threads this build was given.
 */
static void refuse_too_few_threads(size_t postings, unsigned threads) {
  if (!threads) return;
  const size_t share = GEO_VEC_CEILING - GEO_VEC_CEILING / 8; /* room for the busiest thread */
  const size_t per_thread = (postings + threads - 1) / threads;
  if (per_thread <= share) return;

  const size_t needed = (postings + share - 1) / share;
  if (needed > PARSER_THREADS_MAX) {
    fatal(
        ERROR_MEMORY,
        "This dump is past what this build can hold.\n"
        "\n"
        "The first pass counted %zu word occurrences. They become postings in the\n"
        "second pass, where each thread keeps its own in one bucket vector of %zu\n"
        "entries — so even %d threads, the most this program takes, would need about\n"
        "%zu each.\n"
        "\n"
        "Fewer languages is the one lever from here: each one carries the whole address\n"
        "chain of every entry into the term stream again, and dropping the ones you do\n"
        "not search in takes the postings down with them.",
        postings, (size_t)GEO_VEC_CEILING, PARSER_THREADS_MAX,
        (postings + PARSER_THREADS_MAX - 1) / PARSER_THREADS_MAX
    );
  }
  fatal(
      ERROR_MEMORY,
      "This dump needs more parser threads than %u.\n"
      "\n"
      "The first pass counted %zu word occurrences, and every one of them becomes a\n"
      "posting in the second. The postings of one thread live in one bucket vector,\n"
      "which holds %zu and no more — %u threads would each be given about %zu.\n"
      "\n"
      "Build again with at least %zu as the third argument. The place cache is written\n"
      "per thread count, so it is filled afresh; everything else about the build is\n"
      "unchanged.",
      threads, postings, (size_t)GEO_VEC_CEILING, threads, per_thread, needed
  );
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
 *  @param[in] cache_directory     Where a place cache may be kept, or NULL for
 *                                 the plain three walks over the dump.
 *  @param[in] languages           Readings to keep; the first is the one an
 *                                 answer shows unless a caller asks otherwise.
 *  @return 0 on success; failures end the program through fatal().
 */
static int build_index(
    const char *dump_path,
    const char *index_path,
    unsigned parser_thread_count,
    const char *cache_directory,
    const PhotonLanguages *languages
) {
  arnm_mono_timer timeUsedAll;
  arnm_mono_timer_reset(&timeUsedAll);

  /* --- Is there a cache, may there be one, and does the one that is there
         still answer for this dump?  Three questions, and the answer to the
         last decides whether even the first pass may skip the dump. --- */
  PlaceCacheStamp stamp;
  bool cache_write = false, cache_read = false;
  if (cache_directory) {
    uint64_t free_bytes = 0;
    if (!place_cache_stamp_of(dump_path, parser_thread_count, languages, &stamp)) {
      fatal(ERROR_IO, "Cannot look at '%s'.", dump_path);
    }

    char freeBuffer[32], wantBuffer[32];
    format_byte_units(wantBuffer, sizeof(wantBuffer), place_cache_wanted(stamp.dump_bytes), 2);

    if (place_cache_is_current(cache_directory, &stamp)) {
      cache_read = true;
      printf(
          "Place cache in '%s' answers for this dump — the dump stays packed.\n", cache_directory
      );
    } else {
      /* A cache that does not answer for this dump is worth nothing and is in
         the way of its successor — on a disk sized for one cache, leaving it
         standing while measuring the room refuses the very partition that was
         made for the job.  So it goes first, and the room is counted after. */
      uint64_t stale_bytes = place_cache_size(cache_directory);
      if (stale_bytes) {
        char staleBuffer[32];
        format_byte_units(staleBuffer, sizeof(staleBuffer), stale_bytes, 2);
        printf(
            "Place cache in '%s' does not answer for this run — removing %s first.\n",
            cache_directory, staleBuffer
        );
        /* Said before the wait, not after it: unlinking that much takes
           seconds, and the line is the only sign that anything is happening. */
        fflush(stdout);
      }
      place_cache_discard(cache_directory, parser_thread_count);

      PlaceCacheRoom room = place_cache_make_room(cache_directory, stamp.dump_bytes, &free_bytes);
      format_byte_units(freeBuffer, sizeof(freeBuffer), free_bytes, 2);
      if (room == PLACE_CACHE_ROOM_OK) {
        cache_write = true;
        printf(
            "Place cache: writing to '%s' (%s free, %s asked for).\n"
            "  The first pass fills it; the two behind it read it instead of the dump.\n",
            cache_directory, freeBuffer, wantBuffer
        );
      } else if (room == PLACE_CACHE_ROOM_TOO_SMALL) {
        printf(
            "Place cache: '%s' holds %s and %s were asked for — walking the dump three "
            "times instead.\n",
            cache_directory, freeBuffer, wantBuffer
        );
      } else {
        /* Not a matter of size, and not something to pass over quietly: a
           mistyped path would otherwise look exactly like a full disk. */
        fatal(ERROR_USAGE, "Place cache '%s': %s.", cache_directory, place_cache_room_reason(room));
      }
    }
  }

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

  pthread_t parser_threads[PARSER_THREADS_MAX];
  ParserThreadArgs parser_args[10] = {0};
  BufferPool buffer_pool;

  char inputSizeBuf[32], outputSizeBuf[32];
  format_byte_units(inputSizeBuf, sizeof(inputSizeBuf), inputSize, 2);
  format_byte_units(outputSizeBuf, sizeof(outputSizeBuf), outputSize, 2);
  printf("inputSize: %s, outputSize: %s\n", inputSizeBuf, outputSizeBuf);

  buffer_pool_init(&buffer_pool, outputSize * 2);
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    /* One chain per thread — a multi arena bumps without a lock, so it must not be
       shared. The chains outlive the threads: they hold the text bytes. A NULL allocator
       leaves the chain's handle and its bookkeeping to malloc; the arenas themselves always
       come from the host. Every other option keeps arnm's default. */
    arnm_multi_arena_options name_arena_options = {.arena_capacity = NAME_ARENA_CAPACITY};
    parser_args[i].names = arnm_create_multi_arena(&name_arena_options, NULL);
    if (!parser_args[i].names) {
      fatal(ERROR_MEMORY, "Failed to create the name arena for parser thread %u.", i);
    }
    if (name_collector_init(&parser_args[i].words, parser_args[i].names) != ARNM_SUCCESS ||
        name_collector_init(&parser_args[i].display, parser_args[i].names) != ARNM_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init collectors for parser thread %u.", i);
    }
    text_tokenizer_init(&parser_args[i].tokenizer);
    parser_args[i].languages = languages;
    parser_args[i].thread = i;
    parser_args[i].cache_directory = cache_directory;
    parser_args[i].cache_result = ARNM_SUCCESS;
  }

  /* the writers outlive the first pass: they are closed and sealed after it */
  PlaceCacheWriter cache_writers[10] = {0};
  if (cache_write) {
    for (unsigned i = 0; i < parser_thread_count; ++i) {
      arnm_result result = place_cache_writer_open(&cache_writers[i], cache_directory, i);
      if (result != ARNM_SUCCESS) {
        fatal(
            ERROR_IO, "Cannot open the place cache for thread %u (arnm_result %d).", i, (int)result
        );
      }
      parser_args[i].cache_write = &cache_writers[i];
    }
  }

  fseek(fp, 0, SEEK_END);
  uint64_t totalBytes = ftell(fp);

  /* =======================================================================
   *  First pass: what words exist at all
   * ======================================================================= */

  /* What the later passes will read: the cache files as they now stand, or the
     dump again.  Measured once, so every pass can show a progress line. */
  uint64_t cache_bytes[2] = {0, 0};

  printf("\n");
  if (cache_read) {
    cache_bytes[0] = place_cache_bytes(cache_directory, parser_thread_count, PLACE_CACHE_DOCUMENTS);
    cache_bytes[1] = place_cache_bytes(cache_directory, parser_thread_count, PLACE_CACHE_HOUSES);
    run_cached_pass(
        PARSER_PASS_VOCABULARY, "Pass 1 of 3: gathering the vocabulary from the place cache",
        parser_args, parser_threads, parser_thread_count, cache_bytes[0] + cache_bytes[1]
    );
  } else {
    run_pass(
        PARSER_PASS_VOCABULARY, "Pass 1 of 3: gathering the vocabulary from the dump", fp, dstream,
        inputBuffer, inputSize, outputBuffer, outputSize, &buffer_pool, parser_args, parser_threads,
        parser_thread_count, totalBytes
    );
  }

  /* --- the cache is whole only once every file is closed, and only then may
         it be sealed: the manifest is what the next run reads it by --- */
  if (cache_write) {
    for (unsigned i = 0; i < parser_thread_count; ++i) {
      if (parser_args[i].cache_result != ARNM_SUCCESS) {
        fatal(
            ERROR_IO, "Writing the place cache of thread %u failed (arnm_result %d).", i,
            (int)parser_args[i].cache_result
        );
      }
      cache_bytes[0] += cache_writers[i].bytes[PLACE_CACHE_DOCUMENTS];
      cache_bytes[1] += cache_writers[i].bytes[PLACE_CACHE_HOUSES];
      place_cache_writer_close(&cache_writers[i]);
      parser_args[i].cache_write = NULL;
    }
    if (place_cache_seal(cache_directory, &stamp) != ARNM_SUCCESS) {
      fatal(ERROR_IO, "Cannot seal the place cache in '%s'.", cache_directory);
    }
    cache_read = true; /* what was just written is what the next passes read */

    char documentsBuffer[32], housesBuffer[32];
    format_byte_units(documentsBuffer, sizeof(documentsBuffer), cache_bytes[0], 2);
    format_byte_units(housesBuffer, sizeof(housesBuffer), cache_bytes[1], 2);
    printf(
        "\nPlace cache written: %s of documents, %s of house numbers.\n", documentsBuffer,
        housesBuffer
    );
  }

  JsonStats stats = {0};
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    json_stats_add(&stats, &parser_args[i].stats);
  }
  json_stats_print(&stats);

  /* --- the sorted per-thread runs flow together and lose their last doubles --- */
  const NameRun *word_runs[10];
  const NameRun *display_runs[10];
  uint64_t name_bytes = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].finish_result != ARNM_SUCCESS) {
      fatal(
          ERROR_MEMORY, "Parser thread %u failed to sort its texts (arnm_result %d).", i,
          (int)parser_args[i].finish_result
      );
    }
    word_runs[i] = &parser_args[i].word_run;
    display_runs[i] = &parser_args[i].display_run;
    arnm_multi_arena_stats arena_stats;
    if (arnm_multi_arena_measure(parser_args[i].names, &arena_stats) == ARNM_SUCCESS) {
      name_bytes += arena_stats.used;
    }
  }

  progress_begin("Joining the words and spellings of all threads", 0);
  NameSet words, display;
  arnm_result merge_result =
      name_run_merge(&words, word_runs, parser_thread_count, parser_thread_count);
  if (merge_result == ARNM_SUCCESS) {
    merge_result = name_run_merge(&display, display_runs, parser_thread_count, parser_thread_count);
  }
  if (merge_result != ARNM_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to merge the collected texts (arnm_result %d).", (int)merge_result);
  }
  progress_end();

  char nameBytesBuffer[32];
  format_byte_units(nameBytesBuffer, sizeof(nameBytesBuffer), name_bytes, 2);
  uint64_t inputs = 0, repeated = 0, dropped = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    inputs += parser_args[i].tokenizer.inputs;
    repeated += parser_args[i].tokenizer.repeated;
    dropped += parser_args[i].tokenizer.dropped;
  }
  printf("\nTexts: %" PRIu64 " offered, %" PRIu64 " skipped as repetitions\n", inputs, repeated);
  printf("Words: %zu seen, %zu distinct\n", words.total, words.count);
  printf("Spellings: %zu seen, %zu distinct\n", display.total, display.count);
  printf("  text memory: %s\n", nameBytesBuffer);
  if (dropped) { printf("  dropped (no room): %" PRIu64 "\n", dropped); }
  char treeBytesBuffer[32];
  format_byte_units(
      treeBytesBuffer, sizeof(treeBytesBuffer), prefix_tree_memory(&words.prefixes), 2
  );
  printf(
      "Prefix groups: %zu (index tree: depth %u, %zu levels, %s)\n", words.group_count,
      NAME_PREFIX_DEPTH, words.prefixes.levels, treeBytesBuffer
  );

  /* --- can the second pass hold what the first one just counted? ---
     Every word the first pass saw becomes a posting in the second, and the
     postings of one thread live in one bucket vector, which holds
     GEO_VEC_CEILING of them and not one more.  The figure is known here and the
     second pass costs minutes, so this is where it is worth asking rather than
     finding out from a thread that has already run half of them. */
  refuse_too_few_threads(words.total, parser_thread_count);

  /* the runs have handed their words to the merged sets */
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    name_run_free(&parser_args[i].word_run);
    name_run_free(&parser_args[i].display_run);
  }

  /* =======================================================================
   *  Second pass: places, and which words point at them
   * ======================================================================= */

  printf("\n");
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    parser_args[i].word_set = &words;
    parser_args[i].display_set = &display;
    parser_args[i].document_result = ARNM_SUCCESS;
    if (doc_collector_init(&parser_args[i].documents) != ARNM_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init document collector for parser thread %u.", i);
    }
    /* every occurrence counts now — a posting belongs to its document even
       when the same text came by a moment ago */
    parser_args[i].tokenizer.repetition_filter = 0;
  }

  if (cache_read) {
    run_cached_pass(
        PARSER_PASS_DOCUMENTS, "Pass 2 of 3: documents and posting lists, from the place cache",
        parser_args, parser_threads, parser_thread_count, cache_bytes[0]
    );
  } else {
    run_pass(
        PARSER_PASS_DOCUMENTS, "Pass 2 of 3: documents and posting lists", fp, dstream, inputBuffer,
        inputSize, outputBuffer, outputSize, &buffer_pool, parser_args, parser_threads,
        parser_thread_count, totalBytes
    );
  }

  DocCollector *doc_collectors[PARSER_THREADS_MAX];
  uint64_t unknown_words = 0;
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].document_result != ARNM_SUCCESS) {
      CollectorLimit limit = {NULL, 0, 0};
      doc_collector_limit(&parser_args[i].documents, &limit);
      fatal_collector("documents", i, parser_thread_count, &limit, parser_args[i].document_result);
    }
    doc_collectors[i] = &parser_args[i].documents;
    unknown_words += parser_args[i].documents.dropped_words;
  }

  progress_begin("Joining the documents of all threads", 0);
  DocSet documents;
  arnm_result doc_result = doc_collector_merge(
      &documents, doc_collectors, parser_thread_count, words.count, languages->count
  );
  if (doc_result != ARNM_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to join the documents (arnm_result %d).", (int)doc_result);
  }
  progress_end();
  printf(
      "Documents: %zu from %zu segments, postings: %zu\n", documents.document_count,
      documents.segment_count, documents.posting_count
  );
  if (unknown_words) {
    printf("  words without a rank: %" PRIu64 " (should be 0)\n", unknown_words);
  }

  /* =======================================================================
   *  Third pass: the house numbers, onto the streets that now exist
   * ======================================================================= */

  printf("\n");
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    parser_args[i].doc_set = &documents;
    parser_args[i].house_result = ARNM_SUCCESS;
    if (house_collector_init(&parser_args[i].houses) != ARNM_SUCCESS) {
      fatal(ERROR_MEMORY, "Failed to init house collector for parser thread %u.", i);
    }
  }

  if (cache_read) {
    run_cached_pass(
        PARSER_PASS_HOUSES, "Pass 3 of 3: house numbers, from the place cache", parser_args,
        parser_threads, parser_thread_count, cache_bytes[1]
    );
  } else {
    run_pass(
        PARSER_PASS_HOUSES, "Pass 3 of 3: house numbers", fp, dstream, inputBuffer, inputSize,
        outputBuffer, outputSize, &buffer_pool, parser_args, parser_threads, parser_thread_count,
        totalBytes
    );
  }

  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].cache_result != ARNM_SUCCESS) {
      fatal(
          ERROR_IO, "Reading the place cache of thread %u failed (arnm_result %d).", i,
          (int)parser_args[i].cache_result
      );
    }
  }

  HouseCollector *house_collectors[PARSER_THREADS_MAX];
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    if (parser_args[i].house_result != ARNM_SUCCESS) {
      CollectorLimit limit = {NULL, 0, 0};
      house_collector_limit(&parser_args[i].houses, &limit);
      fatal_collector("houses", i, parser_thread_count, &limit, parser_args[i].house_result);
    }
    house_collectors[i] = &parser_args[i].houses;
  }

  progress_begin("Ordering the house numbers onto their streets", 0);
  HouseSet houses;
  arnm_result house_result = house_collector_merge(
      &houses, house_collectors, parser_thread_count, documents.document_count
  );
  if (house_result != ARNM_SUCCESS) {
    fatal(ERROR_MEMORY, "Failed to join the houses (arnm_result %d).", (int)house_result);
  }
  progress_end();
  printf("House numbers: %zu on %zu streets\n", houses.house_count, documents.street_count);
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

  /* --- the result lies down in the shape it will be read in --- */
  /* The tags travel to the writer as plain text, so that the file format owes
     nothing to the parser's idea of a language list. */
  char language_tags[PHOTON_LANGUAGE_MAX][GEO_LANGUAGE_TAG_MAX];
  memset(language_tags, 0, sizeof(language_tags));
  for (uint8_t l = 0; l < languages->count; ++l) {
    memcpy(language_tags[l], languages->tag[l], GEO_LANGUAGE_TAG_MAX);
    language_tags[l][GEO_LANGUAGE_TAG_MAX - 1] = '\0';
  }

  char writeLabel[256];
  snprintf(writeLabel, sizeof(writeLabel), "Writing the index to '%s'", index_path);
  progress_begin_polled(writeLabel, 0, file_bytes, (void *)index_path);
  arnm_result write_result = geo_index_write(
      index_path, &words, &display, &documents, &houses, language_tags, words.total
  );
  if (write_result != ARNM_SUCCESS) {
    fatal(ERROR_IO, "Failed to write index '%s' (arnm_result %d).", index_path, (int)write_result);
  }
  progress_end();

  progress_begin("Giving the memory back", 0);
  house_set_free(&houses);
  doc_set_free(&documents);
  name_set_free(&words);
  name_set_free(&display);
  for (unsigned i = 0; i < parser_thread_count; ++i) {
    house_collector_free(&parser_args[i].houses);
    doc_collector_free(&parser_args[i].documents);
    name_collector_free(&parser_args[i].words);
    name_collector_free(&parser_args[i].display);
    arnm_destroy(parser_args[i].names, NULL);
  }
  buffer_pool_destroy(&buffer_pool);
  free(outputBuffer);
  free(inputBuffer);
  ZSTD_freeDStream(dstream);
  fclose(fp);
  progress_end();

  if (cache_read) {
    char cacheBuffer[32];
    format_byte_units(cacheBuffer, sizeof(cacheBuffer), cache_bytes[0] + cache_bytes[1], 2);
    printf(
        "Place cache kept in '%s' (%s) — the next build of this dump starts from it.\n",
        cache_directory, cacheBuffer
    );
  }

  /* the only total there is: every step above told what it alone had cost */
  char totalBuffer[32];
  arnm_duration_string(
      totalBuffer, sizeof(totalBuffer), (arnm_duration)arnm_mono_timer_nanos(timeUsedAll), 2
  );
  printf("\nTotal time, everything together: %s\n", totalBuffer);
  return 0;
}

/* =========================================================================
 *  The short way: a finished index
 * ========================================================================= */

/** Print one borrowed text, or a placeholder when the entry never carried it. */
static void print_text(const char *text, size_t size) {
  if (!text || !size) {
    printf("—");
    return;
  }
  printf("%.*s", (int)size, text);
}

/** Show what a query found: the place as it is written, and where it lies. */
/** One line per result: the address as written, then where it stands. */
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
  size_t length = arnm_uint64_to_string(digits, (uint8_t)sizeof(digits), value);
  size_t written = 0;
  for (size_t i = 0; i < length && written + 2 < size; ++i) {
    if (i && (length - i) % 3 == 0) buffer[written++] = ' ';
    buffer[written++] = digits[i];
  }
  buffer[written] = '\0';
}

/** One line of the query report: a label, and its count in a column of its own. */
/** A labelled count, digits grouped, aligned with the others around it. */
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
static void print_query_stats(const GeoQueryStats *stats, const char *duration, bool near) {
  printf("\nSearched in %s\n", duration);
  print_count("passes:", stats->passes);
  print_count("words that narrowed:", stats->groups);
  if (near) {
    print_count("cells around you:", stats->near_cells);
    print_count("places in them:", stats->near_documents);
    if (stats->position_dropped) { printf("  %-22s %13s\n", "position:", "dropped"); }
  }
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
 *  Mapping is constant time, so the counts appear before the disk has been
 *  touched beyond the header.
 *
 *  @param[in] index_path    File written by a previous build.
 *  @param[in] query         Free text to search for, or NULL for counts only.
 *  @param[in] result_limit  Most results to show.
 *  @param[in] prefix        Read the last word as a beginning as well.
 *  @param[in] near          Where the searcher stands, or NULL.
 *  @return 0 on success; a failed open ends the program through fatal().
 */
static int open_index(
    const char *index_path,
    const char *query,
    size_t result_limit,
    bool prefix,
    const GeoSearchOptions *near
) {
  arnm_mono_timer timeUsed;
  arnm_mono_timer_init();
  arnm_mono_timer_reset(&timeUsed);

  GeoClient *client = NULL;
  GeoStatus status = geo_client_open(&client, index_path);
  if (status != GEO_OK) {
    fatal(ERROR_IO, "Cannot open index '%s' (GeoStatus %d).", index_path, (int)status);
  }

  GeoClientInfo info;
  geo_client_info(client, &info);

  char timeUsedBuffer[32], sizeBuffer[32];
  arnm_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), timeUsed);
  format_byte_units(sizeBuffer, sizeof(sizeBuffer), info.file_size, 2);

  printf("Index '%s' opened in %s\n", index_path, timeUsedBuffer);
  printf("  file:           %s (format %u)\n", sizeBuffer, info.format);
  printf("  words:          %" PRIu64 "\n", info.words);
  printf("  spellings:      %" PRIu64 "\n", info.spellings);
  printf("  documents:      %" PRIu64 "\n", info.documents);
  printf("  house numbers:  %" PRIu64 "\n", info.houses);
  printf("  postings:       %" PRIu64 "\n", info.postings);
  if (info.languages) {
    printf("  readings:       ");
    for (uint32_t l = 0; l < info.languages; ++l) {
      char tag[GEO_CLIENT_LANGUAGE_MAX] = {0};
      if (geo_client_language(client, l, tag, sizeof(tag)) != GEO_OK) continue;
      printf("%s%s", l ? ", " : "", tag);
    }
    printf("  (the first is shown unless --language asks otherwise)\n");
  }

  if (query) {
    GeoAddress found[64];
    if (result_limit > sizeof(found) / sizeof(found[0])) {
      result_limit = sizeof(found) / sizeof(found[0]);
    }

    /* the counts are gathered while the query runs; asking for them is what
       makes this a debugging tool rather than only a search */
    GeoQueryStats stats;
    GeoSearchOptions options = near ? *near : (GeoSearchOptions){0};
    options.prefix_last = prefix;
    options.stats = &stats;

    arnm_mono_timer queryTime;
    arnm_mono_timer_reset(&queryTime);
    size_t count =
        geo_client_search_options(client, query, strlen(query), &options, found, result_limit);
    arnm_mono_timer_string(timeUsedBuffer, sizeof(timeUsedBuffer), queryTime);

    printf("\n");
    if (options.has_position) { printf("From %.5f, %.5f:\n", options.latitude, options.longitude); }
    print_results(found, count, query);
    print_query_stats(&stats, timeUsedBuffer, options.has_position);
  }

  geo_client_close(client);
  return 0;
}

/* =========================================================================
 *  Command line
 * ========================================================================= */

/**
 * @brief Does @p path end in @p extension, whatever the case?
 *
 *  The comparison goes through strcasecmp(), so `planet.gdx`, `planet.GDX` and
 *  `planet.Gdx` are one and the same name here.  This is what decides which of
 *  the two programs in this file runs, and a caller who typed the suffix in
 *  capitals meant the index either way.
 */
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

/** Both invocations, their arguments and their defaults, in one screen. */
static void print_usage(const char *program) {
  fatal(
      ERROR_USAGE,
      "Usage:\n"
      "  %s <photon_dump.jsonl.zst> [index%s] [parser_threads: 1-10] [--cache=<dir>]\n"
      "      [--languages=de,en,fr]\n"
      "      Builds the search index from the dump and writes it as a binary file.\n"
      "      Without a destination the name comes from the dump\n"
      "      (planet.jsonl.zst -> planet%s). parser_threads defaults to 4.\n"
      "      --cache=<dir> keeps the entries the build needs as a binary file there,\n"
      "      once, and reads them instead of unpacking the dump twice more. It needs\n"
      "      twice the dump's size free and is passed over when it is not; what it\n"
      "      leaves behind is read again by the next build of the same dump.\n"
      "      --languages names the readings the index keeps. The first is the one an\n"
      "      answer shows unless a search asks otherwise; every further one costs\n"
      "      index size and build memory, so name only what you will ask for.\n"
      "      Without it the index keeps German alone, as it always did.\n"
      "\n"
      "  %s <index%s> [\"query\"] [max_results] [lat,lon] [--language=en]\n"
      "      Maps a finished index, shows its counts and — when a query is given —\n"
      "      the places that carry all of its words, in any order.\n"
      "      The last word counts as still being typed and is read as a beginning\n"
      "      as well: \"Marienpl\" finds \"Marienplatz\". A trailing space or comma\n"
      "      closes it and searches it exactly as it stands.\n"
      "      max_results defaults to %d. Given lat,lon the search narrows to the\n"
      "      places around that point first and orders what is left by distance,\n"
      "      after everything the query named outright.\n"
      "      --language picks which reading the answer shows, out of the ones the\n"
      "      index was built with; an index that has none is answered as it stands.\n"
      "\n"
      "The path decides which way it goes: a file ending in %s is loaded,\n"
      "anything else is built.\n"
      "\n"
      "Examples:\n"
      "  %s planet.jsonl.zst 8\n"
      "  %s planet%s \"Berlin, Superstrasse\"\n"
      "  %s planet%s \"15328 Bleyen\" 5\n"
      "  %s planet%s \"Berlin Marienpl\"     (still typing)\n"
      "  %s planet%s \"Berlin Marienplatz \" (finished)\n"
      "  %s planet%s \"Hauptstrasse \" 5 50.9414,6.9583   (near Cologne cathedral)",
      program, GEO_INDEX_EXTENSION, GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION,
      DEFAULT_RESULT_LIMIT, GEO_INDEX_EXTENSION, program, program, GEO_INDEX_EXTENSION, program,
      GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION, program, GEO_INDEX_EXTENSION, program,
      GEO_INDEX_EXTENSION
  );
}

/** Read a positive count, or end the program saying what was wrong. */
/** Read a bounded count from the command line; anything outside the range is fatal. */
static unsigned parse_count(const char *text, unsigned low, unsigned high, const char *name) {
  char *end;
  unsigned long value = strtoul(text, &end, 10);
  if (*text == '\0' || *end != '\0' || value < low || value > high) {
    fatal(ERROR_USAGE, "%s must be between %u and %u.", name, low, high);
  }
  return (unsigned)value;
}

/**
 * @brief Read "50.9414,6.9583" as a position, or end the program saying why not.
 *
 *  Two numbers and a comma, in the order every map writes them.  Anything else
 *  is a mistyped argument rather than a place on earth, and a search silently
 *  run from nowhere would be the worse answer.
 */
static GeoSearchOptions parse_position(const char *text) {
  char *end = NULL;
  double latitude = strtod(text, &end);
  if (end == text || *end != ',') { fatal(ERROR_USAGE, "Position must read as lat,lon."); }

  const char *second = end + 1;
  double longitude = strtod(second, &end);
  if (end == second || *end != '\0') { fatal(ERROR_USAGE, "Position must read as lat,lon."); }
  if (!(latitude >= -90.0 && latitude <= 90.0)) {
    fatal(ERROR_USAGE, "Latitude must lie between -90 and 90.");
  }
  if (!(longitude >= -180.0 && longitude <= 180.0)) {
    fatal(ERROR_USAGE, "Longitude must lie between -180 and 180.");
  }
  return (GeoSearchOptions){.has_position = true, .latitude = latitude, .longitude = longitude};
}

/**
 * @brief Take `--cache=<dir>` out of the argument list.
 *
 *  The rest of the command line is positional and reads its arguments by
 *  count, so a named one has to be gone before the counting starts — it is
 *  lifted out and the remaining arguments close the gap.  Both spellings are
 *  accepted, `--cache=<dir>` and `--cache <dir>`.
 *
 *  @param[in,out] argc  Argument count, lowered by what was taken.
 *  @param[in,out] argv  Argument vector, closed up.
 *  @return The directory, or NULL when the option was not given.
 */
static const char *take_option(int *argc, char *argv[], const char *option, const char *wants) {
  size_t option_size = strlen(option);
  const char *value = NULL;

  for (int i = 1; i < *argc;) {
    const char *argument = argv[i];
    size_t taken = 0;
    if (strncmp(argument, option, option_size) == 0) {
      /* `--language` must not swallow `--languages`: only a name that ends
         here, or ends at its own `=`, is this option. */
      const char *rest = argument + option_size;
      if (*rest == '=') {
        value = rest + 1;
        taken = 1;
      } else if (*rest == '\0') {
        if (i + 1 >= *argc) { fatal(ERROR_USAGE, "%s wants %s.", option, wants); }
        value = argv[i + 1];
        taken = 2;
      }
    }
    if (!taken) {
      ++i;
      continue;
    }
    for (int m = i; m + (int)taken < *argc; ++m) { argv[m] = argv[m + taken]; }
    *argc -= (int)taken;
  }
  if (value && !*value) { fatal(ERROR_USAGE, "%s wants %s.", option, wants); }
  return value;
}

int main(int argc, char *argv[]) {
  const char *cache_directory = take_option(&argc, argv, "--cache", "a directory");
  const char *language_list = take_option(&argc, argv, "--languages", "a list of language tags");
  const char *language = take_option(&argc, argv, "--language", "a language tag");
  if (argc < 2 || argc > 5) { print_usage(argv[0]); }

  const bool searching = has_extension(argv[1], GEO_INDEX_EXTENSION);
  if (cache_directory && searching) {
    fatal(ERROR_USAGE, "--cache belongs to a build, not to a search.");
  }
  if (language_list && searching) {
    fatal(ERROR_USAGE, "--languages belongs to a build; a search asks with --language.");
  }
  if (language && !searching) {
    fatal(
        ERROR_USAGE, "--language belongs to a search; a build names its readings with --languages."
    );
  }

  /* Without a word on the matter a build keeps the German reading, as every
     build did before there was a choice — an index made today answers exactly
     as the one made yesterday does. */
  PhotonLanguages languages;
  if (!photon_languages_parse(language_list ? language_list : "de", &languages)) {
    fatal(
        ERROR_USAGE, "--languages: a tag may hold %d bytes and there may be %d of them.",
        PHOTON_LANGUAGE_TAG_MAX - 1, PHOTON_LANGUAGE_MAX
    );
  }
  if (languages.every) {
    fatal(
        ERROR_USAGE,
        "--languages=all is not supported: the tags have to be known before the second pass,\n"
        "and a planet dump offers more of them than an index may hold. Name the ones you want,\n"
        "for instance --languages=de,en,fr."
    );
  }
  if (!languages.count) { fatal(ERROR_USAGE, "--languages names no language."); }

  const char *input = argv[1];
  if (searching) {
    const char *query = argc >= 3 ? argv[2] : NULL;
    unsigned limit = argc >= 4 ? parse_count(argv[3], 1, 64, "max_treffer") : DEFAULT_RESULT_LIMIT;

    GeoSearchOptions near = {0};
    if (argc == 5) near = parse_position(argv[4]);
    near.language = language;

    /* A word is still being typed unless something closed it: a trailing space
       or comma says the writer is done with it, and then it is read as it
       stands rather than as a beginning. */
    bool prefix = true;
    if (query && *query) {
      char last = query[strlen(query) - 1];
      prefix = last != ' ' && last != ',' && last != ';';
    }
    return open_index(input, query, limit, prefix, &near);
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
    parser_thread_count = parse_count(argv[2], 1, PARSER_THREADS_MAX, "parser_threads");
  } else if (argc == 4) {
    parser_thread_count = parse_count(argv[3], 1, PARSER_THREADS_MAX, "parser_threads");
  }

  arnm_mono_timer_init();
  return build_index(input, index_path, parser_thread_count, cache_directory, &languages);
}

/** @endcond */
