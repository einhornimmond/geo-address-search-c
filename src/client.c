/** @cond INTERNAL */

#include "client.h"

#include "geo_index.h"
#include "json_parse.h" /* only to nail the kind numbers to the builder's */

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
  size_t cur = writer->needed;

  writer->needed += size;
  if (writer->needed >= writer->capacity) {
    if (cur < writer->capacity) {
      memcpy(&writer->buffer[cur], text, writer->capacity - cur);
    }
    return;
  }
  memcpy(&writer->buffer[cur], text, size);
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
        static const char HEX[17] = "0123456789abcdef";
        char escape[6] = {'\\', 'u', '0', '0', '0', '0'};
        escape[4] = HEX[c >> 4];
        escape[5] = HEX[c & 0x0f];
        json_put(writer, escape, sizeof(escape));
      } else {
        json_put(writer, (const char *)&c, 1);
      }
      break;
    }
  }
  json_literal(writer, "\"");
}

/** How many decimal digits a value spells out; a decision tree, no loop, no division. */
static size_t uint64_digits(uint64_t v) {
  if (v < 100000000ULL) {
    if (v < 10000ULL) {
      if (v < 100ULL) { return v < 10ULL ? 1 : 2; }
      return v < 1000ULL ? 3 : 4;
    }
    if (v < 1000000ULL) { return v < 100000ULL ? 5 : 6; }
    return v < 10000000ULL ? 7 : 8;
  }
  if (v < 1000000000000ULL) {
    if (v < 10000000000ULL) { return v < 1000000000ULL ? 9 : 10; }
    return v < 100000000000ULL ? 11 : 12;
  }
  if (v < 10000000000000000ULL) {
    if (v < 100000000000000ULL) { return v < 10000000000000ULL ? 13 : 14; }
    return v < 1000000000000000ULL ? 15 : 16;
  }
  if (v < 1000000000000000000ULL) { return v < 100000000000000000ULL ? 17 : 18; }
  return v < 10000000000000000000ULL ? 19 : 20; /* UINT64_MAX itself spells out 20 */
}

/** Two digits per step, filled back to front; `digits` has to be uint64_digits(value). */
static void uint64_digits_write(char *text, uint64_t value, size_t digits) {
  static const char PAIRS[201] = "00010203040506070809"
                                 "10111213141516171819"
                                 "20212223242526272829"
                                 "30313233343536373839"
                                 "40414243444546474849"
                                 "50515253545556575859"
                                 "60616263646566676869"
                                 "70717273747576777879"
                                 "80818283848586878889"
                                 "90919293949596979899";
  size_t cursor = digits;

  while (value >= 100) {
    uint64_t rest = value / 100;
    uint64_t pair = value - rest * 100;
    text[--cursor] = PAIRS[pair * 2 + 1];
    text[--cursor] = PAIRS[pair * 2];
    value = rest;
  }
  if (value < 10) {
    text[--cursor] = (char)('0' + value);
  } else {
    text[--cursor] = PAIRS[value * 2 + 1];
    text[--cursor] = PAIRS[value * 2];
  }
}

static void json_uint(JsonWriter *writer, uint64_t value) {
  char text[20];
  size_t digits = uint64_digits(value);

  uint64_digits_write(text, value, digits);
  json_put(writer, text, digits);
}

/** Degrees with seven decimals, the way `%.7f` used to spell them. */
static void json_degrees(JsonWriter *writer, double value) {
  /* Anything not a real number in range — NaN, infinity — has no JSON spelling. */
  if (!(value > -1000000.0 && value < 1000000.0)) {
    json_literal(writer, "null");
    return;
  }

  bool negative = value < 0.0;
  if (negative) value = -value;

  uint64_t scaled = (uint64_t)(value * 10000000.0 + 0.5);
  uint64_t whole = scaled / 10000000ULL;
  uint64_t fraction = scaled - whole * 10000000ULL;

  char text[32];
  size_t cursor = 0;
  if (negative && scaled) text[cursor++] = '-';

  size_t digits = uint64_digits(whole);
  uint64_digits_write(&text[cursor], whole, digits);
  cursor += digits;

  text[cursor++] = '.';
  for (size_t i = 0; i < 7; ++i) { text[cursor + i] = '0'; } /* keep the leading zeros */
  uint64_digits_write(&text[cursor], fraction, 7);
  cursor += 7;

  json_put(writer, text, cursor);
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
      json_degrees(&writer, address->latitude);
      json_literal(&writer, ",\"lon\":");
      json_degrees(&writer, address->longitude);
    } else {
      json_literal(&writer, ",\"lat\":null,\"lon\":null");
    }
    json_literal(&writer, ",\"kind\":");
    json_uint(&writer, address->kind);
    json_literal(&writer, ",\"importance\":");
    json_uint(&writer, address->importance);
    json_literal(&writer, ",\"matched\":");
    json_uint(&writer, address->matched);
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
