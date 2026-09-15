/**
 * @file
 * @brief How often the search answers what a query meant — measured, not guessed.
 *
 *  Reads query files — drawn by make_queries.py, kept by hand like regression.tsv,
 *  or converted from other geocoders' suites under external/ — asks each query of
 *  an index the way an embedder asks it, and counts where the expected place stands
 *  among the answers.
 *
 *      zig build eval --release=fast
 *      zig-out/bin/geo_eval planet.gdx tests/eval/queries.tsv tests/eval/regression.tsv
 *      zig-out/bin/geo_eval planet.gdx tests/eval/queries.tsv --failures
 *      zig-out/bin/geo_eval planet.gdx tests/eval/queries.tsv --ranks before.tsv
 *
 *  ### A query file
 *
 *  Tab-separated, one query per line; `#` opens a comment line.  The first line
 *  that is not a comment names the columns, and every later line is read by those
 *  names — so a file carries only the columns it needs, in any order, and a column
 *  this tool does not know is passed over.
 *
 *  | column           | meaning                                                           |
 *  | ---------------- | ----------------------------------------------------------------- |
 *  | `category`       | required; dotted, the part before the first dot groups the report |
 *  | `query`          | required; the text as typed, a trailing space included            |
 *  | `prefix`         | 1 reads the last word as a beginning as well (default), 0 not     |
 *  | `lat`, `lon`     | centre of the map in degrees; both or neither                      |
 *  | `lang`           | reading the answer is spelled in, as GeoSearchOptions::language   |
 *  | `limit`          | answers the query passes within, for the *pass* rate; default 1   |
 *  | `expect_name`    | name of the right answer                                          |
 *  | `expect_number`  | house number it carries                                           |
 *  | `expect_street`  | street it lies on                                                 |
 *  | `expect_city`    | town it lies in                                                   |
 *  | `expect_postcode`| postcode it carries                                               |
 *  | `expect_lat/lon` | where it stands — together with `radius_m`, in metres             |
 *  | `context`        | free text for a reader                                            |
 *
 *  An empty field expects nothing.  At least one expectation has to be set.  Every
 *  expected text may name alternatives separated by `|` — a Nominatim test that
 *  says the village is Silum and the town Triesenberg accepts either as the town.
 *
 *  ### When an answer is the expected one
 *
 *  All expectations given have to hold.  Texts are compared the way geocoder-tester's
 *  `--loose-compare` does — case, marks and punctuation aside, see fold_loose() —
 *  and house numbers without spaces and case:
 *
 *  - **name** — the answer's name, or its name and number joined by a space, which
 *    is how a geocoder writes a door: `Dircksenstraße 51`;
 *  - **street** — the answer's name, because an answer carrying a number is its street;
 *  - **number** — the number found on the answer;
 *  - **city** — the answer's town, or its name where it carries no town, because a
 *    city filed as a county is its own town;
 *  - **postcode** — the answer's postcode, or one of them where the answer lists
 *    several separated by `;`;
 *  - **position** — within `radius_m` of `expect_lat/lon`.
 *
 *  The rank is the position of the first answer that holds, among the first ten,
 *  1-based.  A query *passes* when that rank is within its `limit`.
 *
 *  ### Absent places
 *
 *  A query whose answers miss is asked once more, to tell a ranking that failed
 *  from a place the index does not hold at all — the data the query was written
 *  against may be older or newer than the index.  That second look asks for the
 *  expected name or street, number, postcode and town, without a beginning, from
 *  the expected coordinate where there is one, for up to 256 answers.  A place not
 *  found even so is counted as *absent* and left out of every rate.
 *
 *  @whisper What was meant is written down first, so that what was answered can be held against it
 */

#include "search/client.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Answers one query is asked for — what a map shows below its search box. */
enum { EVAL_LIMIT = 10 };

/** Answers the second look may take to find a place at all. */
enum { EVAL_PRESENCE_LIMIT = 256 };

/** Columns one file may carry, known or not. */
enum { EVAL_COLUMN_MAX = 32 };

/** Longest line a query file may hold, terminator included. */
enum { EVAL_LINE_MAX = 4096 };

/** Distinct categories, and groups, one run may report; more are refused rather than merged. */
enum { EVAL_CATEGORY_MAX = 512 };

/** The columns this tool reads, by their index in @ref COLUMN_NAMES. */
typedef enum EvalColumn {
  COLUMN_CATEGORY,
  COLUMN_QUERY,
  COLUMN_PREFIX,
  COLUMN_LAT,
  COLUMN_LON,
  COLUMN_LANG,
  COLUMN_LIMIT,
  COLUMN_EXPECT_NAME,
  COLUMN_EXPECT_NUMBER,
  COLUMN_EXPECT_STREET,
  COLUMN_EXPECT_CITY,
  COLUMN_EXPECT_POSTCODE,
  COLUMN_EXPECT_LAT,
  COLUMN_EXPECT_LON,
  COLUMN_RADIUS,
  COLUMN_COUNT
} EvalColumn;

static const char *const COLUMN_NAMES[COLUMN_COUNT] = {
    "category",    "query",           "prefix",      "lat",           "lon",
    "lang",        "limit",           "expect_name", "expect_number", "expect_street",
    "expect_city", "expect_postcode", "expect_lat",  "expect_lon",    "radius_m",
};

/** Where each known column stands in the current file, or -1. */
typedef struct EvalLayout {
  int at[COLUMN_COUNT];
  bool read; /**< Set once the header line was seen. */
} EvalLayout;

/** One query as its file gives it.  The strings point into the line buffer; absent ones are "". */
typedef struct EvalQuery {
  const char *category;
  const char *text;
  bool prefix;
  bool has_position;
  double lat;
  double lon;
  const char *language; /**< NULL for the index's default. */
  unsigned limit;
  const char *expect_name;
  const char *expect_number;
  const char *expect_street;
  const char *expect_city;
  const char *expect_postcode;
  bool has_coordinate;
  double expect_lat;
  double expect_lon;
  double radius_m;
} EvalQuery;

/** Counts of one category, or of a group, or of everything. */
typedef struct EvalTally {
  char name[96];
  unsigned queries; /**< Every query asked, absent ones included. */
  unsigned absent;  /**< Places the index does not hold at all. */
  unsigned pass;    /**< Present queries answered within their limit. */
  unsigned top1;
  unsigned top3;
  unsigned top10;
  double reciprocal; /**< Sum of 1/rank over the present queries; 0 for a miss. */
} EvalTally;

/** A growable list of durations, in microseconds. */
typedef struct EvalTimes {
  double *values;
  size_t count;
  size_t capacity;
} EvalTimes;

/* -------------------------------------------------------------------------
 *  Reading a query file
 * ------------------------------------------------------------------------- */

/**
 * @brief Cut @p line into its fields in place.
 *
 *  Tabs become terminators; a trailing newline is dropped.  Every other character
 *  stays, a trailing space included, because a trailing space is part of what was
 *  typed.
 *
 *  @return Number of fields found, at most @p max.
 */
static size_t split_fields(char *line, char **fields, size_t max) {
  size_t length = strlen(line);
  while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';

  size_t count = 0;
  char *start = line;
  for (char *at = line;; ++at) {
    if (*at == '\t' || *at == '\0') {
      bool end = *at == '\0';
      if (count < max) fields[count++] = start;
      *at = '\0';
      if (end) break;
      start = at + 1;
    }
  }
  return count;
}

/** Parse a number field; an empty field is not a number. */
static bool parse_double(const char *text, double *out) {
  if (!text || !*text) return false;
  char *end = NULL;
  *out = strtod(text, &end);
  return end && *end == '\0';
}

/** Learn where the known columns stand from a header line; unknown ones are passed over. */
static bool read_layout(char **fields, size_t count, EvalLayout *layout) {
  for (int c = 0; c < COLUMN_COUNT; ++c) layout->at[c] = -1;
  for (size_t i = 0; i < count; ++i) {
    for (int c = 0; c < COLUMN_COUNT; ++c) {
      if (strcmp(fields[i], COLUMN_NAMES[c]) == 0) layout->at[c] = (int)i;
    }
  }
  layout->read = true;
  return layout->at[COLUMN_CATEGORY] >= 0 && layout->at[COLUMN_QUERY] >= 0;
}

/** The field of @p column, or "" where the file has no such column or the line is short. */
static const char *field_of(
    char **fields, size_t count, const EvalLayout *layout, EvalColumn column
) {
  int at = layout->at[column];
  return at >= 0 && (size_t)at < count ? fields[at] : "";
}

/**
 * @brief Read one line into @p query, or into @p layout when it is the header.
 *
 *  @return 1 for a query, 0 for a comment, a header or an empty line, -1 for a
 *          line that cannot be read — which stops the run, so a broken file does
 *          not quietly shrink the sample.
 */
static int parse_line(char *line, EvalLayout *layout, EvalQuery *query) {
  if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') return 0;
  char *fields[EVAL_COLUMN_MAX];
  size_t count = split_fields(line, fields, EVAL_COLUMN_MAX);
  if (!layout->read) return read_layout(fields, count, layout) ? 0 : -1;

  memset(query, 0, sizeof(*query));
  query->category = field_of(fields, count, layout, COLUMN_CATEGORY);
  query->text = field_of(fields, count, layout, COLUMN_QUERY);
  const char *prefix = field_of(fields, count, layout, COLUMN_PREFIX);
  query->prefix = !*prefix || strcmp(prefix, "1") == 0;

  bool has_lat = parse_double(field_of(fields, count, layout, COLUMN_LAT), &query->lat);
  bool has_lon = parse_double(field_of(fields, count, layout, COLUMN_LON), &query->lon);
  if (has_lat != has_lon) return -1; /* one degree alone describes no place */
  query->has_position = has_lat;

  const char *language = field_of(fields, count, layout, COLUMN_LANG);
  query->language = *language ? language : NULL;
  double limit = 1;
  const char *limit_text = field_of(fields, count, layout, COLUMN_LIMIT);
  if (*limit_text && (!parse_double(limit_text, &limit) || limit < 1)) return -1;
  query->limit = limit > EVAL_LIMIT ? EVAL_LIMIT : (unsigned)limit;

  query->expect_name = field_of(fields, count, layout, COLUMN_EXPECT_NAME);
  query->expect_number = field_of(fields, count, layout, COLUMN_EXPECT_NUMBER);
  query->expect_street = field_of(fields, count, layout, COLUMN_EXPECT_STREET);
  query->expect_city = field_of(fields, count, layout, COLUMN_EXPECT_CITY);
  query->expect_postcode = field_of(fields, count, layout, COLUMN_EXPECT_POSTCODE);

  bool has_expect_lat =
      parse_double(field_of(fields, count, layout, COLUMN_EXPECT_LAT), &query->expect_lat);
  bool has_expect_lon =
      parse_double(field_of(fields, count, layout, COLUMN_EXPECT_LON), &query->expect_lon);
  bool has_radius = parse_double(field_of(fields, count, layout, COLUMN_RADIUS), &query->radius_m);
  if (has_expect_lat != has_expect_lon || (has_expect_lat && !has_radius)) return -1;
  query->has_coordinate = has_expect_lat;

  bool expects = *query->expect_name || *query->expect_number || *query->expect_street ||
                 *query->expect_city || *query->expect_postcode || query->has_coordinate;
  return *query->category && *query->text && expects ? 1 : -1;
}

/* -------------------------------------------------------------------------
 *  Asking and judging
 * ------------------------------------------------------------------------- */

static double now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/** Metres between two points, flat-earth — close enough to tell 150 m from 3 km. */
static double distance_m(double lat1, double lon1, double lat2, double lon2) {
  const double rad = 3.14159265358979323846 / 180.0;
  double x = (lon2 - lon1) * rad * cos((lat1 + lat2) / 2.0 * rad);
  double y = (lat2 - lat1) * rad;
  return sqrt(x * x + y * y) * 6371000.0;
}

/** Base letters of U+00C0 … U+017F, one byte each; '_' where the character has none. */
static const char LATIN_BASE[] = "aaaaaaaceeeeiiii"
                                 "dnooooo_ouuuuyts"
                                 "aaaaaaaceeeeiiii"
                                 "dnooooo_ouuuuyty" /* U+00C0 */
                                 "aaaaaaccccccccdd"
                                 "ddeeeeeeeeeegggg"
                                 "gggghhhhiiiiiiii"
                                 "iiiijjkkklllllll" /* U+0100 */
                                 "lllnnnnnnnnnoooo"
                                 "oooorrrrrrssssss"
                                 "ssttttttuuuuuuuu"
                                 "uuuuwwyyyzzzzzzs"; /* U+0140 */

/**
 * @brief Fold @p text the way geocoder-tester's loose comparison does.
 *
 *  Lower case; the letters of Latin-1 and Latin Extended-A without their marks, `ß`
 *  as `ss`; every run of other characters — spaces, hyphens, apostrophes, dots — as
 *  one space, and none at either end.  So `Rue Georges Clémenceau` and `rue georges
 *  clemenceau` meet, and so do `Saint-Denis` and `Saint Denis`.  Characters beyond
 *  U+017F are kept as they are.  At most @p size - 1 bytes are written, terminated.
 *
 *  @return Bytes written.
 */
static size_t fold_loose(const char *text, size_t length, char *out, size_t size) {
  size_t used = 0;
  bool gap = false;
  for (size_t i = 0; i < length && used + 3 < size;) {
    unsigned char byte = (unsigned char)text[i];
    uint32_t code = byte;
    size_t width = 1;
    if (byte >= 0xC0 && byte < 0xE0 && i + 1 < length) {
      code = ((uint32_t)(byte & 0x1F) << 6) | ((unsigned char)text[i + 1] & 0x3F);
      width = 2;
    }
    char letters[3] = {0};
    if (code < 0x80) {
      bool alnum = (code >= '0' && code <= '9') || (code >= 'a' && code <= 'z') ||
                   (code >= 'A' && code <= 'Z');
      if (alnum) letters[0] = (char)(code >= 'A' && code <= 'Z' ? code + 32 : code);
    } else if (code == 0xDF || code == 0x1E9E) {
      letters[0] = 's';
      letters[1] = 's';
    } else if (code >= 0xC0 && code <= 0x17F && LATIN_BASE[code - 0xC0] != '_') {
      letters[0] = LATIN_BASE[code - 0xC0];
    } else if (code >= 0x80 && !(code >= 0xC0 && code <= 0x17F)) {
      /* beyond the table: kept, bytes as they came */
      for (size_t w = 0; w < width && used + 1 < size; ++w) out[used++] = text[i + w];
      gap = false;
      i += width;
      continue;
    }
    if (letters[0]) {
      if (gap && used) out[used++] = ' ';
      gap = false;
      for (size_t l = 0; letters[l] && used + 1 < size; ++l) out[used++] = letters[l];
    } else {
      gap = true;
    }
    i += width;
  }
  out[used] = '\0';
  return used;
}

/**
 * @brief Does the text of @p size bytes equal one of the `|`-separated @p alternatives?
 *
 *  Compared loosely — see fold_loose() — because the same street is written with and
 *  without its accent by two sources, and what is measured here is the ranking, not
 *  the spelling.  A NULL or empty text equals nothing.
 */
static bool is_one_of(const char *text, size_t size, const char *alternatives) {
  if (!text || !size) return false;
  char folded[512], wanted[512];
  size_t folded_size = fold_loose(text, size, folded, sizeof(folded));
  const char *start = alternatives;
  for (;;) {
    const char *bar = strchr(start, '|');
    size_t length = bar ? (size_t)(bar - start) : strlen(start);
    size_t wanted_size = fold_loose(start, length, wanted, sizeof(wanted));
    if (wanted_size == folded_size && memcmp(folded, wanted, folded_size) == 0) return true;
    if (!bar) return false;
    start = bar + 1;
  }
}

/**
 * @brief Is the house number of @p size bytes one of @p alternatives, written alike?
 *
 *  Numbers are compared without spaces and without case, because the same door is
 *  written `1 A`, `1a` and `1A`, and `1 - 3` beside `1-3`, by the dump and by the
 *  suites alike.  At most 31 bytes of either side are compared.
 */
static bool number_is_one_of(const char *text, size_t size, const char *alternatives) {
  if (!text || !size) return false;
  char folded[32];
  size_t length = 0;
  for (size_t i = 0; i < size && length + 1 < sizeof(folded); ++i) {
    if (text[i] == ' ') continue;
    folded[length++] = (char)(text[i] >= 'A' && text[i] <= 'Z' ? text[i] + 32 : text[i]);
  }
  const char *start = alternatives;
  for (;;) {
    const char *bar = strchr(start, '|');
    const char *end = bar ? bar : start + strlen(start);
    char wanted[32];
    size_t wanted_length = 0;
    for (const char *at = start; at < end && wanted_length + 1 < sizeof(wanted); ++at) {
      if (*at == ' ') continue;
      wanted[wanted_length++] = (char)(*at >= 'A' && *at <= 'Z' ? *at + 32 : *at);
    }
    if (wanted_length == length && memcmp(folded, wanted, length) == 0) return true;
    if (!bar) return false;
    start = bar + 1;
  }
}

/** Is `name number` one of @p alternatives? */
static bool door_is_one_of(const GeoAddress *answer, const char *alternatives) {
  if (!answer->name || !answer->number) return false;
  char door[512];
  int written = snprintf(
      door, sizeof(door), "%.*s %.*s", (int)answer->name_size, answer->name,
      (int)answer->number_size, answer->number
  );
  return written > 0 && (size_t)written < sizeof(door) &&
         is_one_of(door, (size_t)written, alternatives);
}

/** Is one of the answer's `;`-separated postcodes among @p alternatives? */
static bool postcode_is_one_of(const GeoAddress *answer, const char *alternatives) {
  const char *start = answer->postcode;
  const char *end = start ? start + answer->postcode_size : NULL;
  while (start && start < end) {
    const char *semicolon = memchr(start, ';', (size_t)(end - start));
    size_t length = semicolon ? (size_t)(semicolon - start) : (size_t)(end - start);
    if (is_one_of(start, length, alternatives)) return true;
    start = semicolon ? semicolon + 1 : end;
  }
  return false;
}

/**
 * @brief Is @p answer the place @p query expects?
 *
 *  @p relaxed asks the weaker question the presence check needs — is this the named
 *  thing at all: where a name or a street is expected, town and postcode are not
 *  held against the answer, and the position may be ten times the radius off, and
 *  25 km at least.  A city whose point stands 1.7 km from the one a suite wrote down
 *  is still the city; that it stands elsewhere is a failure to report, not an absence.
 */
static bool is_expected(const GeoAddress *answer, const EvalQuery *query, bool relaxed) {
  bool named = *query->expect_name || *query->expect_street;
  if (*query->expect_name && !is_one_of(answer->name, answer->name_size, query->expect_name) &&
      !door_is_one_of(answer, query->expect_name)) {
    return false;
  }
  if (*query->expect_street && !is_one_of(answer->name, answer->name_size, query->expect_street)) {
    return false;
  }
  if (*query->expect_number &&
      !number_is_one_of(answer->number, answer->number_size, query->expect_number)) {
    return false;
  }
  if (*query->expect_city && !(relaxed && named)) {
    bool town = answer->city ? is_one_of(answer->city, answer->city_size, query->expect_city)
                             : is_one_of(answer->name, answer->name_size, query->expect_city);
    if (!town) return false;
  }
  if (*query->expect_postcode && !(relaxed && named) &&
      !postcode_is_one_of(answer, query->expect_postcode)) {
    return false;
  }
  if (query->has_coordinate) {
    double reach = relaxed ? fmax(query->radius_m * 10.0, 25000.0) : query->radius_m;
    if (distance_m(answer->latitude, answer->longitude, query->expect_lat, query->expect_lon) >
        reach) {
      return false;
    }
  }
  return true;
}

/** 1-based position of the expected place among @p count answers, or 0. */
static unsigned rank_of(
    const GeoAddress *answers, size_t count, const EvalQuery *query, bool relaxed
) {
  for (size_t i = 0; i < count; ++i) {
    if (is_expected(&answers[i], query, relaxed)) return (unsigned)(i + 1);
  }
  return 0;
}

/** Append the first of the `|`-separated @p alternatives to @p text, with a space before. */
static void append_first(char *text, size_t size, size_t *used, const char *alternatives) {
  if (!*alternatives) return;
  const char *bar = strchr(alternatives, '|');
  size_t length = bar ? (size_t)(bar - alternatives) : strlen(alternatives);
  if (*used + length + 2 >= size) return;
  if (*used) text[(*used)++] = ' ';
  memcpy(text + *used, alternatives, length);
  *used += length;
  text[*used] = '\0';
}

/**
 * @brief Does the index hold the expected place at all?
 *
 *  Asked the question the index answers best, without a beginning: the expected
 *  name or street with its number; the postcode or the town beside them only where
 *  no coordinate says where to look, which is then the position asked from.  A test
 *  that names no place but a number — Nominatim's `Gnalpstrasse 0` — is asked with
 *  its own text.  Up to @ref EVAL_PRESENCE_LIMIT answers are looked through, and an
 *  answer is held against the relaxed expectation of is_expected().
 */
static bool is_present(const GeoClient *client, const EvalQuery *query) {
  char text[512] = "";
  size_t used = 0;
  const char *named = *query->expect_name ? query->expect_name : query->expect_street;
  if (*named) {
    append_first(text, sizeof(text), &used, named);
    /* a name written as a door already carries its number: `Dircksenstraße 51` */
    size_t number = strcspn(query->expect_number, "|");
    bool carried = number && used > number && text[used - number - 1] == ' ' &&
                   memcmp(text + used - number, query->expect_number, number) == 0;
    if (!carried) append_first(text, sizeof(text), &used, query->expect_number);
    if (!query->has_coordinate) {
      append_first(text, sizeof(text), &used, query->expect_postcode);
      if (!*query->expect_postcode) append_first(text, sizeof(text), &used, query->expect_city);
    }
  } else if (*query->expect_city || *query->expect_postcode) {
    append_first(text, sizeof(text), &used, query->expect_number);
    append_first(text, sizeof(text), &used, query->expect_postcode);
    append_first(text, sizeof(text), &used, query->expect_city);
  } else {
    append_first(text, sizeof(text), &used, query->text);
  }
  if (!used || used + 1 >= sizeof(text)) return false;
  text[used++] = ' '; /* a whole word, not a beginning */
  text[used] = '\0';

  GeoSearchOptions options = {
      .prefix_last = false,
      .has_position = query->has_coordinate,
      .latitude = query->expect_lat,
      .longitude = query->expect_lon,
      .language = query->language,
  };
  static GeoAddress answers[EVAL_PRESENCE_LIMIT];
  size_t count =
      geo_client_search_options(client, text, used, &options, answers, EVAL_PRESENCE_LIMIT);
  return rank_of(answers, count, query, true) > 0;
}

/* -------------------------------------------------------------------------
 *  Counting
 * ------------------------------------------------------------------------- */

/** The tally named @p name, opened on first use; NULL when the table is full. */
static EvalTally *tally_for(EvalTally *tallies, size_t *count, const char *name, size_t name_size) {
  for (size_t i = 0; i < *count; ++i) {
    if (strlen(tallies[i].name) == name_size && memcmp(tallies[i].name, name, name_size) == 0) {
      return &tallies[i];
    }
  }
  if (*count >= EVAL_CATEGORY_MAX || name_size >= sizeof(tallies[0].name)) return NULL;
  EvalTally *fresh = &tallies[(*count)++];
  memset(fresh, 0, sizeof(*fresh));
  memcpy(fresh->name, name, name_size);
  return fresh;
}

/** Add one query: @p rank 0 is a miss, and @p absent leaves the rates alone. */
static void tally_add(EvalTally *tally, unsigned rank, unsigned limit, bool absent) {
  ++tally->queries;
  if (absent) {
    ++tally->absent;
    return;
  }
  if (rank && rank <= limit) ++tally->pass;
  if (rank == 1) ++tally->top1;
  if (rank && rank <= 3) ++tally->top3;
  if (rank && rank <= 10) ++tally->top10;
  if (rank) tally->reciprocal += 1.0 / rank;
}

static double percent(unsigned part, unsigned whole) {
  return whole ? 100.0 * part / whole : 0.0;
}

static void print_tally(const EvalTally *tally) {
  unsigned present = tally->queries - tally->absent;
  printf(
      "%-40s %7u %6u %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.3f\n", tally->name, tally->queries,
      tally->absent, percent(tally->pass, present), percent(tally->top1, present),
      percent(tally->top3, present), percent(tally->top10, present),
      present ? tally->reciprocal / present : 0.0
  );
}

static int compare_tally(const void *left, const void *right) {
  return strcmp(((const EvalTally *)left)->name, ((const EvalTally *)right)->name);
}

static int compare_double(const void *left, const void *right) {
  double a = *(const double *)left, b = *(const double *)right;
  return a < b ? -1 : (a > b ? 1 : 0);
}

static bool times_add(EvalTimes *times, double value) {
  if (times->count == times->capacity) {
    size_t grown = times->capacity ? times->capacity * 2 : 1024;
    double *values = realloc(times->values, grown * sizeof(*values));
    if (!values) return false;
    times->values = values;
    times->capacity = grown;
  }
  times->values[times->count++] = value;
  return true;
}

/** What a query expected, in one line for a reader. */
static void print_expected(const EvalQuery *query) {
  printf("    expected");
  if (*query->expect_name) printf(" name %s", query->expect_name);
  if (*query->expect_street) printf(" street %s", query->expect_street);
  if (*query->expect_number) printf(" number %s", query->expect_number);
  if (*query->expect_postcode) printf(" postcode %s", query->expect_postcode);
  if (*query->expect_city) printf(" town %s", query->expect_city);
  if (query->has_coordinate) {
    printf(" within %.0f m of %.5f,%.5f", query->radius_m, query->expect_lat, query->expect_lon);
  }
}

/** One answer as a reader wants to see it beside a failure. */
static void print_answer(const GeoAddress *answer, const EvalQuery *query, size_t position) {
  printf(
      "      %zu. %.*s%s%.*s, %.*s %.*s", position, (int)answer->name_size,
      answer->name ? answer->name : "—", answer->number ? " " : "", (int)answer->number_size,
      answer->number ? answer->number : "", (int)answer->postcode_size,
      answer->postcode ? answer->postcode : "", (int)answer->city_size,
      answer->city ? answer->city : "—"
  );
  if (query->has_coordinate) {
    printf(
        " — %.1f km from the expected place",
        distance_m(answer->latitude, answer->longitude, query->expect_lat, query->expect_lon) /
            1000.0
    );
  }
  printf(" (kind %u)\n", answer->kind);
}

/* -------------------------------------------------------------------------
 *  The run
 * ------------------------------------------------------------------------- */

static int usage(const char *program) {
  fprintf(
      stderr,
      "usage: %s <index.gdx> <queries.tsv>... [--failures] [--absent] [--ranks <out.tsv>]\n"
      "\n"
      "  --failures      print every query that did not pass, with the three answers\n"
      "                  that came instead\n"
      "  --absent        print every query whose place the index does not hold — a\n"
      "                  renamed street, or a door only interpolation would find\n"
      "  --ranks <file>  write each query's rank, for comparing two runs with diff:\n"
      "                  1 … 10 where the expected place stood, 0 for not among the\n"
      "                  first ten, -1 for a place the index does not hold\n",
      program
  );
  return 2;
}

int main(int argc, char **argv) {
  const char *index_path = NULL;
  const char *ranks_path = NULL;
  bool failures = false;
  bool absentees = false;
  const char *files[256];
  size_t file_count = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--failures") == 0) {
      failures = true;
    } else if (strcmp(argv[i], "--absent") == 0) {
      absentees = true;
    } else if (strcmp(argv[i], "--ranks") == 0 && i + 1 < argc) {
      ranks_path = argv[++i];
    } else if (argv[i][0] == '-') {
      return usage(argv[0]);
    } else if (!index_path) {
      index_path = argv[i];
    } else if (file_count < sizeof(files) / sizeof(files[0])) {
      files[file_count++] = argv[i];
    } else {
      return usage(argv[0]);
    }
  }
  if (!index_path || !file_count) return usage(argv[0]);

  GeoClient *client = NULL;
  GeoStatus status = geo_client_open(&client, index_path);
  if (status != GEO_OK) {
    fprintf(stderr, "cannot open index '%s' (status %d)\n", index_path, (int)status);
    return 1;
  }
  GeoClientInfo info = {0};
  geo_client_info(client, &info);

  FILE *ranks = NULL;
  if (ranks_path) {
    ranks = fopen(ranks_path, "w");
    if (!ranks) {
      fprintf(stderr, "cannot write '%s'\n", ranks_path);
      geo_client_close(client);
      return 1;
    }
    fprintf(ranks, "category\tquery\tlat\tlon\trank\n");
  }

  static EvalTally categories[EVAL_CATEGORY_MAX], groups[EVAL_CATEGORY_MAX];
  size_t category_count = 0, group_count = 0;
  EvalTally all = {.name = "all"};
  EvalTimes times = {0};
  int result = 0;

  for (size_t f = 0; f < file_count && result == 0; ++f) {
    FILE *input = fopen(files[f], "r");
    if (!input) {
      fprintf(stderr, "cannot read '%s'\n", files[f]);
      result = 1;
      break;
    }
    EvalLayout layout = {.read = false};
    char line[EVAL_LINE_MAX];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), input)) {
      ++line_number;
      EvalQuery query;
      int parsed = parse_line(line, &layout, &query);
      if (parsed == 0) continue;
      if (parsed < 0) {
        fprintf(
            stderr, "%s:%u: %s\n", files[f], line_number,
            layout.read ? "not a query line" : "the header names no category and no query column"
        );
        result = 1;
        break;
      }

      GeoSearchOptions options = {
          .prefix_last = query.prefix,
          .has_position = query.has_position,
          .latitude = query.lat,
          .longitude = query.lon,
          .language = query.language,
      };
      GeoAddress answers[EVAL_LIMIT];
      double started = now_us();
      size_t count = geo_client_search_options(
          client, query.text, strlen(query.text), &options, answers, EVAL_LIMIT
      );
      if (!times_add(&times, now_us() - started)) {
        fprintf(stderr, "out of memory\n");
        result = 1;
        break;
      }

      unsigned rank = rank_of(answers, count, &query, false);
      bool absent = rank == 0 && !is_present(client, &query);

      const char *dot = strchr(query.category, '.');
      size_t group_size = dot ? (size_t)(dot - query.category) : strlen(query.category);
      EvalTally *category =
          tally_for(categories, &category_count, query.category, strlen(query.category));
      EvalTally *group = tally_for(groups, &group_count, query.category, group_size);
      if (!category || !group) {
        fprintf(
            stderr, "%s:%u: more than %d categories\n", files[f], line_number, EVAL_CATEGORY_MAX
        );
        result = 1;
        break;
      }
      tally_add(category, rank, query.limit, absent);
      tally_add(group, rank, query.limit, absent);
      tally_add(&all, rank, query.limit, absent);

      if (ranks) {
        if (query.has_position) {
          fprintf(
              ranks, "%s\t%s\t%.6f\t%.6f\t%d\n", query.category, query.text, query.lat, query.lon,
              absent ? -1 : (int)rank
          );
        } else {
          fprintf(ranks, "%s\t%s\t\t\t%d\n", query.category, query.text, absent ? -1 : (int)rank);
        }
      }
      if (absentees && absent) {
        printf("\n%s  \"%s\"  — absent from the index\n", query.category, query.text);
        print_expected(&query);
        printf("\n");
      }
      if (failures && !absent && !(rank && rank <= query.limit)) {
        printf("\n%s  \"%s\"", query.category, query.text);
        if (query.has_position) printf("  from %.4f,%.4f", query.lat, query.lon);
        if (query.language) printf("  in %s", query.language);
        printf("\n");
        print_expected(&query);
        if (rank) {
          printf(" — found at %u, the limit is %u\n", rank, query.limit);
        } else {
          printf(" — not among the first ten\n");
        }
        for (size_t i = 0; i < count && i < 3; ++i) print_answer(&answers[i], &query, i + 1);
        if (!count) printf("      no answers at all\n");
      }
    }
    fclose(input);
  }

  if (result == 0) {
    qsort(categories, category_count, sizeof(categories[0]), compare_tally);
    qsort(groups, group_count, sizeof(groups[0]), compare_tally);

    printf(
        "\nindex    %s — %llu places, format %u\n", index_path, (unsigned long long)info.documents,
        info.format
    );
    printf(
        "queries  %u from %zu file%s, %u of them for places the index does not hold\n\n",
        all.queries, file_count, file_count == 1 ? "" : "s", all.absent
    );
    printf(
        "%-40s %7s %6s %7s %7s %7s %7s %6s\n", "category", "queries", "absent", "pass", "top1",
        "top3", "top10", "MRR"
    );
    for (size_t i = 0; i < category_count; ++i) print_tally(&categories[i]);
    printf("\n");
    for (size_t i = 0; i < group_count; ++i) print_tally(&groups[i]);
    print_tally(&all);

    if (times.count) {
      qsort(times.values, times.count, sizeof(double), compare_double);
      printf(
          "\nlatency  median %.2f ms, p95 %.2f ms, slowest %.2f ms — one search each, the first\n"
          "         ones against pages the operating system may not have fetched yet\n",
          times.values[times.count / 2] / 1000.0, times.values[times.count * 95 / 100] / 1000.0,
          times.values[times.count - 1] / 1000.0
      );
    }
  }

  free(times.values);
  if (ranks) fclose(ranks);
  geo_client_close(client);
  return result;
}
