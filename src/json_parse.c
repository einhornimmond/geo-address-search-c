/** @cond INTERNAL */

#include "json_parse.h"
#include "error.h"

#include <math.h>
#include <string.h>

#include <yyjson.h>

/* =========================================================================
 *  Field helpers
 * ========================================================================= */

static PhotonString string_of(yyjson_val *value) {
  PhotonString result = {NULL, 0};
  if (yyjson_is_str(value)) {
    result.data = yyjson_get_str(value);
    result.size = yyjson_get_len(value);
  }
  return result;
}

static PhotonString localized(
    yyjson_val *object, const char *german_key, const char *fallback_key
) {
  yyjson_val *value = yyjson_obj_get(object, german_key);
  if (fallback_key && !yyjson_is_str(value)) value = yyjson_obj_get(object, fallback_key);
  return string_of(value);
}

static const char *plain(yyjson_val *object, const char *key) {
  yyjson_val *value = yyjson_obj_get(object, key);
  return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static PhotonString place_name(yyjson_val *place) {
  yyjson_val *names = yyjson_obj_get(place, "name");
  PhotonString empty = {NULL, 0};
  return yyjson_is_obj(names) ? localized(names, "name:de", "name") : empty;
}

static PhotonPlaceType detectTypeEnum(const char *type) {
  if (type) {
    if (type[0] == 'h')
      return PHOTON_PLACE_TYPE_HOUSE;
    else if (type[0] == 's') {
      if (type[2] == 'r') return PHOTON_PLACE_TYPE_STREET;
      if (type[2] == 'a') return PHOTON_PLACE_TYPE_STATE;
    } else if (type[0] == 'c') {
      if (type[1] == 'i') {
        return PHOTON_PLACE_TYPE_CITY;
      } else if (type[5] == 'y') {
        return PHOTON_PLACE_TYPE_COUNTY;
      } else if (type[5] == 'r') {
        return PHOTON_PLACE_TYPE_COUNTRY;
      }
    } else if (type[0] == 'o') {
      return PHOTON_PLACE_TYPE_OTHER;
    } else if (type[0] == 'd') {
      return PHOTON_PLACE_TYPE_DISTRICT;
    } else if (type[0] == 'l') {
      return PHOTON_PLACE_TYPE_LOCALITY;
    }
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
  ADDRESS_ROLE_CITY_DE, /**< localized variant, preferred over the plain one */
  ADDRESS_ROLE_STREET_DE
} AddressRole;

/** Recognise the four keys an answer needs; everything else stays role-free. */
static AddressRole address_role(const char *key, size_t key_size) {
  switch (key_size) {
  case 4:
    return memcmp(key, "city", 4) == 0 ? ADDRESS_ROLE_CITY : ADDRESS_ROLE_NONE;
  case 6:
    return memcmp(key, "street", 6) == 0 ? ADDRESS_ROLE_STREET : ADDRESS_ROLE_NONE;
  case 7:
    return memcmp(key, "city:de", 7) == 0 ? ADDRESS_ROLE_CITY_DE : ADDRESS_ROLE_NONE;
  case 8:
    return memcmp(key, "postcode", 8) == 0 ? ADDRESS_ROLE_POSTCODE : ADDRESS_ROLE_NONE;
  case 9:
    return memcmp(key, "street:de", 9) == 0 ? ADDRESS_ROLE_STREET_DE : ADDRESS_ROLE_NONE;
  case 11:
    return memcmp(key, "housenumber", 11) == 0 ? ADDRESS_ROLE_HOUSE : ADDRESS_ROLE_NONE;
  default:
    return ADDRESS_ROLE_NONE;
  }
}

/**
 * @brief Is this the German reading of an address key — `city:de`, `state:de`, and their kin?
 *
 *  A suffix test, nothing more: the key must end in `:de` and carry something
 *  before it.  Longer tags such as `city:de-formal` are left to the general
 *  rule, which passes them over.
 */
static bool key_is_german(const char *key, size_t key_size) {
  return key_size > 3 && memcmp(key + key_size - 3, ":de", 3) == 0;
}

typedef enum ResultType {
  RESULT_SUCCESS,
  RESULT_SKIP,
  RESULT_ERROR_UNKNOWN_TYPE,
  RESULT_ERROR_MISSING_TYPE,
} ResultType;

/* =========================================================================
 *  Extract one content entry into a PhotonPlace
 * ========================================================================= */
static ResultType extract_place(yyjson_val *entry, PhotonPlace *p) {
  memset(p, 0, sizeof(*p));

  p->type = plain(entry, "address_type");
  if (!p->type) { return RESULT_ERROR_MISSING_TYPE; }
  p->typeEnum = detectTypeEnum(p->type);
  if (PHOTON_PLACE_TYPE_UNKNOWN == p->typeEnum) { return RESULT_ERROR_UNKNOWN_TYPE; }

  p->country_code = plain(entry, "country_code");
  p->own_name = place_name(entry);
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

      switch (address_role(key_text, key_size)) {
      case ADDRESS_ROLE_CITY:
        if (!p->city.data) p->city = text;
        break;
      case ADDRESS_ROLE_CITY_DE:
        p->city = text;
        break;
      case ADDRESS_ROLE_STREET:
        if (!p->street.data) p->street = text;
        break;
      case ADDRESS_ROLE_STREET_DE:
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
      /* Parent text enters the dictionary through the parent's own entry —
         "Brandenburg" is a state document of its own. Carrying every language
         variant of every ancestor on every child would multiply the term
         stream thirtyfold and add not a single word. Two readings stay. The
         unlocalized one, as a safety net for parents the dump never lists.
         And the German one, because that is the form the answer shows: a
         street in Prague comes back with "Prag" in its city field, and a query
         repeating what it just read must reach the same street again — the
         unlocalized "Praha" alone would leave it unfindable by the name it
         displays. At home both readings are the same text and search_add()
         keeps one of them; abroad it costs a single term per level. */
      if (indexed && (key_is_german(key_text, key_size) || !memchr(key_text, ':', key_size))) {
        search_add(p, text);
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
bool photon_place_has_point(const PhotonPlace *place) {
  return place->has_point;
}

int json_parse_line(
    const char *line,
    size_t len,
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
    ResultType extractResult = extract_place(entry, &place);
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
