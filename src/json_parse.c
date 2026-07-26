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
  /*static const char* expectedTypes[] = {
    "house",
    "street",
    //"state",
    "city",
    "county",
    // "country",
    "other",
    "district",
    "locality"
  };
  bool asExpected = false;
  for (size_t i = 0; i < sizeof(expectedTypes) / sizeof(expectedTypes[0]); i++) {
    if (strcmp(type, expectedTypes[i]) == 0) {
      asExpected = true;
      break;
    }
  }
  if (!asExpected) {
    printf("unexpected type: %s\n", type);
  }*/
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

/*
 *
## example for district
{
  "type":"Place",
  "content":[
    {
      "place_id":"133982153",
      "object_type":"N",
      "object_id":240095751,
      "osm_key":"place",
      "osm_value":"hamlet",
      "categories":[
        "osm.place.hamlet"
      ],
      "address_type":"district",
      "importance":0.21337448257116948,
      "name":{
        "name":"Neubleyen"
      },
      "address":{
        "other":[
          "DE-BB",
          "Bleyen",
          "Golzow"
        ],
        "city":"Bleyen-Genschmar",
        "county":"Märkisch-Oderland",
        "county:de":"Märkisch-Oderland",
        "state:de":"Brandenburg",
        "state":"Brandenburg",
        "city:de":"Bleyen-Genschmar",
      },
      "extra":{
        "wikipedia":"de:Bleyen",
        "wikidata":"Q883967"
      },
      "postcode":"15328",
      "country_code":"de",
      "centroid":[
        14.6064559,
        52.589975
      ],
      "bbox":[
        14.6064559,
        52.589975,
        14.6064559,
        52.589975
      ],
      "geometry":{
        "type":"Point",
        "coordinates":[
          14.6064559,
          52.589975
        ]
      }
    }
  ]
}

## example for locality
  {
    "type":"Place",
    "content":[
      {
        "place_id":"133951735",
        "object_type":"N",
        "object_id":7196117845,
        "osm_key":"place",
        "osm_value":"isolated_dwelling",
        "categories":[
          "osm.place.isolated_dwelling"
        ],
        "address_type":"locality",
        "importance":0.10671031586175324,
        "name":{
          "name":"Henriettenhof"
        },
        "address":{
          "state:lt":"Brandenburgas",
          "other":[
            "DE-BB",
            "Golzow"
          ],
          "city":"Bleyen-Genschmar",
          "county":"Märkisch-Oderland",
          "suburb:de":"Genschmar",
          "county:de":"Märkisch-Oderland",
          "state:de":"Brandenburg",
          "state":"Brandenburg",
          "city:de":"Bleyen-Genschmar",
          "suburb":"Genschmar",
        },
        "postcode":"15328",
        "country_code":"de",
        "centroid":[
          14.501003,
          52.623578
        ],
        "bbox":[
          14.501003,
          52.623578,
          14.501003,
          52.623578
        ],
        "geometry":{
          "type":"Point",
          "coordinates":[
            14.501003,
            52.623578
          ]
        }
      }
    ]
  }
*/


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

  /* --- address sub-object --- */
  yyjson_val *address = yyjson_obj_get(entry, "address");
  if (!yyjson_is_obj(address)) {
    if (yyjson_is_arr(yyjson_obj_get(entry, "addresslines"))) p->unsupported = 1;
    return RESULT_SKIP;
  }
  const char* osm_value = localized(entry, "osm_value", NULL);
  // residential
  /*if (!strcmp("bus_stop", osm_value)) {
    return RESULT_SKIP;
  }*/
  p->type = localized(entry, "address_type", NULL);
  if (!p->type) {
    return RESULT_ERROR_MISSING_TYPE;
  }
  p->typeEnum = detectTypeEnum(p->type);
  if (PHOTON_PLACE_TYPE_OTHER == p->typeEnum) {
    return RESULT_SKIP;
  }
  if (PHOTON_PLACE_TYPE_STREET == p->typeEnum) {
    if (strcmp("residential", osm_value)) {
      return RESULT_SKIP;
    }
  }
  if (PHOTON_PLACE_TYPE_HOUSE == p->typeEnum) {
    if (strcmp("place", osm_value)) {
      return RESULT_SKIP;
    }
  }
  p->country_code = localized(entry, "country_code", NULL);
  p->own_name = place_name(entry);
  /* --- single-pass address field extraction --- */

  yyjson_obj_iter iter;
  yyjson_obj_iter_init(address, &iter);
  yyjson_val *key, *val;
  while ((key = yyjson_obj_iter_next(&iter))) {
    val = yyjson_obj_iter_get_val(key);
    if (!yyjson_is_str(val)) continue;
    const char *k = yyjson_get_str(key);
    size_t kl = yyjson_get_len(key);
    const char *v = yyjson_get_str(val);

    if (k[0] == 'h' && k[3] == 's') {
      p->house = v;
    } else {
      switch (kl) {
      case 4:
        if (k[0] == 'c') p->city = v;
        break;
      case 5:
        if (k[0] == 's') p->state = v;
        break;
      case 6:
        if (k[0] == 'c') p->county = v;
        else if (k[0] == 's') {
          if(k[1] == 't') p->street = v;
          else if(k[1] == 'u') p->suburb = v;
        }
        break;
      case 7:
        if (k[0] == 'c' && k[1] == 'o' && k[5] == 'r') p->country = v;
        break;
      case 8:
        if (k[0] == 'p' && k[7] == 'e') p->postcode = v;
        break;
      }
    }
  }

  switch(p->typeEnum) {
    case PHOTON_PLACE_TYPE_HOUSE: p->house = p->own_name; break;
    case PHOTON_PLACE_TYPE_STREET: p->street = p->own_name; break;
    case PHOTON_PLACE_TYPE_CITY: p->city = p->own_name; break;
    case PHOTON_PLACE_TYPE_COUNTY: p->county = p->own_name; break;
    case PHOTON_PLACE_TYPE_STATE: p->state = p->own_name; break;
    case PHOTON_PLACE_TYPE_COUNTRY: p->country = p->own_name; break;
    default:
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

  if (p->typeEnum == PHOTON_PLACE_TYPE_UNKNOWN) {
    return RESULT_ERROR_UNKNOWN_TYPE;
  }
  return RESULT_SUCCESS;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

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
    }
    else if (extractResult == RESULT_SUCCESS) {
      static int count = 0;
      if (true &&
        ((place.street && strcmp(place.street, "Suchlandstraße") == 0) &&
         (!place.state /*|| strcmp(place.state, "Könnern") == 0*/) && count <= 10)) { //*/
      //if (place.typeEnum == PHOTON_PLACE_TYPE_HOUSE && count <= 10) {
      //if (place.typeEnum == PHOTON_PLACE_TYPE_CITY && !place.city && count <= 10) {
        char* buf = (char*)calloc(1, len+1);
        memcpy(buf, line, len);
        printf("line: %s\n\n", buf);
        free(buf);
        count++;
      }
      callback(&place, user_data);
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
    if (place->country)      yyjson_mut_obj_add_str(doc, root, "country",      place->country);
    if (place->state)        yyjson_mut_obj_add_str(doc, root, "state",        place->state);
    if (place->county)       yyjson_mut_obj_add_str(doc, root, "county",       place->county);
    if (place->city)         yyjson_mut_obj_add_str(doc, root, "city",         place->city);
    if (place->suburb)       yyjson_mut_obj_add_str(doc, root, "suburb",       place->suburb);
    if (place->postcode)     yyjson_mut_obj_add_str(doc, root, "postcode",     place->postcode);
    if (place->street)       yyjson_mut_obj_add_str(doc, root, "street",       place->street);
    if (place->house)        yyjson_mut_obj_add_str(doc, root, "house",        place->house);

    yyjson_mut_obj_add_int(doc, root, "lon_e7",      place->lon_e7);
    yyjson_mut_obj_add_int(doc, root, "lat_e7",      place->lat_e7);
    yyjson_mut_obj_add_bool(doc, root, "has_point",  place->has_point);
    yyjson_mut_obj_add_bool(doc, root, "unsupported", place->unsupported);

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
