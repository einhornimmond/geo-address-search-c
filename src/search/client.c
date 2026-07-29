/** @cond INTERNAL */

#include "search/client.h"

#include "search/geo_index.h"
#include "types/geo_place_kind.h"
#include "types/photon_place_type.h"

#include "gradido_blockchain_core/utils/converter.h"

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

/** Most results one call may ask for; beyond that a search is a listing.
 *
 *  Taken from the index rather than chosen again here: the query cannot rank
 *  more places than @ref GEO_QUERY_LIMIT_MAX at once, and a second number of
 *  our own would be free to drift past it — silently, and only in the ordering. */
#define GEO_CLIENT_LIMIT_MAX GEO_QUERY_LIMIT_MAX

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
  GeoSearchOptions options = {.prefix_last = prefix_last};
  return geo_client_search_options(client, query, query_size, &options, out, limit);
}

/** Degrees as a caller writes them, in the fixed point the index works in. */
static int32_t degrees_e7(double degrees, double edge) {
  if (!(degrees > -edge && degrees < edge)) { /* also catches NaN */
    degrees = degrees > 0 ? edge : -edge;
  }
  return (int32_t)(degrees * 1.0e7 + (degrees < 0 ? -0.5 : 0.5));
}

size_t geo_client_search_options(
    const GeoClient *client,
    const char *query,
    size_t query_size,
    const GeoSearchOptions *options,
    GeoAddress *out,
    size_t limit
) {
  static const GeoSearchOptions PLAIN = {0};
  if (!options) options = &PLAIN;

  GeoQueryStats *stats = options->stats;
  if (stats) memset(stats, 0, sizeof(*stats));
  if (!client || !query || !query_size || !out || !limit) return 0;
  if (limit > GEO_CLIENT_LIMIT_MAX) limit = GEO_CLIENT_LIMIT_MAX;

  GeoQueryOptions asked = {
      .prefix_last = options->prefix_last,
      .has_position = options->has_position,
      .latitude_e7 = options->has_position ? degrees_e7(options->latitude, 90.0) : 0,
      .longitude_e7 = options->has_position ? degrees_e7(options->longitude, 180.0) : 0,
  };

  /* The folding scratch lives here, on this call's own stack — that is what
     makes two threads able to search the same client at the same time. */
  TextTokenizer tokenizer;
  GeoHit hits[GEO_CLIENT_LIMIT_MAX];

  size_t count = geo_index_query_options(
      &client->index, &tokenizer, query, query_size, &asked, hits, limit, stats
  );
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
    if (cur < writer->capacity) { memcpy(&writer->buffer[cur], text, writer->capacity - cur); }
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

/**
 * @brief Write a count as a JSON number.
 *
 *  The digits come from the core's converter — the same LR-algorithm this file
 *  used to carry a copy of, and one copy of it is enough.  Its size step tops
 *  out at nineteen digits, which every field written here stays far below: a
 *  kind, a weight, a count of words.
 */
static void json_uint(JsonWriter *writer, uint64_t value) {
  char text[24]; /* nineteen digits, the terminator, and room to spare */
  size_t digits = grdu_uint64_to_string(text, sizeof(text), value);
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

  cursor += grdu_uint64_to_string(&text[cursor], sizeof(text) - cursor, whole);

  text[cursor++] = '.'; /* over the terminator the converter just wrote */
  for (size_t i = 0; i < 7; ++i) { text[cursor + i] = '0'; } /* keep the leading zeros */
  /* The converter fills a field of known width from its end, so a short
     fraction leaves those zeros standing in front of it.  Zero itself is the
     one value it writes as a single digit at the front instead — and a zero
     fraction is already spelled by the seven that stand there. */
  if (fraction) { grdu_uint64_to_string_known_string_size(&text[cursor], fraction, 7); }
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
