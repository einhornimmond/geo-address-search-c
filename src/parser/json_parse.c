/** @cond INTERNAL */

#include "parser/json_parse.h"
#include "foundation/error.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "arnm/memory_block.h"

/* =========================================================================
 *  Field helpers
 * ========================================================================= */

/** Borrow a string value with its length; a non-string yields the empty borrow. */
static PhotonString string_of(const arnm_json_value *value) {
  PhotonString result = {NULL, 0};
  const char *data = NULL;
  uint32_t length = 0;
  if (ARNM_SUCCESS == arnm_json_read_string(value, &data, &length)) {
    result.data = data;
    result.size = length;
  }
  return result;
}

/** The member @p key names, or NULL where the object does not carry one. */
static arnm_json_value *member(const arnm_json_value *object, const char *key) {
  arnm_json_value *value = NULL;
  return ARNM_SUCCESS == arnm_json_object_get(object, key, &value) ? value : NULL;
}

/* =========================================================================
 *  The languages a build reads
 * ========================================================================= */

/** ASCII lower case; a language tag is ASCII by definition. */
static char lowered(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/**
 * @brief A language tag as one number — its length, then its letters.
 *
 *  Two tags are the same exactly when their numbers are.  An empty tag, or one
 *  of seven bytes or more, has no number and yields 0.  That answer is used,
 *  not merely guarded against: the tags a dump key carries reach here unchecked
 *  through photon_languages_hold() and languages_hold_word(), where 0 equals no
 *  stored number and the key is passed over.
 */
static uint64_t tag_word(const char *tag, size_t size) {
  if (!size || size >= PHOTON_LANGUAGE_TAG_MAX) return 0;
  uint64_t word = size;
  for (size_t i = 0; i < size; ++i) { word |= (uint64_t)(unsigned char)tag[i] << (8 * (i + 1)); }
  return word;
}

/**
 * @brief Is @p key exactly @p want?
 *
 *  The first two bytes are compared here rather than handed to the C library,
 *  because they settle it: `country` and `city:de` are both seven bytes long
 *  and part at the second letter, and every other key of the address block
 *  parts at the first.  What is left over goes to memcmp, which then runs only
 *  for the key this really is.
 */
static bool key_is(const char *key, size_t key_size, const char *want, uint8_t want_size) {
  if (key_size != want_size || key_size < 2) return false;
  if (key[0] != want[0] || key[1] != want[1]) return false;
  return key_size == 2 || memcmp(key + 2, want + 2, key_size - 2) == 0;
}

bool photon_languages_parse(const char *text, PhotonLanguages *out) {
  memset(out, 0, sizeof(*out));
  if (!text || !*text) return true;

  bool whole = true;
  const char *cursor = text;
  while (*cursor) {
    while (*cursor == ',' || *cursor == ' ') ++cursor;
    const char *start = cursor;
    while (*cursor && *cursor != ',' && *cursor != ' ') ++cursor;
    size_t size = (size_t)(cursor - start);
    if (!size) continue;

    if (size == 3 && lowered(start[0]) == 'a' && lowered(start[1]) == 'l' &&
        lowered(start[2]) == 'l') {
      /* "all" is not a language beside others — it is the absence of a list */
      memset(out, 0, sizeof(*out));
      out->every = true;
      return true;
    }
    if (size >= PHOTON_LANGUAGE_TAG_MAX) {
      whole = false;
      continue;
    }
    char tag[PHOTON_LANGUAGE_TAG_MAX] = {0};
    for (size_t i = 0; i < size; ++i) tag[i] = lowered(start[i]);
    if (photon_languages_hold(out, tag, size)) continue; /* named twice */
    if (out->count >= PHOTON_LANGUAGE_MAX) {
      whole = false;
      continue;
    }
    memcpy(out->tag[out->count], tag, sizeof(tag));
    out->tag_size[out->count] = (uint8_t)size;
    out->tag_word[out->count] = tag_word(tag, size);
    ++out->count;
  }
  if (out->count) {
    snprintf(out->name_key, sizeof(out->name_key), "name:%s", out->tag[0]);
    out->city_key_size =
        (uint8_t)snprintf(out->city_key, sizeof(out->city_key), "city:%s", out->tag[0]);
    out->street_key_size =
        (uint8_t)snprintf(out->street_key, sizeof(out->street_key), "street:%s", out->tag[0]);
  }
  return whole;
}

/** The same question as photon_languages_hold(), the tag already a number.
 *
 *  Static, so that the address loop keeps it inline: it is asked once per
 *  localized key of every entry that becomes a document, and a call frame
 *  around three integer compares is most of what the question costs. */
static bool languages_hold_word(const PhotonLanguages *languages, uint64_t word) {
  if (!word) return false;
  if (languages->every) return true;
  for (uint8_t i = 0; i < languages->count; ++i) {
    if (languages->tag_word[i] == word) return true;
  }
  return false;
}

bool photon_languages_hold(const PhotonLanguages *languages, const char *tag, size_t tag_size) {
  if (!languages) return false;
  return languages_hold_word(languages, tag_word(tag, tag_size));
}

int photon_languages_index(const PhotonLanguages *languages, const char *tag) {
  if (!languages || !tag) return -1;
  for (uint8_t i = 0; i < languages->count; ++i) {
    if (strcmp(languages->tag[i], tag) == 0) return (int)i;
  }
  return -1;
}

/** Whether @p tag is the default reading — the one the document record keeps. */
static bool tag_is_default(const PhotonLanguages *languages, const char *tag, size_t tag_size) {
  if (!languages || !languages->count) return false;
  return languages->tag_word[0] == tag_word(tag, tag_size);
}

/**
 * @brief The variant for @p tag, made where it is not there yet.
 *
 *  A linear scan again, over a list that holds one entry per language the build
 *  asked for.  Nothing cleverer would pay off at this length, and the list is
 *  walked once per localized key rather than once per entry.
 *
 *  @return The variant, or NULL when the tag will not fit or the list is full.
 */
static PhotonVariant *variant_for(PhotonPlace *p, const char *tag, size_t tag_size) {
  if (!tag_size || tag_size >= PHOTON_LANGUAGE_TAG_MAX) return NULL;
  for (uint8_t i = 0; i < p->variant_count; ++i) {
    if (strncmp(p->variants[i].tag, tag, tag_size) == 0 && p->variants[i].tag[tag_size] == '\0') {
      return &p->variants[i];
    }
  }
  if (p->variant_count >= PHOTON_PLACE_VARIANT_MAX) {
    if (p->variant_dropped < UINT8_MAX) ++p->variant_dropped;
    return NULL;
  }
  PhotonVariant *variant = &p->variants[p->variant_count++];
  memset(variant, 0, sizeof(*variant));
  for (size_t i = 0; i < tag_size; ++i) variant->tag[i] = lowered(tag[i]);
  return variant;
}

/**
 * @brief The entry's own name: the default reading into @c own_name, the rest beside it.
 *
 *  The `name` object holds one key per language the dump knows, and one walk over
 *  it settles all of them.  Both readings that can fill @c own_name are picked up
 *  as they go by — the localized one this build answers in, and the unlocalized
 *  one behind it — and which of the two wins is decided once the walk is over,
 *  because a JSON object promises no order.  Everything else is kept only where
 *  this build named the language: a planet's worth of translations of every
 *  village is what the variant list is small for.
 */
static void place_names(
    const arnm_json_value *names, const PhotonLanguages *languages, PhotonPlace *p
) {
  arnm_json_object_iter iter;
  if (ARNM_SUCCESS != arnm_json_object_iter_init(names, &iter)) return;

  /* nothing beyond the default reading was asked for */
  const bool wants_variants = languages && (languages->every || languages->count >= 2);

  PhotonString plain_reading = {NULL, 0};   /**< the unlocalized `name`. */
  PhotonString default_reading = {NULL, 0}; /**< `name:<default>`, where the dump has one. */

  const char *key = NULL;
  uint32_t key_size = 0;
  arnm_json_value *value = NULL;
  for (; arnm_json_object_iter_next(&iter, &key, &key_size, &value);) {
    if (key_size == 4 && memcmp(key, "name", 4) == 0) {
      plain_reading = string_of(value);
      continue;
    }
    if (key_size <= 5 || memcmp(key, "name:", 5) != 0) continue;
    const char *tag = key + 5;
    size_t tag_size = key_size - 5;
    if (tag_is_default(languages, tag, tag_size)) {
      default_reading = string_of(value);
      continue;
    }
    if (!wants_variants) continue;
    if (!photon_languages_hold(languages, tag, tag_size)) continue;
    PhotonString text = string_of(value);
    if (!text.data) continue;
    PhotonVariant *variant = variant_for(p, tag, tag_size);
    if (variant) variant->name = text;
  }

  /* the localized reading is what an answer shows; the plain one carries the
     entries the dump never translated */
  p->own_name = default_reading.data ? default_reading : plain_reading;
}

/**
 * @brief Fold the `address_type` string into a number — the whole string, or nothing.
 *
 *  The length comes in because it is already there: the parser keeps it beside every
 *  string value, so it costs no strlen to ask.  Having it, the dispatch is a
 *  switch on the length and one comparison — the nine values this build knows
 *  are spread over five lengths, and within a length the first byte tells them
 *  apart, so exactly one memcmp runs per entry.
 *
 *  The comparison is over the whole string on purpose.  Matching on a prefix
 *  would be a shade cheaper and would quietly file a value this build has never
 *  seen — a `"hamlet"` Photon might add tomorrow would become a house and be
 *  indexed as one.  json_parse_line() treats an unrecognised type as a dump it
 *  cannot read and stops; that stance is only worth anything if recognition is
 *  exact.  Measured at 1.1 ns per entry over the prefix version, some 0.2 s
 *  across a planet dump — against a pass that spends minutes unpacking it.
 *
 *  @param[in] type  The `address_type` value; may be NULL.
 *  @param[in] size  Its length in bytes, the terminator not counted.
 *  @return The matching type, or @ref PHOTON_PLACE_TYPE_UNKNOWN for everything
 *          else — an empty string, a short one, a longer one that starts alike.
 */
static PhotonPlaceType detectTypeEnum(const char *type, size_t size) {
  if (!type) return PHOTON_PLACE_TYPE_UNKNOWN;

  switch (size) {
  case 4:
    if (memcmp(type, "city", 4) == 0) return PHOTON_PLACE_TYPE_CITY;
    break;
  case 5:
    switch (type[0]) {
    case 'h':
      if (memcmp(type, "house", 5) == 0) return PHOTON_PLACE_TYPE_HOUSE;
      break;
    case 's':
      if (memcmp(type, "state", 5) == 0) return PHOTON_PLACE_TYPE_STATE;
      break;
    case 'o':
      if (memcmp(type, "other", 5) == 0) return PHOTON_PLACE_TYPE_OTHER;
      break;
    }
    break;
  case 6:
    switch (type[0]) {
    case 's':
      if (memcmp(type, "street", 6) == 0) return PHOTON_PLACE_TYPE_STREET;
      break;
    case 'c':
      if (memcmp(type, "county", 6) == 0) return PHOTON_PLACE_TYPE_COUNTY;
      break;
    }
    break;
  case 7:
    if (memcmp(type, "country", 7) == 0) return PHOTON_PLACE_TYPE_COUNTRY;
    break;
  case 8:
    switch (type[0]) {
    case 'd':
      if (memcmp(type, "district", 8) == 0) return PHOTON_PLACE_TYPE_DISTRICT;
      break;
    case 'l':
      if (memcmp(type, "locality", 8) == 0) return PHOTON_PLACE_TYPE_LOCALITY;
      break;
    }
    break;
  }
  return PHOTON_PLACE_TYPE_UNKNOWN;
}

/* =========================================================================
 *  Role-free search strings
 * ========================================================================= */

/**
 * @brief Remember @p text as something one might type, unless it is already there.
 *
 *  The dump repeats itself within an entry — `city` and `city:de` usually
 *  carry the same text, and a street may be named like its city.  A linear
 *  scan over at most @ref PHOTON_PLACE_SEARCH_MAX entries settles that; the
 *  list is short enough that nothing cleverer would pay off.
 */
static void search_add(PhotonPlace *p, PhotonString text) {
  if (!text.data || !text.size) return;
  for (uint8_t i = 0; i < p->search_count; ++i) {
    if (p->search[i].size == text.size && memcmp(p->search[i].data, text.data, text.size) == 0) {
      return;
    }
  }
  if (p->search_count >= PHOTON_PLACE_SEARCH_MAX) {
    if (p->search_dropped < UINT8_MAX) ++p->search_dropped;
    return;
  }
  p->search[p->search_count++] = text;
}

/** Take every string value of an object — keys are irrelevant, the text is not. */
static void search_add_object(PhotonPlace *p, const arnm_json_value *object) {
  arnm_json_object_iter iter;
  if (ARNM_SUCCESS != arnm_json_object_iter_init(object, &iter)) return;
  arnm_json_value *value = NULL;
  for (; arnm_json_object_iter_next(&iter, NULL, NULL, &value);) {
    search_add(p, string_of(value));
  }
}

/** Take every string of an array — the `other` list of alternate names. */
static void search_add_array(PhotonPlace *p, const arnm_json_value *array) {
  arnm_json_array_iter iter;
  if (ARNM_SUCCESS != arnm_json_array_iter_init(array, &iter)) return;
  arnm_json_value *item = NULL;
  for (; arnm_json_array_iter_next(&iter, &item);) { search_add(p, string_of(item)); }
}

/* =========================================================================
 *  Address roles that survive into an answer
 * ========================================================================= */

typedef enum AddressRole {
  ADDRESS_ROLE_NONE,
  ADDRESS_ROLE_CITY,
  ADDRESS_ROLE_STREET,
  ADDRESS_ROLE_POSTCODE,
  ADDRESS_ROLE_HOUSE,
  ADDRESS_ROLE_CITY_DEFAULT,  /**< `city:<default>` — outranks the plain reading. */
  ADDRESS_ROLE_STREET_DEFAULT /**< `street:<default>`, likewise. */
} AddressRole;

/** Recognise the four keys an answer needs; everything else stays role-free. */
static AddressRole address_role(const char *key, size_t key_size) {
  switch (key_size) {
  case 4:
    return memcmp(key, "city", 4) == 0 ? ADDRESS_ROLE_CITY : ADDRESS_ROLE_NONE;
  case 6:
    return memcmp(key, "street", 6) == 0 ? ADDRESS_ROLE_STREET : ADDRESS_ROLE_NONE;
  case 8:
    return memcmp(key, "postcode", 8) == 0 ? ADDRESS_ROLE_POSTCODE : ADDRESS_ROLE_NONE;
  case 11:
    return memcmp(key, "housenumber", 11) == 0 ? ADDRESS_ROLE_HOUSE : ADDRESS_ROLE_NONE;
  default:
    return ADDRESS_ROLE_NONE;
  }
}

/**
 * @brief The role of a key, the default reading's two keys among them.
 *
 *  The dump writes `city`, `city:de`, `city:pt-BR` — the same field in as many
 *  readings as it knows, and only one of those readings fills the answer.  That
 *  one is recognised here by its whole name, against a key built once when the
 *  build read its language list.  Every other localized key falls out as
 *  @ref ADDRESS_ROLE_NONE and is looked at again further down, where the colon
 *  is searched for — but only for an entry that becomes a document at all.
 *  Four fifths of a planet dump are house numbers, which do not, and for those
 *  this is the last anyone looks at the key.
 */
static AddressRole address_role_of(
    const PhotonLanguages *languages, const char *key, size_t key_size
) {
  AddressRole role = address_role(key, key_size);
  if (role != ADDRESS_ROLE_NONE || !languages || !languages->count) return role;
  if (key_is(key, key_size, languages->city_key, languages->city_key_size)) {
    return ADDRESS_ROLE_CITY_DEFAULT;
  }
  if (key_is(key, key_size, languages->street_key, languages->street_key_size)) {
    return ADDRESS_ROLE_STREET_DEFAULT;
  }
  return ADDRESS_ROLE_NONE;
}

/* =========================================================================
 *  The keys an entry itself carries
 * ========================================================================= */

/** The members of one content entry this build reads; everything else is passed over. */
typedef enum EntryField {
  ENTRY_FIELD_NONE,
  ENTRY_FIELD_NAME,
  ENTRY_FIELD_STREET,
  ENTRY_FIELD_ADDRESS,
  ENTRY_FIELD_POSTCODE,
  ENTRY_FIELD_CENTROID,
  ENTRY_FIELD_IMPORTANCE,
  ENTRY_FIELD_HOUSENUMBER,
  ENTRY_FIELD_ADDRESS_TYPE,
  ENTRY_FIELD_COUNTRY_CODE
} EntryField;

/**
 * @brief Recognise a key of the entry itself — the same shape as address_role().
 *
 *  An entry carries a handful of keys this build reads and a handful it does
 *  not (`osm_id`, `osm_key`, `extent`, …).  Asking the object for each of the
 *  first kind by name would walk it once per question; the walk below asks
 *  every key what it is instead, and a key answers in a switch on its length
 *  and at most one memcmp.  Nine questions become one pass.
 */
static EntryField entry_field(const char *key, size_t key_size) {
  switch (key_size) {
  case 4:
    return memcmp(key, "name", 4) == 0 ? ENTRY_FIELD_NAME : ENTRY_FIELD_NONE;
  case 6:
    return memcmp(key, "street", 6) == 0 ? ENTRY_FIELD_STREET : ENTRY_FIELD_NONE;
  case 7:
    return memcmp(key, "address", 7) == 0 ? ENTRY_FIELD_ADDRESS : ENTRY_FIELD_NONE;
  case 8:
    switch (key[0]) {
    case 'p':
      return memcmp(key, "postcode", 8) == 0 ? ENTRY_FIELD_POSTCODE : ENTRY_FIELD_NONE;
    case 'c':
      return memcmp(key, "centroid", 8) == 0 ? ENTRY_FIELD_CENTROID : ENTRY_FIELD_NONE;
    default:
      return ENTRY_FIELD_NONE;
    }
  case 10:
    return memcmp(key, "importance", 10) == 0 ? ENTRY_FIELD_IMPORTANCE : ENTRY_FIELD_NONE;
  case 11:
    return memcmp(key, "housenumber", 11) == 0 ? ENTRY_FIELD_HOUSENUMBER : ENTRY_FIELD_NONE;
  case 12:
    switch (key[0]) {
    case 'a':
      return memcmp(key, "address_type", 12) == 0 ? ENTRY_FIELD_ADDRESS_TYPE : ENTRY_FIELD_NONE;
    case 'c':
      return memcmp(key, "country_code", 12) == 0 ? ENTRY_FIELD_COUNTRY_CODE : ENTRY_FIELD_NONE;
    default:
      return ENTRY_FIELD_NONE;
    }
  default:
    return ENTRY_FIELD_NONE;
  }
}

/**
 * @brief One coordinate of a centroid, however the dump chose to write it.
 *
 *  JSON draws no line between an integer and a fraction, and a serializer is
 *  free to write a whole degree without one — `13` where `13.0` was meant.
 *  Both are the same number and both are read as one; only a value that is no
 *  number at all answers 0.
 *
 *  This mattered: the parser used to take the fractional form alone, so an
 *  entry sitting on a whole degree was silently moved to the equator or the
 *  prime meridian.  Nothing in a dump announces which form it uses, and the
 *  entries this hit are the ones nobody would think to check.
 */
static double coordinate_at(const arnm_json_value *centroid, uint32_t index) {
  arnm_json_value *element = NULL;
  if (ARNM_SUCCESS != arnm_json_array_get(centroid, index, &element)) return 0.0;
  double degrees = 0.0;
  return ARNM_SUCCESS == arnm_json_read_double(element, &degrees) ? degrees : 0.0;
}

/** How extract_place() ended: taken, passed over, or a dump that cannot be read. */
typedef enum ResultType {
  RESULT_SUCCESS,            /**< The entry is filled in and worth indexing. */
  RESULT_SKIP,               /**< Nothing wrong with it, nothing to index either. */
  RESULT_ERROR_UNKNOWN_TYPE, /**< An `address_type` this build does not name. */
  RESULT_ERROR_MISSING_TYPE, /**< No `address_type` at all. */
} ResultType;

/* =========================================================================
 *  Extract one content entry into a PhotonPlace
 * ========================================================================= */

/**
 * @brief Read one content entry into @p p — answer fields by role, the rest as text.
 *
 *  Every string is borrowed from the document, so @p p is only as alive as the
 *  parse around it.  The entry is walked once: a scalar the answer needs is
 *  filed as it goes by, and the three containers below it — `name`, `address`,
 *  `centroid` — are only remembered, because what to do with them depends on
 *  fields the walk may not have reached yet.  A JSON object promises no order,
 *  so nothing may be decided before the last key has been seen.
 *
 *  @param[in]  entry  One object out of the `content` array.
 *  @param[out] p      Zeroed first, then filled; untouched fields stay NULL.
 *  @return What became of it; see @ref ResultType.
 */
static ResultType extract_place(
    const arnm_json_value *entry, const PhotonLanguages *languages, PhotonPlace *p
) {
  photon_place_reset(p);

  PhotonString address_type = {NULL, 0};
  PhotonString entry_street = {NULL, 0};
  const arnm_json_value *names = NULL;
  const arnm_json_value *address = NULL;
  const arnm_json_value *centroid = NULL;

  /* --- one pass over the entry's own keys --- */
  arnm_json_object_iter entry_iter;
  if (ARNM_SUCCESS != arnm_json_object_iter_init(entry, &entry_iter)) {
    return RESULT_ERROR_MISSING_TYPE;
  }
  const char *key = NULL;
  uint32_t key_size = 0;
  arnm_json_value *value = NULL;
  for (; arnm_json_object_iter_next(&entry_iter, &key, &key_size, &value);) {
    switch (entry_field(key, key_size)) {
    case ENTRY_FIELD_ADDRESS_TYPE:
      /* string_of() hands back the length beside the bytes, which is exactly what
         the exact match below needs and costs nothing extra to ask for. */
      address_type = string_of(value);
      break;
    case ENTRY_FIELD_COUNTRY_CODE:
      p->country_code = string_of(value).data;
      break;
    case ENTRY_FIELD_POSTCODE:
      p->postcode = string_of(value);
      break;
    case ENTRY_FIELD_HOUSENUMBER:
      p->house = string_of(value);
      break;
    case ENTRY_FIELD_IMPORTANCE: {
      double importance = 0.0;
      if (ARNM_SUCCESS == arnm_json_read_double(value, &importance)) p->importance = importance;
      break;
    }
    case ENTRY_FIELD_STREET:
      /* the address block outranks this one, and it has not been read yet */
      entry_street = string_of(value);
      break;
    case ENTRY_FIELD_NAME:
      names = value;
      break;
    case ENTRY_FIELD_ADDRESS:
      address = value;
      break;
    case ENTRY_FIELD_CENTROID:
      centroid = value;
      break;
    case ENTRY_FIELD_NONE:
      break;
    }
  }

  p->type = address_type.data;
  if (!p->type) { return RESULT_ERROR_MISSING_TYPE; }
  p->typeEnum = detectTypeEnum(address_type.data, address_type.size);
  if (PHOTON_PLACE_TYPE_UNKNOWN == p->typeEnum) { return RESULT_ERROR_UNKNOWN_TYPE; }

  if (names) place_names(names, languages, p);

  /* houses are payload of their street — their repeated parent text belongs
     to the street's document, not into the term stream a fifth time.  The
     number itself is read before this decision, because it decides it. */
  const int indexed = p->typeEnum != PHOTON_PLACE_TYPE_HOUSE && !p->house.data;

  /* --- one pass over the address block: roles for the answer, text for the index --- */
  arnm_json_object_iter address_iter;
  if (address && ARNM_SUCCESS == arnm_json_object_iter_init(address, &address_iter)) {
    for (; arnm_json_object_iter_next(&address_iter, &key, &key_size, &value);) {
      /* The key decides before the value is looked at.  A house entry keeps
         nothing but the four answer fields — its parent text belongs to its
         street's document — so a key with no role is let go here, and for two
         of every three entries of a planet dump that is the whole of the work
         this loop does. */
      const AddressRole role = address_role_of(languages, key, key_size);
      if (ADDRESS_ROLE_NONE == role && !indexed) continue;

      PhotonString text = string_of(value);
      if (!text.data) {
        /* the one member that is no text of its own: "other", a list of
           alternate names, which carries no role and enters the index whole */
        if (indexed && ARNM_JSON_TYPE_ARRAY == arnm_json_value_type(value)) {
          search_add_array(p, value);
        }
        continue;
      }

      switch (role) {
      case ADDRESS_ROLE_CITY:
        if (!p->city.data) p->city = text;
        break;
      case ADDRESS_ROLE_CITY_DEFAULT:
        p->city = text; /* the reading an answer shows outranks the plain one */
        break;
      case ADDRESS_ROLE_STREET:
        if (!p->street.data) p->street = text;
        break;
      case ADDRESS_ROLE_STREET_DEFAULT:
        /* A street keeps one reading only: it is the key a house finds its
           street by, and two spellings of the same street would be two
           streets. */
        p->street = text;
        break;
      case ADDRESS_ROLE_POSTCODE:
        if (!p->postcode.data) p->postcode = text;
        break;
      case ADDRESS_ROLE_HOUSE:
        p->house = text;
        break;
      default:
        break;
      }
      /* Everything below serves the term stream and the readings beside the
         default one, and a house entry has neither — its parent text belongs
         to its street's document.  So this is where a house number's key is
         let go of, before anyone looks for a colon in it. */
      if (!indexed) continue;

      /* Parent text enters the dictionary through the parent's own entry —
         "Brandenburg" is a state document of its own. Carrying every language
         variant of every ancestor on every child would multiply the term
         stream thirtyfold and add not a single word, so only the readings this
         build named come along. The unlocalized one always does, as a safety
         net for parents the dump never lists. And every language asked for
         does, because those are the forms an answer shows: a street in Prague
         comes back with "Prague" in its city field where the caller asked for
         English, and a query repeating what it just read must reach the same
         street again — the unlocalized "Praha" alone would leave it unfindable
         by the name it displays. At home the readings are the same text and
         search_add() keeps one of them; abroad each costs a single term per
         level, which is the price of the languages the build asked for. */
      const char *colon = memchr(key, ':', key_size);
      if (!colon) {
        search_add(p, text); /* the unlocalized reading always comes along */
        continue;
      }
      const char *tag = colon + 1;
      size_t tag_size = key_size - (size_t)(colon - key) - 1;
      if (!languages || !languages_hold_word(languages, tag_word(tag, tag_size))) continue;
      search_add(p, text);

      /* A town in a language this build keeps is a reading of its own; the
         default one is already in the field above and needs none. */
      if (role == ADDRESS_ROLE_NONE &&
          address_role(key, (size_t)(colon - key)) == ADDRESS_ROLE_CITY) {
        PhotonVariant *variant = variant_for(p, tag, tag_size);
        if (variant) variant->city = text;
      }
    }
  }

  if (indexed) {
    if (names) search_add_object(p, names);
    /* many entries carry their postcode beside the address block, not inside
       it — and a postcode is one of the strongest filters a query can bring */
    search_add(p, p->postcode);
    if (p->house.data) {
      /* a number that slipped in through the address block adds nothing */
      for (uint8_t i = 0; i < p->search_count; ++i) {
        if (p->search[i].size == p->house.size &&
            memcmp(p->search[i].data, p->house.data, p->house.size) == 0) {
          p->search[i] = p->search[--p->search_count];
          break;
        }
      }
    }
  }

  /* --- some fields sit beside the address block, not inside it: the street
         does, and the address block's reading of it comes first --- */
  if (!p->street.data) p->street = entry_street;

  /* A house number makes an address, whatever the hierarchy calls the entry —
     the dump files a holiday camp with a number under "other" and a shipwreck
     without one under "house".  What carries no number and belongs to no level
     of the address hierarchy is a pond or a cycleway, and stays outside. */
  if (PHOTON_PLACE_TYPE_OTHER == p->typeEnum && !p->house.data) { return RESULT_SKIP; }

  /* --- the entry's own name also fills the role it stands for --- */
  switch (p->typeEnum) {
  case PHOTON_PLACE_TYPE_HOUSE:
    /* A house without a number is not an address but a place with a name — a
       shipwreck, a shop, a monument.  It keeps its name and stays out of the
       house numbers; the address_type only says how deep in the hierarchy it
       sits, not what kind of thing it is. */
    break;
  case PHOTON_PLACE_TYPE_STREET:
    p->street = p->own_name;
    break;
  case PHOTON_PLACE_TYPE_CITY:
  case PHOTON_PLACE_TYPE_STATE_COUNTY_CITY:
  case PHOTON_PLACE_TYPE_INDEPENDENT_CITY:
    p->city = p->own_name;
    /* And it fills that role in every reading it has one for.  A town is its
       own town: asked in English, Praha must answer "Prague" in both fields,
       not "Prague" under a city called "Prag". */
    for (uint8_t i = 0; i < p->variant_count; ++i) {
      if (!p->variants[i].city.data) p->variants[i].city = p->variants[i].name;
    }
    break;
  default:
    break;
  }

  /* --- centroid --- */
  if (centroid && arnm_json_array_size(centroid) == 2) {
    p->has_point = 1;
    p->lon_e7 = (int32_t)round(coordinate_at(centroid, 0) * 1.0e7);
    p->lat_e7 = (int32_t)round(coordinate_at(centroid, 1) * 1.0e7);
  }

  return RESULT_SUCCESS;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */
void photon_place_reset(PhotonPlace *place) {
  memset(place, 0, offsetof(PhotonPlace, search));
}

bool photon_place_has_point(const PhotonPlace *place) {
  return place->has_point;
}

/* =========================================================================
 *  The ground one line is parsed on
 * ========================================================================= */

/** The arena every document of this thread is built in; grown, never shared. */
static __thread arnm parse_arena;
/** Bytes @c parse_arena holds, or 0 while it holds none. */
static __thread uint32_t parse_arena_capacity = 0;
/** The reader of this thread, bound to @c parse_arena for as long as it lives. */
static __thread arnm_json_reader parse_reader;
/** Whether @c parse_reader has seen arnm_json_reader_init(). */
static __thread bool parse_reader_ready = false;

/**
 * @brief Arena bytes a line of @p length is likely to need, before a retry has to think.
 *
 *  A JSON text holds at most one value per two bytes, a value costs sixteen
 *  bytes in the tree, the tree is built by growing a buffer by half again, and
 *  the strings are unescaped into a copy of the text beside it.  Thirteen times
 *  the line plus a margin covers all of that for every document, which is why
 *  the arena is sized from the longest line seen rather than per line: it grows
 *  a handful of times at the start of a dump and then never again.
 *
 *  It only has to be right often enough to make growing rare.  A parse that
 *  finds the arena short refuses instead of overrunning it, and json_parse_line()
 *  then widens the ground and reads the line again.
 */
static uint32_t parse_arena_size_for(size_t length) {
  const size_t wanted = length * 13u + 512u;
  return wanted > (size_t)ARNM_MAX_ALLOC_SIZE ? ARNM_MAX_ALLOC_SIZE : (uint32_t)wanted;
}

/** Take the arena back and claim @p capacity bytes instead; the reader keeps its address. */
static void parse_arena_reserve(uint32_t capacity) {
  if (ARNM_SUCCESS != arnm_reinit_arena(&parse_arena, capacity)) {
    fatal(ERROR_MEMORY, "Failed to reserve %u bytes for JSON parsing.", capacity);
  }
  parse_arena_capacity = capacity;
}

/**
 * @brief Let the document go and put the arena back where it started.
 *
 *  The release hands the document's blocks back and the reset settles what order
 *  they came in — an arena only takes its tail, so a block freed before the one
 *  above it would stay reserved until something moved the index home.  Together
 *  they leave the next line the whole arena, which is why it never has to grow
 *  for anything but a longer line.
 */
static void parse_arena_release(void) {
  (void)arnm_json_reader_release(&parse_reader);
  arnm_reset(&parse_arena);
}

int json_parse_line(
    const char *line,
    size_t len,
    const PhotonLanguages *languages,
    PhotonPlaceCallback callback,
    void *user_data,
    JsonParseResult *result
) {
  memset(result, 0, sizeof(*result));
  if (len > ARNM_JSON_READER_MAX_INPUT_SIZE) {
    fatal(ERROR_JSON, "A JSON line of %zu bytes is longer than this build can read.", len);
  }

  /* --- this thread's arena and reader, grown to the longest line so far --- */
  const uint32_t wanted = parse_arena_size_for(len);
  if (wanted > parse_arena_capacity) parse_arena_reserve(wanted);
  if (!parse_reader_ready) {
    if (ARNM_SUCCESS !=
        arnm_json_reader_init(&parse_reader, &parse_arena, ARNM_JSON_READ_DEFAULT)) {
      fatal(ERROR_MEMORY, "Failed to prepare the JSON reader.");
    }
    parse_reader_ready = true;
  }

  arnm_result parsed = arnm_json_reader_parse(&parse_reader, line, (uint32_t)len);
  while (ARNM_ERROR_OUT_OF_MEMORY == parsed) {
    /* the estimate was short for this line — widen the ground and read it again */
    (void)arnm_json_reader_release(&parse_reader);
    if (parse_arena_capacity >= ARNM_MAX_ALLOC_SIZE) {
      fatal(ERROR_MEMORY, "A JSON line of %zu bytes outgrew the parser's arena.", len);
    }
    uint32_t grown = parse_arena_capacity > ARNM_MAX_ALLOC_SIZE / 2 ? ARNM_MAX_ALLOC_SIZE
                                                                    : parse_arena_capacity * 2u;
    parse_arena_reserve(grown);
    parsed = arnm_json_reader_parse(&parse_reader, line, (uint32_t)len);
  }
  if (ARNM_SUCCESS != parsed) {
    fatal(
        ERROR_JSON, "Failed to parse JSON (%s), error at pos: %u, length %zu, error: %d",
        arnm_json_reader_error_message(&parse_reader),
        arnm_json_reader_error_position(&parse_reader), len, (int)parsed
    );
  }

  const arnm_json_value *root = arnm_json_reader_root(&parse_reader);
  if (ARNM_JSON_TYPE_OBJECT != arnm_json_value_type(root)) {
    parse_arena_release();
    return 1; /* success, but not a valid object */
  }

  result->is_valid = 1;

  const PhotonString type = string_of(member(root, "type"));
  if (type.size != 5 || memcmp(type.data, "Place", 5) != 0) {
    parse_arena_release();
    return 1;
  }

  result->is_place = 1;

  arnm_json_array_iter content_iter;
  if (ARNM_SUCCESS != arnm_json_array_iter_init(member(root, "content"), &content_iter)) {
    parse_arena_release();
    return 1;
  }

  arnm_json_value *entry = NULL;
  for (; arnm_json_array_iter_next(&content_iter, &entry);) {
    if (ARNM_JSON_TYPE_OBJECT != arnm_json_value_type(entry)) continue;

    PhotonPlace place;
    ResultType extractResult = extract_place(entry, languages, &place);
    if (extractResult == RESULT_ERROR_UNKNOWN_TYPE) {
      fatal(ERROR_JSON, "unknown type: %.*s", (int)len, line);
    } else if (extractResult == RESULT_ERROR_MISSING_TYPE) {
      fatal(ERROR_JSON, "missing type: %.*s", (int)len, line);
    } else if (extractResult == RESULT_SKIP) {
    } else if (extractResult == RESULT_SUCCESS) {
      if (callback(&place, user_data)) { printf("callback error with: %.*s\n\n", (int)len, line); }
      ++result->entry_count;
    }
  }

  parse_arena_release();
  return 1;
}

/* =========================================================================
 *  Debug serialisation — PhotonPlace → JSON string
 * ========================================================================= */

/** Add a field only when the entry carried it, so absent stays absent in the output. */
static void add_string_field(arnm_json_writer *writer, const char *key, PhotonString text) {
  if (text.data) arnm_json_writer_add_string_length(writer, key, text.data, (uint32_t)text.size);
}

char *photon_place_to_json(const PhotonPlace *place) {
  static __thread char buf[4096];
  if (!place) {
    snprintf(buf, sizeof(buf), "null");
    return buf;
  }

  /* The host allocator throughout: this is a debugger's call, not a hot path,
     and a writer of its own owes the parser's arena nothing. */
  arnm_json_writer writer;
  if (ARNM_SUCCESS != arnm_json_writer_init(&writer, NULL, ARNM_JSON_WRITE_DEFAULT)) {
    snprintf(buf, sizeof(buf), "{\"error\":\"failed to create JSON document\"}");
    return buf;
  }

  if (place->type) arnm_json_writer_add_string(&writer, "type", place->type);
  add_string_field(&writer, "name", place->own_name);
  add_string_field(&writer, "street", place->street);
  add_string_field(&writer, "house", place->house);
  add_string_field(&writer, "postcode", place->postcode);
  add_string_field(&writer, "city", place->city);
  if (place->country_code) {
    arnm_json_writer_add_string(&writer, "country_code", place->country_code);
  }

  arnm_json_writer_add_double(&writer, "importance", place->importance);
  arnm_json_writer_add_int64(&writer, "lon_e7", place->lon_e7);
  arnm_json_writer_add_int64(&writer, "lat_e7", place->lat_e7);
  arnm_json_writer_add_bool(&writer, "has_point", place->has_point != 0);

  arnm_json_writer_open_array(&writer, "search");
  for (uint8_t i = 0; i < place->search_count; ++i) {
    arnm_json_writer_add_string_length(
        &writer, NULL, place->search[i].data, (uint32_t)place->search[i].size
    );
  }
  arnm_json_writer_close(&writer);

  if (place->variant_count) {
    arnm_json_writer_open_object(&writer, "variants");
    for (uint8_t i = 0; i < place->variant_count; ++i) {
      const PhotonVariant *variant = &place->variants[i];
      arnm_json_writer_open_object(&writer, variant->tag);
      add_string_field(&writer, "name", variant->name);
      add_string_field(&writer, "city", variant->city);
      arnm_json_writer_close(&writer);
    }
    arnm_json_writer_close(&writer);
  }

  arnm_memory_block text = {NULL, 0};
  uint32_t length = 0;
  const arnm_result written = arnm_json_writer_write(&writer, NULL, &text, &length);
  if (ARNM_SUCCESS == written && length < sizeof(buf)) {
    memcpy(buf, text.data, length);
    buf[length] = '\0';
  } else {
    snprintf(
        buf, sizeof(buf), "{\"error\":\"serialization failed\", \"len\":\"%u\", \"result\":%d}",
        length, (int)written
    );
  }
  (void)arnm_memory_block_free(&text, NULL);
  (void)arnm_json_writer_release(&writer);

  return buf;
}

/** @endcond */
