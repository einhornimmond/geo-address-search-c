/** @defgroup parser Dump parsing
 *  @brief Everything that only the way in needs: unpacking a Photon dump,
 *         reading its lines, handing them between threads and writing them
 *         down once so the later walks are spared the file.
 *
 *  Parent group for the modules the search never links.  What leaves here is
 *  a @ref PhotonPlace; what happens to it afterwards belongs to @ref search.
 */

/** @defgroup json_parse JSON parser
 *  @ingroup parser
 *  @brief Parse Photon JSONL lines through arnm's JSON reader.  Extracts one
 *         @ref PhotonPlace per Place content entry: a few fields for the
 *         answer, everything else as role-free search strings.
 *
 *  The dump denormalises the whole address chain into every entry, and it
 *  does so imperfectly — a state may be missing, a city name may sit in the
 *  state field, city and street may carry the same text.  None of that is
 *  repaired here, because none of it needs repairing: a search index asks
 *  which words belong to a coordinate, never which role a word plays.  Only
 *  the fields that appear in an answer keep their role.
 *  @{
 */

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types/photon_place_type.h"

/** A borrowed string with its byte length — points into the parsed document. */
typedef struct PhotonString {
  const char *data; /**< NUL-terminated text, or NULL when the field was absent. */
  size_t size;      /**< Byte length without the terminating NUL. */
} PhotonString;

/** Room for a language tag and its terminator — `de`, `en`, `pt-BR`. */
enum { PHOTON_LANGUAGE_TAG_MAX = 8 };

/** Languages one build may keep, and localized readings one entry may carry.
 *
 *  Both are small on purpose.  A dump offers thirty translations of a capital
 *  city and none of a field path; keeping all of them everywhere is what makes
 *  an index unbuildable, so a build names the few it wants. */
enum { PHOTON_LANGUAGE_MAX = 32, PHOTON_PLACE_VARIANT_MAX = 32 };

/**
 * @brief The languages a build reads out of the dump.
 *
 *  The first is the one an answer shows by default and the one written into
 *  the document record; the rest become variants beside it.  Empty means the
 *  build asked for nothing beyond the unlocalized reading.
 */
typedef struct PhotonLanguages {
  char tag[PHOTON_LANGUAGE_MAX][PHOTON_LANGUAGE_TAG_MAX]; /**< Lowercase, NUL-terminated. */
  uint8_t tag_size[PHOTON_LANGUAGE_MAX];                  /**< Length of each tag. */
  /** The same tags as one integer each — length in the low byte, the letters
   *  above it.  A tag is two or three bytes, and asking the C library to
   *  compare that many costs more in call overhead than the comparison does in
   *  work; as a number it is a single instruction.  On a planet dump this is
   *  asked once per localized address key, which is tens of millions of times. */
  uint64_t tag_word[PHOTON_LANGUAGE_MAX];
  uint8_t count; /**< Tags in use. */
  bool every;    /**< Take whatever tag the dump offers, up to the cap. */
  /** The keys of the default reading — `"name:de"`, `"city:de"`, `"street:de"` —
   *  built once here rather than per entry.  They are the same for every one of
   *  a planet's 356 million entries, and both building and searching for them
   *  cost more than the fields they name.  Recognising the two address keys by
   *  a whole-string compare is what keeps the colon out of the hot path: an
   *  entry that becomes no document never has to look for one. */
  char name_key[PHOTON_LANGUAGE_TAG_MAX + 6];
  char city_key[PHOTON_LANGUAGE_TAG_MAX + 6];
  char street_key[PHOTON_LANGUAGE_TAG_MAX + 8];
  uint8_t city_key_size;   /**< Length of @c city_key; 0 where there is none. */
  uint8_t street_key_size; /**< Length of @c street_key; 0 where there is none. */
} PhotonLanguages;

/**
 * @brief One entry as one language writes it.
 *
 *  Only what actually differs is kept: a variant exists where the dump named a
 *  reading of its own, and its fields are NULL where it did not.  A street in
 *  Prague carries a Czech `name` and an English `city`, and nothing else.
 */
typedef struct PhotonVariant {
  char tag[PHOTON_LANGUAGE_TAG_MAX]; /**< Which language, NUL-terminated. */
  PhotonString name;                 /**< The entry's own name in it, or NULL. */
  PhotonString city;                 /**< The town of its address in it, or NULL. */
} PhotonVariant;

/**
 * @brief Read a comma-separated list of tags — `de,en,fr`, or `all`.
 *
 *  Tags are lowercased, trimmed of spaces and deduplicated; an empty piece is
 *  passed over.  `all` sets @c every and leaves the list itself empty, which
 *  makes the first reading the dump offers the default one.
 *
 *  @param[in]  text  The list as written; NULL or empty yields a count of 0.
 *  @param[out] out   Zeroed first, then filled.
 *  @return false when a tag was too long or there were more than
 *          @ref PHOTON_LANGUAGE_MAX of them; @p out then holds what fit.
 */
bool photon_languages_parse(const char *text, PhotonLanguages *out);

/** Whether @p tag is one of the languages @p languages asked for. */
bool photon_languages_hold(const PhotonLanguages *languages, const char *tag, size_t tag_size);

/** Where @p tag stands in the list, or -1 when it is not in it.
 *
 *  The place is what a variant record keeps, because a number is what the file
 *  is ordered by; the tag itself is written once, in the language table. */
int photon_languages_index(const PhotonLanguages *languages, const char *tag);

/** Capacity for the role-free search strings of one entry.
 *
 *  The address block carries a localized variant per language — a German
 *  entry measured 30 distinct texts on average, so this must sit well above
 *  that or names quietly fall off the end.  A build that asks for many
 *  languages raises that figure by roughly one text per language and level,
 *  which is why the cap is what it is and why @c search_dropped is worth
 *  watching: it counts what still did not fit. */
enum { PHOTON_PLACE_SEARCH_MAX = 128 };

/**
 * @brief One Photon Place content entry, split into answer and index.
 *
 *  Every string points into the parsed JSON document and is valid only
 *  during the callback that receives this struct.  Fields that were not
 *  present in the JSON have a NULL @c data pointer.
 *
 *  @c search holds every text of the entry that someone might type into a
 *  query — name variants in all languages, city, county, state, suburb and
 *  whatever else the address block carries — deduplicated within the entry
 *  and stripped of any role.  House entries leave it empty: they are the
 *  payload of their street, not something one searches for by name, and
 *  indexing their repeated parent text would multiply the term stream
 *  several times over.
 */
typedef struct PhotonPlace {
  const char *type;         /**< The address_type as written: "country", "state", … */
  PhotonPlaceType typeEnum; /**< The same, folded to a number; never UNKNOWN here. */
  PhotonString own_name;    /**< Display name in the default language, else the plain "name". */
  PhotonString street;      /**< Street of the address, or the entry itself. */
  PhotonString house;       /**< House number, verbatim: "12A", "12-14", … */
  PhotonString postcode;    /**< Postal code as written. */
  PhotonString city;        /**< City of the address, or the entry itself. */
  const char *country_code; /**< ISO 3166-1 alpha-2. */
  double importance;        /**< Photon's own weight; 0 when the entry named none. */
  int32_t lon_e7;           /**< Longitude × 10⁷; meaningless unless @c has_point. */
  int32_t lat_e7;           /**< Latitude × 10⁷; meaningless unless @c has_point. */
  int has_point;            /**< A centroid was present. */
  uint8_t search_count;     /**< How many of @c search are filled. */
  uint8_t search_dropped;   /**< How many did not fit; 0 in sane data. */
  uint8_t variant_count;    /**< How many of @c variants are filled. */
  uint8_t variant_dropped;  /**< How many did not fit. */
  /* The two arrays stand last, and the counts in front of them, so that
     photon_place_reset() can leave three kilobytes untouched — see there. */
  PhotonString search[PHOTON_PLACE_SEARCH_MAX];     /**< Role-free index texts. */
  PhotonVariant variants[PHOTON_PLACE_VARIANT_MAX]; /**< Readings beside the default one. */
} PhotonPlace;

/* Nothing may stand behind the arrays: photon_place_reset() zeroes what comes
   before them and nothing else, so a field added at the end would be read as
   whatever the last entry left there.  This says so at compile time. */
static_assert(
    sizeof(PhotonPlace) == offsetof(PhotonPlace, search) + sizeof(((PhotonPlace *)0)->search) +
                               sizeof(((PhotonPlace *)0)->variants),
    "a field was added behind the arrays of PhotonPlace; photon_place_reset() would miss it"
);

/**
 * @brief Empty an entry so it can be filled again.
 *
 *  Everything up to @c search is zeroed and the two arrays are not, because
 *  nothing ever reads past their counts and those counts stand in the part that
 *  is.  It is a small distinction and worth three kilobytes of writing per
 *  entry — on a planet dump, per entry means 356 million times.
 *
 *  @param[out] place  Entry to empty; must not be NULL.
 */
void photon_place_reset(PhotonPlace *place);

/**
 * @brief Whether the entry brought a coordinate of its own.
 *
 *  @param[in] place  The entry; not NULL.
 *  @return true when @c lon_e7 and @c lat_e7 hold a centroid the dump named.
 *          A house without one takes its street's position later, but that
 *          happens in the collector, not here.
 */
bool photon_place_has_point(const PhotonPlace *place);

/**
 * @brief Called once per Place content entry, with the entry still borrowed.
 *
 *  @param[in] place      The entry; every string in it points into the document
 *                        being parsed and dies with the call.
 *  @param[in] user_data  Whatever was handed to json_parse_line().
 *  @return 0 when the entry was taken.  A non-zero return does not stop the walk
 *          — the line is printed and parsing goes on to the next entry.
 */
typedef int (*PhotonPlaceCallback)(const PhotonPlace *place, void *user_data);

/**
 * @brief Per-document metadata returned by json_parse_line().
 *
 *  Callers aggregate these counters into a @c JsonStats.
 */
typedef struct {
  int is_valid;       /**< The root was a JSON object. */
  int is_place;       /**< …and its `type` was `"Place"`. */
  size_t entry_count; /**< Entries handed to the callback — the skipped ones not counted. */
} JsonParseResult;

/**
 * @brief Parse one JSONL line and invoke @p callback for each Place entry.
 *
 *  The reader and the arena it parses into are thread-local, and the arena
 *  grows to the longest line seen.  Each line is read once: the keys of an
 *  entry, of its `name` object and of its `address` block are walked in one
 *  pass each and dispatched by what they are, rather than asked for by name
 *  one at a time.
 *
 *  Strings in @ref PhotonPlace point into the parsed JSON document and are
 *  valid only during the callback — copy them if you need them to outlive
 *  the call.
 *
 *  An entry whose `address_type` is `"other"` and which carries no house number
 *  is passed over: it names nothing anyone searches for.  It is not counted in
 *  @c entry_count either.
 *
 *  @param[in]  line      JSON text; need not be terminated.
 *  @param[in]  len       Byte length of @p line.
 *  @param[in]  languages Localized readings to keep; NULL keeps only the
 *                        unlocalized one.  The first tag is the default —
 *                        what @c own_name and @c city come back as.
 *  @param[in]  callback  Invoked once per Place content entry that survives.
 *  @param[in]  user_data Forwarded to @p callback unchanged.
 *  @param[out] result    Document-level counts, for the statistics to add up.
 *  @return Always 1.  There is no failure return, because there is no failure a
 *          caller could act on.
 *  @warning Three conditions end the process through fatal() rather than coming
 *          back: JSON that will not parse, an entry without an `address_type`,
 *          and an `address_type` this build does not know.  A dump is either
 *          readable or it is not, and finding out halfway through a 24 GB file
 *          is worse than finding out at the first line.
 */
int json_parse_line(
    const char *line,
    size_t len,
    const PhotonLanguages *languages,
    PhotonPlaceCallback callback,
    void *user_data,
    JsonParseResult *result
);

/**
 * @brief Serialize a PhotonPlace to a compact JSON string for debugging.
 *
 *  Writes into a thread-local buffer of 4 KiB; the next call on the same thread
 *  overwrites it.  String values are escaped enough to be read back.
 *
 *  @param[in] place  Place to write out; NULL yields @c "null".
 *  @return The text, valid until this thread calls again.  Copy it to keep it.
 *
 *  @whisper A place speaks its full shape — for the debugger's eye alone
 */
char *photon_place_to_json(const PhotonPlace *place);

/** @} */
