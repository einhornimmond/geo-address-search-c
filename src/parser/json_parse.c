/** @cond INTERNAL */

#include "parser/json_parse.h"
#include "foundation/error.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <yyjson.h>

/* =========================================================================
 *  Field helpers
 * ========================================================================= */

/** Borrow a string value with its length; a non-string yields the empty borrow. */
static PhotonString string_of(yyjson_val *value) {
  PhotonString result = {NULL, 0};
  if (yyjson_is_str(value)) {
    result.data = yyjson_get_str(value);
    result.size = yyjson_get_len(value);
  }
  return result;
}

/**
 * @brief Prefer the German reading of a field, fall back to the neutral one.
 *
 *  The dump carries a variant per language, and this build answers in German
 *  where the data offers it.  A @p fallback_key of NULL means there is nothing
 *  to fall back to and the German reading is the only one wanted.
 */
static PhotonString localized(
    yyjson_val *object, const char *german_key, const char *fallback_key
) {
  yyjson_val *value = yyjson_obj_get(object, german_key);
  if (fallback_key && !yyjson_is_str(value)) value = yyjson_obj_get(object, fallback_key);
  return string_of(value);
}

/** A string field as a bare pointer, for the few values whose length is not needed. */
static const char *plain(yyjson_val *object, const char *key) {
  yyjson_val *value = yyjson_obj_get(object, key);
  return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
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
 *  Two tags are the same exactly when their numbers are.  A tag longer than
 *  seven bytes has no number and yields 0, which matches nothing: the parser
 *  refuses such a tag on the way in, so nothing that reaches here has one.
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
 *  The `name` object holds one key per language the dump knows.  One of them is
 *  what an answer shows unless the caller asks otherwise, and the others are
 *  kept only where this build named them — a planet's worth of translations of
 *  every village is what the variant list is small for.
 */
static void place_names(yyjson_val *place, const PhotonLanguages *languages, PhotonPlace *p) {
  yyjson_val *names = yyjson_obj_get(place, "name");
  if (!yyjson_is_obj(names)) return;

  if (languages && languages->count) {
    p->own_name = localized(names, languages->name_key, "name");
  } else {
    p->own_name = string_of(yyjson_obj_get(names, "name"));
  }

  /* nothing beyond the default was asked for */
  if (!languages || (!languages->every && languages->count < 2)) return;

  yyjson_obj_iter iter;
  yyjson_obj_iter_init(names, &iter);
  yyjson_val *key;
  while ((key = yyjson_obj_iter_next(&iter))) {
    const char *key_text = yyjson_get_str(key);
    size_t key_size = yyjson_get_len(key);
    if (key_size <= 5 || memcmp(key_text, "name:", 5) != 0) continue;
    const char *tag = key_text + 5;
    size_t tag_size = key_size - 5;
    if (tag_is_default(languages, tag, tag_size)) continue; /* already the default reading */
    if (!photon_languages_hold(languages, tag, tag_size)) continue;
    PhotonString text = string_of(yyjson_obj_iter_get_val(key));
    if (!text.data) continue;
    PhotonVariant *variant = variant_for(p, tag, tag_size);
    if (variant) variant->name = text;
  }
}

/**
 * @brief Fold the `address_type` string into a number — the whole string, or nothing.
 *
 *  The length comes in because it is already there: yyjson keeps it beside every
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
static void search_add_object(PhotonPlace *p, yyjson_val *object) {
  if (!yyjson_is_obj(object)) return;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(object, &iter);
  yyjson_val *key;
  while ((key = yyjson_obj_iter_next(&iter))) {
    search_add(p, string_of(yyjson_obj_iter_get_val(key)));
  }
}

/** Take every string of an array — the `other` list of alternate names. */
static void search_add_array(PhotonPlace *p, yyjson_val *array) {
  if (!yyjson_is_arr(array)) return;
  size_t index, max;
  yyjson_val *item;
  yyjson_arr_foreach(array, index, max, item) {
    search_add(p, string_of(item));
  }
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
 *  parse around it.  The entry is walked once: a key that names an answer field
 *  is filed by its role, and everything else joins the role-free search list.
 *
 *  @param[in]  entry  One object out of the `content` array.
 *  @param[out] p      Zeroed first, then filled; untouched fields stay NULL.
 *  @return What became of it; see @ref ResultType.
 */
static ResultType extract_place(
    yyjson_val *entry, const PhotonLanguages *languages, PhotonPlace *p
) {
  photon_place_reset(p);

  /* string_of() hands back the length beside the bytes, which is exactly what
     the exact match below needs and costs nothing extra to ask for. */
  const PhotonString address_type = string_of(yyjson_obj_get(entry, "address_type"));
  p->type = address_type.data;
  if (!p->type) { return RESULT_ERROR_MISSING_TYPE; }
  p->typeEnum = detectTypeEnum(address_type.data, address_type.size);
  if (PHOTON_PLACE_TYPE_UNKNOWN == p->typeEnum) { return RESULT_ERROR_UNKNOWN_TYPE; }

  p->country_code = plain(entry, "country_code");
  place_names(entry, languages, p);
  p->postcode = string_of(yyjson_obj_get(entry, "postcode"));

  yyjson_val *importance = yyjson_obj_get(entry, "importance");
  if (yyjson_is_num(importance)) { p->importance = yyjson_get_num(importance); }

  /* houses are payload of their street — their repeated parent text belongs
     to the street's document, not into the term stream a fifth time.  The
     number itself is read before this decision, because it decides it. */
  p->house = string_of(yyjson_obj_get(entry, "housenumber"));
  const int indexed = p->typeEnum != PHOTON_PLACE_TYPE_HOUSE && !p->house.data;

  /* --- one pass over the address block: roles for the answer, text for the index --- */
  yyjson_val *address = yyjson_obj_get(entry, "address");
  if (yyjson_is_obj(address)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(address, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *value = yyjson_obj_iter_get_val(key);
      if (yyjson_is_arr(value)) { /* "other": alternate names, no role */
        if (indexed) search_add_array(p, value);
        continue;
      }
      PhotonString text = string_of(value);
      if (!text.data) continue;
      const char *key_text = yyjson_get_str(key);
      size_t key_size = yyjson_get_len(key);

      AddressRole role = address_role_of(languages, key_text, key_size);
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
      const char *colon = memchr(key_text, ':', key_size);
      if (!colon) {
        search_add(p, text); /* the unlocalized reading always comes along */
        continue;
      }
      const char *tag = colon + 1;
      size_t tag_size = key_size - (size_t)(colon - key_text) - 1;
      if (!languages || !languages_hold_word(languages, tag_word(tag, tag_size))) continue;
      search_add(p, text);

      /* A town in a language this build keeps is a reading of its own; the
         default one is already in the field above and needs none. */
      if (role == ADDRESS_ROLE_NONE &&
          address_role(key_text, (size_t)(colon - key_text)) == ADDRESS_ROLE_CITY) {
        PhotonVariant *variant = variant_for(p, tag, tag_size);
        if (variant) variant->city = text;
      }
    }
  }

  if (indexed) {
    search_add_object(p, yyjson_obj_get(entry, "name"));
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

  /* --- some fields sit beside the address block, not inside it: the postal
         code does, and so does the house number --- */
  if (!p->house.data) p->house = string_of(yyjson_obj_get(entry, "housenumber"));
  if (!p->street.data) p->street = string_of(yyjson_obj_get(entry, "street"));

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
  yyjson_val *centroid = yyjson_obj_get(entry, "centroid");
  if (yyjson_is_arr(centroid) && yyjson_arr_size(centroid) == 2) {
    double lon = yyjson_get_real(yyjson_arr_get(centroid, 0));
    double lat = yyjson_get_real(yyjson_arr_get(centroid, 1));
    p->has_point = 1;
    p->lon_e7 = (int32_t)round(lon * 1.0e7);
    p->lat_e7 = (int32_t)round(lat * 1.0e7);
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

int json_parse_line(
    const char *line,
    size_t len,
    const PhotonLanguages *languages,
    PhotonPlaceCallback callback,
    void *user_data,
    JsonParseResult *result
) {
  memset(result, 0, sizeof(*result));

  /* --- thread-local yyjson buffer --- */
  size_t buf_size = yyjson_read_max_memory_usage(len, 0);
  static __thread uint8_t *alc_buf = NULL;
  static __thread size_t alc_buf_size = 0;

  if (alc_buf_size < buf_size) {
    free(alc_buf);
    alc_buf = malloc(buf_size);
    if (!alc_buf) {
      fatal(ERROR_MEMORY, "Failed to allocate %zu bytes for JSON parsing.", buf_size);
    }
    alc_buf_size = buf_size;
  }

  yyjson_alc alc;
  yyjson_alc_pool_init(&alc, alc_buf, alc_buf_size);

  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_opts((char *)line, len, 0, &alc, &err);
  if (!doc) {
    fatal(
        ERROR_JSON, "Failed to parse JSON (%s), error at pos: %zu, length %zu, error: %d", err.msg,
        err.pos, len, err.code
    );
    return 0;
  }

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return 1; /* success, but not a valid object */
  }

  result->is_valid = 1;

  yyjson_val *type = yyjson_obj_get(root, "type");
  if (!yyjson_is_str(type) || strcmp(yyjson_get_str(type), "Place") != 0) {
    yyjson_doc_free(doc);
    return 1;
  }

  result->is_place = 1;

  yyjson_val *content = yyjson_obj_get(root, "content");
  if (!yyjson_is_arr(content)) {
    yyjson_doc_free(doc);
    return 1;
  }

  size_t index, max;
  yyjson_val *entry;
  yyjson_arr_foreach(content, index, max, entry) {
    if (!yyjson_is_obj(entry)) continue;

    PhotonPlace place;
    ResultType extractResult = extract_place(entry, languages, &place);
    if (extractResult == RESULT_ERROR_UNKNOWN_TYPE) {
      char *buf = (char *)calloc(1, len + 1);
      memcpy(buf, line, len);
      fatal(ERROR_JSON, "unknown type: %s", buf);
    } else if (extractResult == RESULT_ERROR_MISSING_TYPE) {
      char *buf = (char *)calloc(1, len + 1);
      memcpy(buf, line, len);
      fatal(ERROR_JSON, "missing type: %s", buf);
    } else if (extractResult == RESULT_SKIP) {
    } else if (extractResult == RESULT_SUCCESS) {
      if (callback(&place, user_data)) {
        char *buf = (char *)calloc(1, len + 1);
        memcpy(buf, line, len);
        printf("callback error with: %s\n\n", buf);
        free(buf);
      }
      ++result->entry_count;
    }
  }

  yyjson_doc_free(doc);
  return 1;
}

/* =========================================================================
 *  Debug serialisation — PhotonPlace → JSON string (via yyjson mutable API)
 * ========================================================================= */

/** Add a field only when the entry carried it, so absent stays absent in the output. */
static void add_string_field(
    yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key, PhotonString text
) {
  if (text.data) yyjson_mut_obj_add_strn(doc, root, key, text.data, text.size);
}

char *photon_place_to_json(const PhotonPlace *place) {
  static __thread char buf[4096];
  if (!place) {
    snprintf(buf, sizeof(buf), "null");
    return buf;
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) {
    snprintf(buf, sizeof(buf), "{\"error\":\"failed to create JSON document\"}");
    return buf;
  }

  yyjson_mut_val *root = yyjson_mut_obj(doc);
  if (!root) {
    yyjson_mut_doc_free(doc);
    snprintf(buf, sizeof(buf), "{\"error\":\"failed to create JSON object\"}");
    return buf;
  }
  yyjson_mut_doc_set_root(doc, root);

  if (place->type) yyjson_mut_obj_add_str(doc, root, "type", place->type);
  add_string_field(doc, root, "name", place->own_name);
  add_string_field(doc, root, "street", place->street);
  add_string_field(doc, root, "house", place->house);
  add_string_field(doc, root, "postcode", place->postcode);
  add_string_field(doc, root, "city", place->city);
  if (place->country_code) yyjson_mut_obj_add_str(doc, root, "country_code", place->country_code);

  yyjson_mut_obj_add_real(doc, root, "importance", place->importance);
  yyjson_mut_obj_add_int(doc, root, "lon_e7", place->lon_e7);
  yyjson_mut_obj_add_int(doc, root, "lat_e7", place->lat_e7);
  yyjson_mut_obj_add_bool(doc, root, "has_point", place->has_point);

  yyjson_mut_val *search = yyjson_mut_arr(doc);
  for (uint8_t i = 0; i < place->search_count; ++i) {
    yyjson_mut_arr_add_strn(doc, search, place->search[i].data, place->search[i].size);
  }
  yyjson_mut_obj_add_val(doc, root, "search", search);

  if (place->variant_count) {
    yyjson_mut_val *variants = yyjson_mut_obj(doc);
    for (uint8_t i = 0; i < place->variant_count; ++i) {
      const PhotonVariant *variant = &place->variants[i];
      yyjson_mut_val *reading = yyjson_mut_obj(doc);
      add_string_field(doc, reading, "name", variant->name);
      add_string_field(doc, reading, "city", variant->city);
      yyjson_mut_obj_add_val(doc, variants, variant->tag, reading);
    }
    yyjson_mut_obj_add_val(doc, root, "variants", variants);
  }

  size_t len = 0;
  const char *json = yyjson_mut_write(doc, 0, &len);
  if (json && len < sizeof(buf)) {
    memcpy(buf, json, len);
    buf[len] = '\0';
  } else {
    snprintf(
        buf, sizeof(buf), "{\"error\":\"serialization failed\", \"len\":\"%zu\"}, json: %s", len,
        json
    );
  }
  free((void *)json);
  yyjson_mut_doc_free(doc);

  return buf;
}

/** @endcond */
