/**
 * Node addon over the geo index client.
 *
 * Four functions and an opaque handle — the same shape the Bun binding reaches
 * through `bun:ffi`, so both can present the identical `GeoIndex` class. The
 * search hands back JSON rather than a built-up object graph: the library
 * writes it in one go, and one string crossing the boundary costs less than
 * nine property assignments per result.
 *
 * The handle is a N-API external. Whoever forgets to close it still gets it
 * closed when the garbage collector reaches it; whoever closes it twice is
 * simply ignored the second time.
 */

#include "client.h"

#include <node_api.h>
#include <stdlib.h>
#include <string.h>

/** What a JavaScript handle actually holds. */
typedef struct Handle {
  GeoClient *client; /**< NULL once closed — the finalizer then has nothing to do. */
  char *buffer;      /**< Reused answer buffer; grows, never shrinks. */
  size_t capacity;
} Handle;

/** Initial size of the answer buffer, enough for a couple of dozen results. */
#define INITIAL_BUFFER (16 * 1024)

/** Throw and return NULL — the shape every failing entry point ends in. */
static napi_value fail(napi_env env, const char *message) {
  napi_throw_error(env, NULL, message);
  return NULL;
}

/** Read a JavaScript string as freshly allocated UTF-8, or NULL. */
static char *string_argument(napi_env env, napi_value value, size_t *out_size) {
  size_t size = 0;
  if (napi_get_value_string_utf8(env, value, NULL, 0, &size) != napi_ok) return NULL;

  char *text = malloc(size + 1);
  if (!text) return NULL;
  if (napi_get_value_string_utf8(env, value, text, size + 1, &size) != napi_ok) {
    free(text);
    return NULL;
  }
  if (out_size) *out_size = size;
  return text;
}

/** Take the handle out of the first argument. */
static Handle *handle_argument(napi_env env, napi_value value) {
  void *data = NULL;
  if (napi_get_value_external(env, value, &data) != napi_ok) return NULL;
  return data;
}

/** Release everything a handle holds. Runs on close() and again on collection. */
static void handle_release(Handle *handle) {
  if (!handle) return;
  if (handle->client) {
    geo_client_close(handle->client);
    handle->client = NULL;
  }
  free(handle->buffer);
  handle->buffer = NULL;
  handle->capacity = 0;
}

static void finalize(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  handle_release(data);
  free(data);
}

/* =========================================================================
 *  open(path) -> handle
 * ========================================================================= */

static napi_value binding_open(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1) {
    return fail(env, "open() needs the path to an index file");
  }

  char *path = string_argument(env, argv[0], NULL);
  if (!path) return fail(env, "open() needs the path as a string");

  Handle *handle = calloc(1, sizeof(*handle));
  if (!handle) {
    free(path);
    return fail(env, "out of memory");
  }

  GeoStatus status = geo_client_open(&handle->client, path);
  free(path);
  if (status != GEO_OK) {
    free(handle);
    switch (status) {
    case GEO_ERROR_FILE: return fail(env, "file cannot be read or mapped");
    case GEO_ERROR_FORMAT: return fail(env, "not an index this build can read");
    case GEO_ERROR_MEMORY: return fail(env, "out of memory");
    default: return fail(env, "invalid argument");
    }
  }

  napi_value external;
  if (napi_create_external(env, handle, finalize, NULL, &external) != napi_ok) {
    handle_release(handle);
    free(handle);
    return fail(env, "cannot create the handle");
  }
  return external;
}

/* =========================================================================
 *  close(handle)
 * ========================================================================= */

static napi_value binding_close(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1) {
    return fail(env, "close() needs a handle");
  }
  /* The external itself stays alive until it is collected; only what it holds
     is let go here, and the finalizer will then find nothing left to do. */
  handle_release(handle_argument(env, argv[0]));
  return NULL;
}

/* =========================================================================
 *  info(handle) -> object
 * ========================================================================= */

/** Set one numeric property, reporting whether it worked. */
static bool set_number(napi_env env, napi_value object, const char *name, double value) {
  napi_value number;
  if (napi_create_double(env, value, &number) != napi_ok) return false;
  return napi_set_named_property(env, object, name, number) == napi_ok;
}

static napi_value binding_info(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1) {
    return fail(env, "info() needs a handle");
  }

  Handle *handle = handle_argument(env, argv[0]);
  if (!handle || !handle->client) return fail(env, "this index is already closed");

  GeoClientInfo counts;
  if (geo_client_info(handle->client, &counts) != GEO_OK) {
    return fail(env, "cannot read the index counts");
  }

  napi_value object;
  if (napi_create_object(env, &object) != napi_ok) return fail(env, "out of memory");

  if (!set_number(env, object, "fileSize", (double)counts.file_size) ||
      !set_number(env, object, "documents", (double)counts.documents) ||
      !set_number(env, object, "houses", (double)counts.houses) ||
      !set_number(env, object, "words", (double)counts.words) ||
      !set_number(env, object, "spellings", (double)counts.spellings) ||
      !set_number(env, object, "postings", (double)counts.postings) ||
      !set_number(env, object, "format", (double)counts.format)) {
    return fail(env, "cannot build the counts object");
  }
  return object;
}

/* =========================================================================
 *  searchJson(handle, query, prefixLast, limit) -> string
 * ========================================================================= */

static napi_value binding_search(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 4) {
    return fail(env, "searchJson() needs a handle, a query, a flag and a limit");
  }

  Handle *handle = handle_argument(env, argv[0]);
  if (!handle || !handle->client) return fail(env, "this index is already closed");

  size_t query_size = 0;
  char *query = string_argument(env, argv[1], &query_size);
  if (!query) return fail(env, "the query has to be a string");

  bool prefix_last = true;
  napi_get_value_bool(env, argv[2], &prefix_last);

  int32_t limit = 10;
  napi_get_value_int32(env, argv[3], &limit);
  if (limit < 1) limit = 1;

  if (!handle->buffer) {
    handle->buffer = malloc(INITIAL_BUFFER);
    if (!handle->buffer) {
      free(query);
      return fail(env, "out of memory");
    }
    handle->capacity = INITIAL_BUFFER;
  }

  /* Twice at most: if the answer does not fit, the buffer grows to the size
     the library reported and the call is repeated once. */
  size_t needed = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    needed = geo_client_search_json(
        handle->client, query, query_size, prefix_last, (size_t)limit, handle->buffer,
        handle->capacity
    );
    if (needed < handle->capacity) break;

    char *grown = realloc(handle->buffer, needed + 1);
    if (!grown) {
      free(query);
      return fail(env, "out of memory");
    }
    handle->buffer = grown;
    handle->capacity = needed + 1;
  }
  free(query);

  napi_value answer;
  if (napi_create_string_utf8(env, handle->buffer, needed, &answer) != napi_ok) {
    return fail(env, "cannot hand the answer over");
  }
  return answer;
}

/* =========================================================================
 *  Registration
 * ========================================================================= */

/** Bind one C function under a name in the module's exports. */
static bool export_function(
    napi_env env, napi_value exports, const char *name, napi_callback callback
) {
  napi_value function;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, NULL, &function) != napi_ok) {
    return false;
  }
  return napi_set_named_property(env, exports, name, function) == napi_ok;
}

NAPI_MODULE_INIT(/* napi_env env, napi_value exports */) {
  if (!export_function(env, exports, "open", binding_open) ||
      !export_function(env, exports, "close", binding_close) ||
      !export_function(env, exports, "info", binding_info) ||
      !export_function(env, exports, "searchJson", binding_search)) {
    napi_throw_error(env, NULL, "cannot register the geo index addon");
  }
  return exports;
}
