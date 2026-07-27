/** @cond INTERNAL */

#include "json_parse.h"
#include "error.h"

#include <math.h>
#include <string.h>

#include <yyjson.h>

/* =========================================================================
 *  Localisation helpers — moved from storage_stats.c
 * ========================================================================= */

static const char *localized(yyjson_val *object, const char *german_key, const char *fallback_key) {
  yyjson_val *value = yyjson_obj_get(object, german_key);
  if (fallback_key && !yyjson_is_str(value)) value = yyjson_obj_get(object, fallback_key);
  return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static const char *place_name(yyjson_val *place) {
  yyjson_val *names = yyjson_obj_get(place, "name");
  return yyjson_is_obj(names) ? localized(names, "name:de", "name") : NULL;
}

static PhotonPlaceType detectTypeEnum(const char* type)
{
  if (type) {
    if (type[0] == 'h') return PHOTON_PLACE_TYPE_HOUSE;
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

  p->type = localized(entry, "address_type", NULL);
  if (!p->type) {
    return RESULT_ERROR_MISSING_TYPE;
  }
  p->typeEnum = detectTypeEnum(p->type);
  p->country_code = localized(entry, "country_code", NULL);
  p->own_name = place_name(entry);
  if (PHOTON_PLACE_TYPE_OTHER == p->typeEnum) {
    return RESULT_SKIP;
  }

  /* --- centroid --- */
  yyjson_val *centroid = yyjson_obj_get(entry, "centroid");
  if (yyjson_is_arr(centroid) && yyjson_arr_size(centroid) == 2) {
    double lon = yyjson_get_real(yyjson_arr_get(centroid, 0));
    double lat = yyjson_get_real(yyjson_arr_get(centroid, 1));
    p->lon_e7 = (int32_t)round(lon * 1.0e7);
    p->lat_e7 = (int32_t)round(lat * 1.0e7);
  }

  if (p->typeEnum == PHOTON_PLACE_TYPE_UNKNOWN) {
    return RESULT_ERROR_UNKNOWN_TYPE;
  }
  return RESULT_SUCCESS;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */
bool photon_place_has_point(const PhotonPlace* place)
{
  return place->lat_e7 && place->lon_e7;
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
      char* buf = (char*)calloc(1, len+1);
      memcpy(buf, line, len);
      fatal(ERROR_JSON, "unknown type: %s", buf);
    } else if (extractResult == RESULT_ERROR_MISSING_TYPE)  {
      char* buf = (char*)calloc(1, len+1);
      memcpy(buf, line, len);
      fatal(ERROR_JSON, "missing type: %s", buf);
    } else if (extractResult == RESULT_SKIP) {
    } else if (extractResult == RESULT_SUCCESS) {
      if(callback(&place, user_data)) {
        char* buf = (char*)calloc(1, len+1);
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

    if (place->type)         yyjson_mut_obj_add_str(doc, root, "type",         place->type);
    if (place->own_name)     yyjson_mut_obj_add_str(doc, root, "name",         place->own_name);
    if (place->country_code) yyjson_mut_obj_add_str(doc, root, "country_code", place->country_code);

    yyjson_mut_obj_add_int(doc, root, "lon_e7",      place->lon_e7);
    yyjson_mut_obj_add_int(doc, root, "lat_e7",      place->lat_e7);

    size_t len = 0;
    const char *json = yyjson_mut_write(doc, 0, &len);
    if (json && len < sizeof(buf)) {
        memcpy(buf, json, len);
        buf[len] = '\0';
    } else {
        snprintf(buf, sizeof(buf), "{\"error\":\"serialization failed\", \"len\":\"%zu\"}, json: %s", len, json);
    }
    free((void *)json);
    yyjson_mut_doc_free(doc);

    return buf;
}

/** @endcond */
