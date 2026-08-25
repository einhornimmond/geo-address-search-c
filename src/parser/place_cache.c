/** @cond INTERNAL */

#include "parser/place_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/**
 * Free space demanded, as a fraction of the compressed dump, in sixteenths.
 *
 * Measured rather than guessed: the planet dump of July 2026 is 24.21 GB packed
 * and leaves a cache of 28.9 GB — 119 % of it.  Twenty-four sixteenths is 150 %,
 * which carries that with a quarter of the dump to spare and still refuses a
 * disk that would fill up halfway through.  It was 200 % once, and that number
 * turned away a 52 GB partition made for exactly this.
 */
#define PLACE_CACHE_HEADROOM_16 24

/** A record longer than this is not a record but a damaged file. */
#define PLACE_CACHE_RECORD_CEILING (16u * 1024u * 1024u)

/** Written for a field the entry never carried; no text is this long. */
#define PLACE_CACHE_ABSENT 0xFFFFu

/* =========================================================================
 *  Where the files lie
 * ========================================================================= */

/** Build "<directory>/photon-cache-<thread>.<kind>"; the caller frees it. */
static char *file_path(const char *directory, unsigned thread, PlaceCacheKind kind) {
  const char *suffix = kind == PLACE_CACHE_HOUSES ? "houses" : "documents";
  size_t size = strlen(directory) + 64;
  char *path = malloc(size);
  if (!path) return NULL;
  snprintf(path, size, "%s/photon-cache-%u.%s", directory, thread, suffix);
  return path;
}

/** Build "<directory>/photon-cache.manifest"; the caller frees it. */
static char *manifest_path(const char *directory) {
  size_t size = strlen(directory) + 32;
  char *path = malloc(size);
  if (!path) return NULL;
  snprintf(path, size, "%s/photon-cache.manifest", directory);
  return path;
}

/**
 * @brief Make @p path and every directory above it that is missing.
 *
 *  Walks the path from the front and makes what is not there.  A component that
 *  already exists is stepped over, whatever it is — the check that the whole is
 *  a directory happens afterwards, on the whole.
 *
 *  @return true when the path names a directory at the end of it.
 */
static bool make_directories(const char *path) {
  char *copy = strdup(path);
  if (!copy) return false;

  for (char *at = copy + 1; *at; ++at) {
    if (*at != '/') continue;
    *at = '\0';
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
      free(copy);
      return false;
    }
    *at = '/';
  }
  bool made = mkdir(copy, 0755) == 0 || errno == EEXIST;
  free(copy);
  return made;
}

uint64_t place_cache_wanted(uint64_t dump_bytes) {
  return dump_bytes / 16 * PLACE_CACHE_HEADROOM_16 + dump_bytes % 16 * PLACE_CACHE_HEADROOM_16 / 16;
}

PlaceCacheRoom place_cache_make_room(
    const char *directory, uint64_t dump_bytes, uint64_t *out_free
) {
  if (out_free) *out_free = 0;
  if (!directory || !*directory) return PLACE_CACHE_ROOM_UNMAKEABLE;

  struct stat status;
  if (stat(directory, &status) != 0) {
    if (!make_directories(directory)) return PLACE_CACHE_ROOM_UNMAKEABLE;
    if (stat(directory, &status) != 0) return PLACE_CACHE_ROOM_UNMAKEABLE;
  }
  if (!S_ISDIR(status.st_mode)) return PLACE_CACHE_ROOM_NOT_DIRECTORY;
  if (access(directory, W_OK) != 0) return PLACE_CACHE_ROOM_UNWRITABLE;

  struct statvfs space;
  if (statvfs(directory, &space) != 0) return PLACE_CACHE_ROOM_UNWRITABLE;
  uint64_t free_bytes = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
  if (out_free) *out_free = free_bytes;

  uint64_t wanted = place_cache_wanted(dump_bytes);
  return free_bytes >= wanted ? PLACE_CACHE_ROOM_OK : PLACE_CACHE_ROOM_TOO_SMALL;
}

const char *place_cache_room_reason(PlaceCacheRoom room) {
  switch (room) {
  case PLACE_CACHE_ROOM_OK:
    return "there is room";
  case PLACE_CACHE_ROOM_UNMAKEABLE:
    return "the directory is not there and could not be made";
  case PLACE_CACHE_ROOM_NOT_DIRECTORY:
    return "that name is taken by something that is not a directory";
  case PLACE_CACHE_ROOM_UNWRITABLE:
    return "the directory cannot be written to";
  case PLACE_CACHE_ROOM_TOO_SMALL:
    return "there is not enough room";
  }
  return "the directory cannot be used";
}

/* =========================================================================
 *  The manifest — what binds a cache to one dump and one build
 * ========================================================================= */

/**
 * @brief Fold the languages of a run into one number.
 *
 *  FNV-1a over the tags in the order they were given, because the order is
 *  what decides which reading a document record keeps — `de,en` and `en,de`
 *  build different indexes and must not share a cache.  A build that asked for
 *  every tag the dump offers folds a mark of its own, since no list describes
 *  it.
 */
static uint64_t languages_hash(const PhotonLanguages *languages) {
  uint64_t hash = 1469598103934665603u; /* FNV offset basis */
  if (!languages) return hash;
  if (languages->every) {
    hash ^= (uint64_t)'*';
    hash *= 1099511628211u;
    return hash;
  }
  for (uint8_t i = 0; i < languages->count; ++i) {
    for (const char *c = languages->tag[i]; *c; ++c) {
      hash ^= (uint64_t)(unsigned char)*c;
      hash *= 1099511628211u;
    }
    hash ^= (uint64_t)',';
    hash *= 1099511628211u;
  }
  return hash;
}

bool place_cache_stamp_of(
    const char *dump_path, unsigned threads, const PhotonLanguages *languages, PlaceCacheStamp *out
) {
  if (!dump_path || !out) return false;
  struct stat status;
  if (stat(dump_path, &status) != 0) return false;
  memset(out, 0, sizeof(*out));
  out->layout = PLACE_CACHE_LAYOUT;
  out->threads = threads;
  out->dump_bytes = (uint64_t)status.st_size;
  out->dump_mtime = (uint64_t)status.st_mtime;
  out->languages = languages_hash(languages);
  return true;
}

arnm_result place_cache_seal(const char *directory, const PlaceCacheStamp *stamp) {
  if (!directory || !stamp) return ARNM_ERROR_NULL_POINTER;
  char *path = manifest_path(directory);
  if (!path) return ARNM_ERROR_OUT_OF_MEMORY;

  arnm_result result = ARNM_ERROR_ENCODE_FAILED;
  FILE *file = fopen(path, "wb");
  if (file) {
    /* The magic goes down with the rest, and the file appears whole or not at
       all — a half-written manifest would claim a cache that is not there. */
    if (fwrite(PLACE_CACHE_MAGIC, 8, 1, file) == 1 && fwrite(stamp, sizeof(*stamp), 1, file) == 1) {
      result = ARNM_SUCCESS;
    }
    if (fclose(file) != 0) result = ARNM_ERROR_ENCODE_FAILED;
    if (result != ARNM_SUCCESS) remove(path);
  }
  free(path);
  return result;
}

bool place_cache_is_current(const char *directory, const PlaceCacheStamp *want) {
  if (!directory || !want) return false;
  char *path = manifest_path(directory);
  if (!path) return false;

  bool current = false;
  FILE *file = fopen(path, "rb");
  if (file) {
    char magic[8];
    PlaceCacheStamp found;
    if (fread(magic, sizeof(magic), 1, file) == 1 && memcmp(magic, PLACE_CACHE_MAGIC, 8) == 0 &&
        fread(&found, sizeof(found), 1, file) == 1) {
      current = found.layout == want->layout && found.threads == want->threads &&
                found.dump_bytes == want->dump_bytes && found.dump_mtime == want->dump_mtime &&
                found.languages == want->languages;
    }
    fclose(file);
  }
  free(path);

  /* the manifest may agree and a file still be missing — the run that wrote it
     could have been killed between the two */
  for (unsigned t = 0; current && t < want->threads; ++t) {
    for (int k = 0; k < 2; ++k) {
      char *file_name = file_path(directory, t, (PlaceCacheKind)k);
      if (!file_name || access(file_name, R_OK) != 0) current = false;
      free(file_name);
    }
  }
  return current;
}

uint64_t place_cache_size(const char *directory) {
  if (!directory) return 0;
  return place_cache_bytes(directory, PLACE_CACHE_THREADS_MAX, PLACE_CACHE_DOCUMENTS) +
         place_cache_bytes(directory, PLACE_CACHE_THREADS_MAX, PLACE_CACHE_HOUSES);
}

void place_cache_discard(const char *directory, unsigned threads) {
  if (!directory) return;
  char *path = manifest_path(directory);
  if (path) {
    remove(path);
    free(path);
  }
  /* Reach past what this run would write: a cache left by a run with more
     threads has files this one never names, and they would lie there taking up
     the room the new cache needs and never be looked at again. */
  if (threads < PLACE_CACHE_THREADS_MAX) threads = PLACE_CACHE_THREADS_MAX;
  for (unsigned t = 0; t < threads; ++t) {
    for (int k = 0; k < 2; ++k) {
      char *file_name = file_path(directory, t, (PlaceCacheKind)k);
      if (file_name) {
        remove(file_name);
        free(file_name);
      }
    }
  }
}

uint64_t place_cache_bytes(const char *directory, unsigned threads, PlaceCacheKind kind) {
  if (!directory) return 0;
  uint64_t total = 0;
  for (unsigned t = 0; t < threads; ++t) {
    char *path = file_path(directory, t, kind);
    if (!path) continue;
    struct stat status;
    if (stat(path, &status) == 0) total += (uint64_t)status.st_size;
    free(path);
  }
  return total;
}

/* =========================================================================
 *  Writing
 * ========================================================================= */

/** One record under construction: a buffer that grows and never shrinks. */
typedef struct RecordBuilder {
  char **buffer;
  size_t *capacity;
  size_t used;
  bool overflowed;
} RecordBuilder;

/**
 * @brief Append raw bytes, growing the buffer when they do not fit.
 *
 *  The writing family below all comes through here, and so does the one way it
 *  can fail: a growth that cannot be served sets @c overflowed and every later
 *  put becomes a no-op.  Nothing is checked at each call site, and the record is
 *  discarded once, at the end, by whoever reads that flag.
 */
static void put_bytes(RecordBuilder *b, const void *data, size_t size) {
  if (b->overflowed) return;
  if (b->used + size > *b->capacity) {
    size_t wanted = (b->used + size) * 2;
    char *grown = realloc(*b->buffer, wanted);
    if (!grown) {
      b->overflowed = true;
      return;
    }
    *b->buffer = grown;
    *b->capacity = wanted;
  }
  memcpy(*b->buffer + b->used, data, size);
  b->used += size;
}

/* Fixed-width writes in host byte order, and doubles in whatever the host calls
   IEEE 754. The format is therefore not portable, and nothing in it says so: no
   field records the byte order and nothing validates one. PLACE_CACHE_MAGIC is no
   help there — eight raw characters read the same on either endianness, so it
   answers "is this one of ours", not "was it written the way we read".

   What keeps a foreign-endian cache from being misread today is an accident
   rather than a check: the layout number in every file header, and the whole
   stamp in the manifest, are themselves read byte-swapped, so 1 arrives as
   16777216 and the comparison fails. The cache is refused and the dump is
   unpacked instead — the right outcome, reached by luck. Anyone moving a cache
   between machines of different byte order, or making that supported, has to add
   a byte-order field and check it. */

static void put_u8(RecordBuilder *b, uint8_t value) {
  put_bytes(b, &value, 1);
}
static void put_i32(RecordBuilder *b, int32_t value) {
  put_bytes(b, &value, 4);
}
static void put_f64(RecordBuilder *b, double value) {
  put_bytes(b, &value, 8);
}

/**
 * @brief Write one text: its length, its bytes, and the terminator behind them.
 *
 *  The terminator is stored rather than added on reading, so a text out of the
 *  cache is the same thing a text out of the JSON parser is — NUL-terminated,
 *  with a length that does not count the NUL.  Anything that took one and
 *  reached past @c size still finds what it expected.
 */
static void put_string(RecordBuilder *b, PhotonString text) {
  if (!text.data) {
    uint16_t absent = PLACE_CACHE_ABSENT;
    put_bytes(b, &absent, 2);
    return;
  }
  uint16_t size = text.size < PLACE_CACHE_ABSENT ? (uint16_t)text.size : PLACE_CACHE_ABSENT - 1;
  put_bytes(b, &size, 2);
  put_bytes(b, text.data, size);
  put_u8(b, 0);
}

/** Does this entry become a document of its own? Mirrors the builder's rule. */
static bool becomes_document(const PhotonPlace *place) {
  return !place->house.data && place->typeEnum != PHOTON_PLACE_TYPE_HOUSE;
}

arnm_result place_cache_writer_open(
    PlaceCacheWriter *writer, const char *directory, unsigned thread
) {
  if (!writer || !directory) return ARNM_ERROR_NULL_POINTER;
  memset(writer, 0, sizeof(*writer));

  writer->capacity = 64 * 1024;
  writer->buffer = malloc(writer->capacity);
  if (!writer->buffer) return ARNM_ERROR_OUT_OF_MEMORY;

  for (int k = 0; k < 2; ++k) {
    writer->path[k] = file_path(directory, thread, (PlaceCacheKind)k);
    if (!writer->path[k]) {
      place_cache_writer_remove(writer);
      return ARNM_ERROR_OUT_OF_MEMORY;
    }
    writer->file[k] = fopen(writer->path[k], "wb");
    if (!writer->file[k]) {
      place_cache_writer_remove(writer);
      return ARNM_ERROR_ENCODE_FAILED;
    }
    /* a megabyte of buffer per file: the write is sequential and wants to
       arrive at the disk in pieces the disk likes */
    setvbuf(writer->file[k], NULL, _IOFBF, 1u << 20);

    uint32_t header[2] = {PLACE_CACHE_LAYOUT, (uint32_t)k};
    if (fwrite(PLACE_CACHE_MAGIC, 8, 1, writer->file[k]) != 1 ||
        fwrite(header, sizeof(header), 1, writer->file[k]) != 1) {
      place_cache_writer_remove(writer);
      return ARNM_ERROR_ENCODE_FAILED;
    }
    writer->bytes[k] = 8 + sizeof(header);
  }
  return ARNM_SUCCESS;
}

arnm_result place_cache_write(PlaceCacheWriter *writer, const PhotonPlace *place) {
  if (!writer || !place) return ARNM_ERROR_NULL_POINTER;

  /* Everything that is not a document of its own goes to the other file, the
     numbered doors and the house-level entries the dump left without a number
     alike.  The latter become nothing at all in the third pass, which passes
     over them — but the first pass still takes their town and their postal
     code into the dictionary, and a cache that dropped them would build a
     smaller dictionary than the dump does. */
  PlaceCacheKind kind = becomes_document(place) ? PLACE_CACHE_DOCUMENTS : PLACE_CACHE_HOUSES;

  RecordBuilder builder = {
      .buffer = &writer->buffer, .capacity = &writer->capacity, .used = 4, .overflowed = false
  };
  put_u8(&builder, (uint8_t)place->typeEnum);
  put_u8(&builder, place->has_point ? 1u : 0u);

  if (kind == PLACE_CACHE_DOCUMENTS) {
    put_u8(&builder, place->search_count);
    put_u8(&builder, place->search_dropped);
    put_i32(&builder, place->lat_e7);
    put_i32(&builder, place->lon_e7);
    put_f64(&builder, place->importance);
    put_string(&builder, place->own_name);
    put_string(&builder, place->city);
    put_string(&builder, place->postcode);
    for (uint8_t i = 0; i < place->search_count; ++i) { put_string(&builder, place->search[i]); }
    /* The localized readings ride along at the end of the record: a build that
       asks for no language writes two zero bytes and nothing more, which is
       what keeps a single-language cache the size it always was. */
    put_u8(&builder, place->variant_count);
    put_u8(&builder, place->variant_dropped);
    for (uint8_t i = 0; i < place->variant_count; ++i) {
      put_bytes(&builder, place->variants[i].tag, PHOTON_LANGUAGE_TAG_MAX);
      put_string(&builder, place->variants[i].name);
      put_string(&builder, place->variants[i].city);
    }
  } else {
    put_u8(&builder, 0);
    put_u8(&builder, 0);
    put_i32(&builder, place->lat_e7);
    put_i32(&builder, place->lon_e7);
    put_string(&builder, place->street);
    put_string(&builder, place->city);
    put_string(&builder, place->postcode);
    put_string(&builder, place->house);
  }
  if (builder.overflowed) return ARNM_ERROR_OUT_OF_MEMORY;

  uint32_t payload = (uint32_t)(builder.used - 4);
  memcpy(writer->buffer, &payload, 4);
  if (fwrite(writer->buffer, builder.used, 1, writer->file[kind]) != 1) {
    return ARNM_ERROR_ENCODE_FAILED;
  }
  ++writer->count[kind];
  writer->bytes[kind] += builder.used;
  return ARNM_SUCCESS;
}

void place_cache_writer_close(PlaceCacheWriter *writer) {
  if (!writer) return;
  for (int k = 0; k < 2; ++k) {
    if (writer->file[k]) {
      fclose(writer->file[k]);
      writer->file[k] = NULL;
    }
  }
  free(writer->buffer);
  writer->buffer = NULL;
  writer->capacity = 0;
}

void place_cache_writer_remove(PlaceCacheWriter *writer) {
  if (!writer) return;
  place_cache_writer_close(writer);
  for (int k = 0; k < 2; ++k) {
    if (writer->path[k]) {
      remove(writer->path[k]);
      free(writer->path[k]);
      writer->path[k] = NULL;
    }
  }
}

/* =========================================================================
 *  Reading
 * ========================================================================= */

arnm_result place_cache_reader_open(
    PlaceCacheReader *reader, const char *directory, unsigned thread, PlaceCacheKind kind
) {
  if (!reader || !directory) return ARNM_ERROR_NULL_POINTER;
  memset(reader, 0, sizeof(*reader));

  char *path = file_path(directory, thread, kind);
  if (!path) return ARNM_ERROR_OUT_OF_MEMORY;
  reader->file = fopen(path, "rb");
  free(path);
  if (!reader->file) return ARNM_ERROR_DECODE_FAILED;
  setvbuf(reader->file, NULL, _IOFBF, 1u << 20);

  char magic[8];
  uint32_t header[2];
  if (fread(magic, sizeof(magic), 1, reader->file) != 1 ||
      memcmp(magic, PLACE_CACHE_MAGIC, 8) != 0 ||
      fread(header, sizeof(header), 1, reader->file) != 1 || header[0] != PLACE_CACHE_LAYOUT ||
      header[1] != (uint32_t)kind) {
    fclose(reader->file);
    memset(reader, 0, sizeof(*reader));
    return ARNM_ERROR_INVALID_PARAM;
  }

  reader->kind = kind;
  reader->capacity = 64 * 1024;
  reader->buffer = malloc(reader->capacity);
  if (!reader->buffer) {
    fclose(reader->file);
    memset(reader, 0, sizeof(*reader));
    return ARNM_ERROR_OUT_OF_MEMORY;
  }
  reader->bytes = sizeof(magic) + sizeof(header);
  return ARNM_SUCCESS;
}

/** Walk a record: the cursor moves, the strings stay where they lie. */
typedef struct RecordCursor {
  const char *data;
  size_t size;
  size_t position;
  bool short_of_bytes;
} RecordCursor;

/**
 * @brief Borrow @p size bytes from the cursor, or refuse for good.
 *
 *  The mirror of put_bytes(): the reading family comes through here, and a
 *  record that runs short sets @c short_of_bytes once.  Every take after that
 *  answers with nothing, so a truncated file is caught at the end of the record
 *  rather than checked field by field.
 */
static const void *take(RecordCursor *c, size_t size) {
  if (c->short_of_bytes || c->position + size > c->size) {
    c->short_of_bytes = true;
    return NULL;
  }
  const void *at = c->data + c->position;
  c->position += size;
  return at;
}

/* Fixed-width reads mirroring the writes above; a short record yields zeros,
   and the cursor's flag is what says they are not real values. */

static uint8_t take_u8(RecordCursor *c) {
  const uint8_t *at = take(c, 1);
  return at ? *at : 0;
}

static int32_t take_i32(RecordCursor *c) {
  const void *at = take(c, 4);
  int32_t value = 0;
  if (at) memcpy(&value, at, 4);
  return value;
}

static double take_f64(RecordCursor *c) {
  const void *at = take(c, 8);
  double value = 0;
  if (at) memcpy(&value, at, 8);
  return value;
}

static PhotonString take_string(RecordCursor *c) {
  PhotonString text = {NULL, 0};
  const void *at = take(c, 2);
  if (!at) return text;
  uint16_t size;
  memcpy(&size, at, 2);
  if (size == PLACE_CACHE_ABSENT) return text;
  const char *bytes = take(c, (size_t)size + 1); /* the terminator travelled with it */
  if (!bytes) return text;
  text.data = bytes;
  text.size = size;
  return text;
}

bool place_cache_read(PlaceCacheReader *reader, PhotonPlace *out) {
  if (!reader || !reader->file || !out) return false;

  /* The end of the file and a record that will not be read both stop the walk,
     and they must not look alike: one is how a cache ends, the other is a cache
     that must not be trusted.  @c broken tells them apart, and the caller that
     ignores it would silently build an index from half its data. */
  uint32_t payload = 0;
  if (fread(&payload, sizeof(payload), 1, reader->file) != 1) {
    reader->broken = !feof(reader->file);
    return false;
  }
  if (payload > PLACE_CACHE_RECORD_CEILING) {
    reader->broken = true;
    return false;
  }

  if (payload > reader->capacity) {
    char *grown = realloc(reader->buffer, payload);
    if (!grown) {
      reader->broken = true;
      return false;
    }
    reader->buffer = grown;
    reader->capacity = payload;
  }
  if (payload && fread(reader->buffer, payload, 1, reader->file) != 1) {
    reader->broken = true;
    return false;
  }

  RecordCursor cursor = {.data = reader->buffer, .size = payload, .position = 0};
  photon_place_reset(out);
  out->typeEnum = (PhotonPlaceType)take_u8(&cursor);
  out->has_point = take_u8(&cursor) ? 1 : 0;
  uint8_t search_count = take_u8(&cursor);
  out->search_dropped = take_u8(&cursor);
  out->lat_e7 = take_i32(&cursor);
  out->lon_e7 = take_i32(&cursor);

  if (reader->kind == PLACE_CACHE_DOCUMENTS) {
    out->importance = take_f64(&cursor);
    out->own_name = take_string(&cursor);
    out->city = take_string(&cursor);
    out->postcode = take_string(&cursor);
    if (search_count > PHOTON_PLACE_SEARCH_MAX) {
      reader->broken = true;
      return false;
    }
    for (uint8_t i = 0; i < search_count; ++i) { out->search[i] = take_string(&cursor); }
    out->search_count = search_count;

    uint8_t variant_count = take_u8(&cursor);
    out->variant_dropped = take_u8(&cursor);
    if (variant_count > PHOTON_PLACE_VARIANT_MAX) {
      reader->broken = true;
      return false;
    }
    for (uint8_t i = 0; i < variant_count; ++i) {
      const void *tag = take(&cursor, PHOTON_LANGUAGE_TAG_MAX);
      if (tag) memcpy(out->variants[i].tag, tag, PHOTON_LANGUAGE_TAG_MAX);
      out->variants[i].tag[PHOTON_LANGUAGE_TAG_MAX - 1] = '\0';
      out->variants[i].name = take_string(&cursor);
      out->variants[i].city = take_string(&cursor);
    }
    out->variant_count = variant_count;
  } else {
    out->street = take_string(&cursor);
    out->city = take_string(&cursor);
    out->postcode = take_string(&cursor);
    out->house = take_string(&cursor);
  }
  if (cursor.short_of_bytes) {
    reader->broken = true;
    return false;
  }

  ++reader->count;
  reader->bytes += sizeof(payload) + payload;
  return true;
}

void place_cache_reader_close(PlaceCacheReader *reader) {
  if (!reader) return;
  if (reader->file) fclose(reader->file);
  free(reader->buffer);
  memset(reader, 0, sizeof(*reader));
}

/** @endcond */
