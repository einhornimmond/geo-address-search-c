/** @cond INTERNAL */

#include "client.h"

#include "geo_index.h"
#include "json_parse.h" /* only to nail the kind numbers to the builder's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kinds a caller sees are the ones the builder wrote; if the builder ever
   renumbers them, this build refuses to compile rather than to answer wrongly. */
static_assert((int)GEO_PLACE_STREET == (int)PHOTON_PLACE_TYPE_STREET, "kind numbers drifted");
static_assert((int)GEO_PLACE_CITY == (int)PHOTON_PLACE_TYPE_CITY, "kind numbers drifted");
static_assert((int)GEO_PLACE_HOUSE == (int)PHOTON_PLACE_TYPE_HOUSE, "kind numbers drifted");
static_assert((int)GEO_PLACE_DISTRICT == (int)PHOTON_PLACE_TYPE_DISTRICT, "kind numbers drifted");
static_assert(
    (int)GEO_PLACE_INDEPENDENT_CITY == (int)PHOTON_PLACE_TYPE_INDEPENDENT_CITY,
    "kind numbers drifted"
);

/** Most results one call may ask for; beyond that a search is a listing. */
#define GEO_CLIENT_LIMIT_MAX 256

struct GeoClient {
  GeoIndex index;
};

/** Carry an internal result code out to the caller's vocabulary. */
static GeoStatus status_of(grd_result result) {
  switch (result) {
  case GRD_SUCCESS:
    return GEO_OK;
  case GRD_ERROR_NULL_POINTER:
    return GEO_ERROR_ARGUMENT;
  case GRD_ERROR_DECODE_FAILED:
    return GEO_ERROR_FILE;
  case GRD_ERROR_OUT_OF_MEMORY:
    return GEO_ERROR_MEMORY;
  default:
    return GEO_ERROR_FORMAT;
  }
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

GeoStatus geo_client_open(GeoClient **out, const char *path) {
  if (!out || !path) return GEO_ERROR_ARGUMENT;
  *out = NULL;

  GeoClient *client = calloc(1, sizeof(*client));
  if (!client) return GEO_ERROR_MEMORY;

  grd_result result = geo_index_open(&client->index, path);
  if (result != GRD_SUCCESS) {
    free(client);
    return status_of(result);
  }
  *out = client;
  return GEO_OK;
}

void geo_client_close(GeoClient *client) {
  if (!client) return;
  geo_index_close(&client->index);
  free(client);
}

GeoStatus geo_client_info(const GeoClient *client, GeoClientInfo *out) {
  if (!client || !out) return GEO_ERROR_ARGUMENT;
  out->file_size = client->index.size;
  out->documents = client->index.document_count;
  out->houses = client->index.house_count;
  out->words = client->index.words.word_count;
  out->spellings = client->index.display.word_count;
  out->postings = client->index.posting_count;
  out->format = GEO_INDEX_VERSION;
  return GEO_OK;
}

/* =========================================================================
 *  Searching
 * ========================================================================= */

/** Point a result field at a display text, or at nothing when there is none. */
static void borrow_text(
    const GeoIndex *index, uint32_t rank, const char **out_text, size_t *out_size
) {
  *out_text = NULL;
  *out_size = 0;
  if (rank == GEO_RANK_NONE) return;
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, rank, &size);
  if (!text) return;
  *out_text = text;
  *out_size = size;
}

/** Turn one hit into what a caller reads. */
static void fill_address(const GeoIndex *index, const GeoHit *hit, GeoAddress *address) {
  const GeoDocument *document = &index->documents[hit->document];
  memset(address, 0, sizeof(*address));

  borrow_text(index, document->name_rank, &address->name, &address->name_size);
  borrow_text(index, document->postcode_rank, &address->postcode, &address->postcode_size);
  borrow_text(index, document->city_rank, &address->city, &address->city_size);

  int32_t lat = document->lat_e7;
  int32_t lon = document->lon_e7;
  if (hit->house != GEO_RANK_NONE) { /* the number was found on this place */
    const GeoHouse *house = &index->houses[hit->house];
    borrow_text(index, house->number_rank, &address->number, &address->number_size);
    lat = house->lat_e7;
    lon = house->lon_e7;
  }

  address->latitude = lat / 1.0e7;
  address->longitude = lon / 1.0e7;
  address->document = hit->document;
  address->matched = hit->matched;
  address->importance = hit->importance;
  address->kind = document->type;
  address->has_point = (document->flags & GEO_DOCUMENT_HAS_POINT) ? 1u : 0u;
}

size_t geo_client_search(
    const GeoClient *client,
    const char *query,
    size_t query_size,
    bool prefix_last,
    GeoAddress *out,
    size_t limit
) {
  if (!client || !query || !query_size || !out || !limit) return 0;
  if (limit > GEO_CLIENT_LIMIT_MAX) limit = GEO_CLIENT_LIMIT_MAX;

  /* The folding scratch lives here, on this call's own stack — that is what
     makes two threads able to search the same client at the same time. */
  TextTokenizer tokenizer;
  GeoHit hits[GEO_CLIENT_LIMIT_MAX];

  size_t count =
      geo_index_query(&client->index, &tokenizer, query, query_size, prefix_last, hits, limit);
  for (size_t i = 0; i < count; ++i) { fill_address(&client->index, &hits[i], &out[i]); }
  return count;
}

/* =========================================================================
 *  The same answer as text, for callers across a language border
 * ========================================================================= */

/** A writer that keeps counting after the buffer is full, so the caller learns the size. */
typedef struct JsonWriter {
  char *buffer;
  size_t capacity;
  size_t needed; /**< Bytes the whole answer wants, NUL excluded. */
} JsonWriter;

static void json_put(JsonWriter *writer, const char *text, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (writer->needed + 1 < writer->capacity) { writer->buffer[writer->needed] = text[i]; }
    ++writer->needed;
  }
}

static void json_literal(JsonWriter *writer, const char *text) {
  json_put(writer, text, strlen(text));
}

/** Write a string as JSON, escaping what the format forbids. */
static void json_string(JsonWriter *writer, const char *text, size_t size) {
  if (!text) {
    json_literal(writer, "null");
    return;
  }
  json_literal(writer, "\"");
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
    case '"':
      json_literal(writer, "\\\"");
      break;
    case '\\':
      json_literal(writer, "\\\\");
      break;
    case '\n':
      json_literal(writer, "\\n");
      break;
    case '\r':
      json_literal(writer, "\\r");
      break;
    case '\t':
      json_literal(writer, "\\t");
      break;
    default:
      if (c < 0x20) { /* control characters have to be spelled out */
        char escape[7];
        snprintf(escape, sizeof(escape), "\\u%04x", c);
        json_literal(writer, escape);
      } else {
        json_put(writer, (const char *)&c, 1);
      }
      break;
    }
  }
  json_literal(writer, "\"");
}

static void json_number(JsonWriter *writer, const char *format, double value) {
  char text[32];
  int written = snprintf(text, sizeof(text), format, value);
  if (written > 0) json_put(writer, text, (size_t)written);
}

size_t geo_client_search_json(
    const GeoClient *client,
    const char *query,
    size_t query_size,
    bool prefix_last,
    size_t limit,
    char *buffer,
    size_t buffer_size
) {
  if (limit > GEO_CLIENT_LIMIT_MAX) limit = GEO_CLIENT_LIMIT_MAX;
  GeoAddress addresses[GEO_CLIENT_LIMIT_MAX];
  size_t count = geo_client_search(client, query, query_size, prefix_last, addresses, limit);

  JsonWriter writer = {.buffer = buffer, .capacity = buffer ? buffer_size : 0, .needed = 0};
  json_literal(&writer, "[");
  for (size_t i = 0; i < count; ++i) {
    const GeoAddress *address = &addresses[i];
    if (i) json_literal(&writer, ",");
    json_literal(&writer, "{\"name\":");
    json_string(&writer, address->name, address->name_size);
    json_literal(&writer, ",\"number\":");
    json_string(&writer, address->number, address->number_size);
    json_literal(&writer, ",\"postcode\":");
    json_string(&writer, address->postcode, address->postcode_size);
    json_literal(&writer, ",\"city\":");
    json_string(&writer, address->city, address->city_size);
    if (address->has_point) {
      json_literal(&writer, ",\"lat\":");
      json_number(&writer, "%.7f", address->latitude);
      json_literal(&writer, ",\"lon\":");
      json_number(&writer, "%.7f", address->longitude);
    } else {
      json_literal(&writer, ",\"lat\":null,\"lon\":null");
    }
    json_literal(&writer, ",\"kind\":");
    json_number(&writer, "%.0f", (double)address->kind);
    json_literal(&writer, ",\"importance\":");
    json_number(&writer, "%.0f", (double)address->importance);
    json_literal(&writer, ",\"matched\":");
    json_number(&writer, "%.0f", (double)address->matched);
    json_literal(&writer, "}");
  }
  json_literal(&writer, "]");

  if (buffer && buffer_size) {
    size_t end = writer.needed < buffer_size ? writer.needed : buffer_size - 1;
    buffer[end] = '\0';
  }
  return writer.needed;
}

/** @endcond */
