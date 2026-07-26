/** @cond INTERNAL */

#include "json_parse.h"
#include "error.h"
#include "meta_area_allocator.h"

#include <math.h>
#include <string.h>

#include <yyjson.h>

/* =========================================================================
 *  Localisation helpers — moved from storage_stats.c
 * ========================================================================= */

static const char *localized(yyjson_val *object, const char *german_key, const char *fallback_key) {
  yyjson_val *value = yyjson_obj_get(object, german_key);
  if (!yyjson_is_str(value) && strcmp(german_key, fallback_key) != 0)
    value = yyjson_obj_get(object, fallback_key);
  return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static const char *place_name(yyjson_val *place) {
  yyjson_val *names = yyjson_obj_get(place, "name");
  return yyjson_is_obj(names) ? localized(names, "name:de", "name") : NULL;
}

/* =========================================================================
 *  Extract one content entry into a PhotonPlace
 * ========================================================================= */

static void extract_place(yyjson_val *entry, PhotonPlace *p) {
  memset(p, 0, sizeof(*p));

  /* --- address sub-object --- */
  yyjson_val *address = yyjson_obj_get(entry, "address");
  if (!yyjson_is_obj(address)) {
    if (yyjson_is_arr(yyjson_obj_get(entry, "addresslines"))) p->unsupported = 1;
    address = NULL;
  }

  p->type = localized(entry, "address_type", "address_type");
  p->own_name = place_name(entry);
  p->country_code = localized(entry, "country_code", "country_code");

  /* --- single-pass address field extraction --- */
  if (address) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(address, &iter);
    yyjson_val *key, *val;
    while ((key = yyjson_obj_iter_next(&iter))) {
      val = yyjson_obj_iter_get_val(key);
      if (!yyjson_is_str(val)) continue;
      const char *k = yyjson_get_str(key);
      size_t kl = yyjson_get_len(key);
      const char *v = yyjson_get_str(val);

      switch (kl) {
      case 4:
        if (!p->city && k[0] == 'c') p->city = v;
        break;
      case 5:
        if (!p->state && k[0] == 's') p->state = v;
        break;
      case 6:
        if (k[0] == 'c' && !p->county)
          p->county = v;
        else if (k[0] == 's' && !p->street)
          p->street = v;
        break;
      case 7:
        if (k[0] == 'c') {
          if (k[1] == 'o' && !memcmp(k, "country", 7) && !p->country)
            p->country = v;
          else if (k[1] == 'i' && !memcmp(k, "city:de", 7))
            p->city = v;
        }
        break;
      case 8:
        if (k[0] == 'p' && !memcmp(k, "postcode", 8) && !p->postcode) {
          p->postcode = v;
        } else {
          if (!memcmp(k, "state:de", 8)) p->state = v;
        }
        break;
      case 9:
        if (k[0] == 'c' && !memcmp(k, "county:de", 9))
          p->county = v;
        else if (k[0] == 's' && !memcmp(k, "street:de", 9))
          p->street = v;
        break;
      case 10:
        if (!memcmp(k, "country:de", 10)) p->country = v;
        break;
      }
    }
  }

  /* --- resolve type category: use own_name when it is the primary name --- */
  if (p->type) {
    switch (strlen(p->type)) {
    case 8:
      if (p->type[0] == 'p') p->postcode = p->own_name;
      break;
    case 7:
      if (p->type[0] == 'c') p->country = p->own_name;
      break;
    case 5:
      if (p->type[0] == 's') p->state = p->own_name;
      break;
    case 6:
      if (p->type[0] == 'c')
        p->county = p->own_name;
      else if (p->type[0] == 's')
        p->street = p->own_name;
      break;
    case 4:
      if (p->type[0] == 'c') p->city = p->own_name;
      break;
    }
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

  /* --- house: housenumber from own_name or address sub-field --- */
  if (p->type && (strcmp(p->type, "house") == 0 || strcmp(p->type, "house_number") == 0))
    p->house = p->own_name;
  if (!p->house && address) p->house = localized(address, "housenumber", "housenumber");
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

int json_parse_line(
    const char *line,
    size_t len,
    PhotonPlaceCallback callback,
    void *user_data,
    MetaAreaAllocator *alloc,
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
    extract_place(entry, &place);
    callback(&place, user_data);
    ++result->entry_count;
  }

  yyjson_doc_free(doc);
  return 1;
}

/** @endcond */
