#pragma once

/*
 * FluxIPC – namespace-tree IPC + standard HTTP framework.
 *
 * A FluxIPC server owns a namespace tree.  Every node is a container
 * (namespace) or a leaf (callable method).  A leaf exposes two independent
 * representations:
 *
 *   • IPC  (Unix socket, CLI)        – positional argv, raw bytes out
 *   • JSON (HTTP + local JSON call)  – parsed request, cJSON response
 *
 * The default return styles are deliberately separate: IPC keeps the original
 * "formatted text buffer" contract, while the JSON representation uses cJSON.
 * Callers that want structured JSON over the Unix socket can use
 * fluxipc_call_json(), which invokes the same JSON handler as HTTP without any
 * HTTP/TCP traffic.
 */

#include <stdint.h>
#include <stddef.h>

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Limits ──────────────────────────────────────────────────────────────── */
#define FLUXIPC_PATH_MAX    256
#define FLUXIPC_NAME_MAX    64
#define FLUXIPC_USAGE_MAX   256
#define FLUXIPC_MAX_ARGS    32

/* ─── Method bitmask (HTTP verbs, also used by local JSON calls) ──────────── */
#define FLUXIPC_M_GET     (1u << 0)
#define FLUXIPC_M_POST    (1u << 1)
#define FLUXIPC_M_PUT     (1u << 2)
#define FLUXIPC_M_PATCH   (1u << 3)
#define FLUXIPC_M_DELETE  (1u << 4)
#define FLUXIPC_M_ALL     (FLUXIPC_M_GET | FLUXIPC_M_POST | FLUXIPC_M_PUT | \
                           FLUXIPC_M_PATCH | FLUXIPC_M_DELETE)
#define FLUXIPC_M_DEFAULT (FLUXIPC_M_GET | FLUXIPC_M_POST)

/* ─── Forward declarations ────────────────────────────────────────────────── */
typedef struct fluxipc_node fluxipc_node_t;

/** One key/value pair (query parameter or HTTP header). */
typedef struct {
    const char *key;
    const char *value;
} fluxipc_kv_t;

/* ─── IPC handler ─────────────────────────────────────────────────────────── */
/**
 * Invoked for Unix-socket / CLI calls.  Matches the original FluxIPC contract:
 * positional arguments and a caller-provided raw output buffer.
 *
 * @param data    the method's data pointer
 * @param argc    argument count; argv[0] is the endpoint path
 * @param argv    argument vector
 * @param out_buf output buffer (the main-branch signature uses @c void *)
 * @param out_cap capacity of @p out_buf
 * @param out_len set to the number of bytes written
 * @return 0 on success, negative errno on error
 */
typedef int (*fluxipc_handler_fn)(void   *data,
                                  int     argc,
                                  char  **argv,
                                  void   *out_buf,
                                  size_t  out_cap,
                                  size_t *out_len);

/* ─── JSON request / response ─────────────────────────────────────────────── */
/**
 * Parsed JSON request handed to a leaf's structured handler.  The same request
 * type is used by HTTP and by local IPC calls (`fluxipc_call_json`); business
 * code should not care which transport produced it.
 *
 * All pointers are borrowed for the duration of the call.
 */
typedef struct {
    const char          *method;      /* original verb string, e.g. "PUT"     */
    uint32_t             verb;        /* parsed FLUXIPC_M_* bit; use macros    */
    const char          *path;        /* decoded namespace path, e.g. /a/b    */
    fluxipc_node_t      *node;        /* matched leaf                         */
    const fluxipc_kv_t  *query;       /* query parameters                     */
    size_t               query_count;
    const fluxipc_kv_t  *headers;     /* HTTP headers (empty for local JSON)  */
    size_t               header_count;
    const char          *content_type;
    const char          *body;        /* raw request body                     */
    size_t               body_len;
    const cJSON         *json;        /* parsed request body (may be NULL)    */
    void                *data;        /* the method's data pointer            */
} fluxipc_json_request_t;

/** Backward-compatible alias for code written against the HTTP API. */
typedef fluxipc_json_request_t fluxipc_http_request_t;

/**
 * Filled by a structured handler.  Ownership of @c json transfers to the
 * server, which serialises and then cJSON_Delete()s it.  Leave @c json NULL
 * for an empty body.  @c status defaults to 200 when 0.
 */
typedef struct {
    int         status;       /* response status; 0 → 200                     */
    cJSON      *json;         /* response body (server takes ownership)       */
    const char *content_type; /* NULL → "application/json; charset=utf-8"     */
} fluxipc_json_response_t;

/** Backward-compatible alias for code written against the HTTP API. */
typedef fluxipc_json_response_t fluxipc_http_response_t;

/**
 * Structured leaf handler.  Invoked for HTTP requests and for local
 * `fluxipc_call_json()` calls.  @return 0 or negative errno.
 */
typedef int (*fluxipc_json_handler_fn)(void                          *data,
                                       const fluxipc_json_request_t  *req,
                                       fluxipc_json_response_t       *resp);

/** Backward-compatible alias. */
typedef fluxipc_json_handler_fn fluxipc_http_handler_fn;

/* ─── Request helpers ────────────────────────────────────────────────────── */

/**
 * Read a field from the parsed JSON body first, then from query parameters.
 * @return the value, or @p def when absent.
 */
const char *fluxipc_json_str(const fluxipc_json_request_t *req,
                             const char *key, const char *def);
long        fluxipc_json_int(const fluxipc_json_request_t *req,
                             const char *key, long def);
double      fluxipc_json_double(const fluxipc_json_request_t *req,
                                const char *key, double def);
int         fluxipc_json_bool(const fluxipc_json_request_t *req,
                              const char *key, int def);
int         fluxipc_json_has(const fluxipc_json_request_t *req,
                             const char *key);

/**
 * Build a JSON error response: `{"error": msg, "code": code}`.
 *
 * @p code may be a negative errno (mapped to a response status) or a positive
 * explicit status (e.g. 409).  Ownership of the built cJSON object transfers
 * to the server.
 * @return 0 on success, -ENOMEM on allocation failure.
 */
int fluxipc_json_error(fluxipc_json_response_t *resp, int code,
                       const char *msg);

/** True when the request verb contains any bit in @p mask. */
#define FLUXIPC_VERB_IS(req, mask)  (((req)->verb & (mask)) != 0)
/** True for GET. */
#define FLUXIPC_VERB_IS_READ(req)   FLUXIPC_VERB_IS((req), FLUXIPC_M_GET)
/** True for POST, PUT or PATCH. */
#define FLUXIPC_VERB_IS_WRITE(req)  FLUXIPC_VERB_IS((req), \
                                     FLUXIPC_M_POST | FLUXIPC_M_PUT | \
                                     FLUXIPC_M_PATCH)

/* ─── JSON Schema helpers ─────────────────────────────────────────────────── */
/**
 * Build a compact JSON Schema object for a method's structured request body.
 * The result is ordinary JSON Schema text (used by OpenAPI, the manifest and
 * positional CALL_JSON binding):
 *
 *     .schema = FLUXIPC_SCHEMA(
 *         FLUXIPC_PROP_INT("a") ","
 *         FLUXIPC_PROP_INT("b"))
 *
 * `type:object` is kept intentionally: it is the standard way to state that
 * the request body is an object, and OpenAPI clients rely on it.  For a
 * hand-written schema simply pass the JSON string directly.
 */
#define FLUXIPC_SCHEMA(...) \
    "{\"type\":\"object\",\"properties\":{" __VA_ARGS__ "}}"

/** One schema property with an explicit JSON Schema type string. */
#define FLUXIPC_PROP(name, type) \
    "\"" name "\":{\"type\":\"" type "\"}"

#define FLUXIPC_PROP_INT(name)     FLUXIPC_PROP(name, "integer")
#define FLUXIPC_PROP_NUMBER(name)  FLUXIPC_PROP(name, "number")
#define FLUXIPC_PROP_STR(name)     FLUXIPC_PROP(name, "string")
#define FLUXIPC_PROP_BOOL(name)    FLUXIPC_PROP(name, "boolean")

/** Array property with a scalar item type (e.g. "string"). */
#define FLUXIPC_PROP_ARRAY(name, item_type) \
    "\"" name "\":{\"type\":\"array\",\"items\":{\"type\":\"" \
    item_type "\"}}"

/* ─── Method descriptor ───────────────────────────────────────────────────── */
/**
 * Metadata + implementation of a leaf.
 *
 * Both @c ipc and @c json are optional, but a leaf must provide at least one.
 * @c summary / @c description / @c schema are copied by the library.  @c schema
 * is an optional JSON Schema (draft 2020-12) string for the structured body.
 */
typedef struct {
    const char              *summary;
    const char              *description;
    const char              *schema;
    uint32_t                 methods;   /* FLUXIPC_M_*; 0 → FLUXIPC_M_DEFAULT */
    fluxipc_handler_fn       ipc;       /* socket / CLI (raw bytes)            */
    union {
        fluxipc_json_handler_fn json;   /* structured JSON (HTTP + local)      */
        fluxipc_json_handler_fn http;   /* backward-compatible alias           */
    };
    void                    *data;
} fluxipc_method_t;

/**
 * Convenience initialiser: FLUXIPC_METHOD(ipc_fn, json_fn, summary, schema,
 * methods).  Produces a compound literal, so it can be passed by address to
 * fluxipc_node_leaf() / fluxipc_register_method() or stored in a static table:
 *
 *     fluxipc_register_method("/greet/hello", &FLUXIPC_METHOD(
 *         hello_ipc, hello_json, "Say hello", NULL, FLUXIPC_M_GET));
 */
#define FLUXIPC_METHOD(ipc_fn, json_fn, sum, schema_json, masks) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .json = (json_fn), \
                        .summary = (sum), .schema = (schema_json), \
                        .methods = (masks) }

/** Like FLUXIPC_METHOD() but also sets the per-leaf @c data pointer. */
#define FLUXIPC_METHOD_D(ipc_fn, json_fn, data_ptr, sum, schema_json, masks) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .json = (json_fn), \
                        .data = (data_ptr), \
                        .summary = (sum), .schema = (schema_json), \
                        .methods = (masks) }

/* ─── Namespace tree ──────────────────────────────────────────────────────── */

/** Root node of the tree; valid after fluxipc_server_init(). */
fluxipc_node_t *fluxipc_tree_root(void);

/** Get or create a container child.  Returns a borrowed handle or NULL. */
fluxipc_node_t *fluxipc_node_mkdir(fluxipc_node_t *parent, const char *name);

/**
 * Register a leaf method under @p parent.  Low-level primitive; prefer
 * fluxipc_register_method() unless you already hold the parent container.
 * Returns a borrowed handle, or NULL (errno set; EEXIST if already present).
 */
fluxipc_node_t *fluxipc_node_leaf(fluxipc_node_t *parent,
                                  const char     *name,
                                  const fluxipc_method_t *method);

/** Look up a direct child.  Returns a borrowed handle or NULL. */
fluxipc_node_t *fluxipc_node_find(fluxipc_node_t *parent, const char *name);

/* ─── High-level registration ─────────────────────────────────────────────── */

/**
 * Register a leaf from a full method descriptor (path-based).  Creates every
 * missing intermediate container on the way:
 *
 *     fluxipc_register_method("/module/sensor/status", &FLUXIPC_METHOD(
 *         status_ipc, status_http, "Read the sensor", NULL, FLUXIPC_M_GET));
 *
 * The method is copied exactly like fluxipc_node_leaf(), so passing a stack
 * temporary or a compound literal is safe.  The call is atomic: on failure no
 * half-created containers are left behind.
 *
 * Registering the same path again is allowed and merges the two calls: only
 * still-empty handlers/metadata are filled, so an IPC-only registration can be
 * completed later by a JSON-only one (and vice versa).  The method mask
 * belongs to whichever call first attaches a JSON handler.
 *
 * @param path    absolute path, e.g. "/a/b/c" (leading '/' required).
 * @param method  metadata + at least one handler (ipc and/or json).
 * @return borrowed leaf handle, or NULL with errno set
 *         (EINVAL bad path/method, EEXIST name is a container, ENOMEM).
 */
fluxipc_node_t *fluxipc_register_method(const char *path,
                                        const fluxipc_method_t *method);

/**
 * Classic main-branch registration: register an IPC endpoint at @p path.
 *
 *     fluxipc_register("/devices/foo/name", "[<new-name>]", 0, handler, ctx);
 *
 * @p usage becomes the leaf summary.  @p slot_sz is accepted for source
 * compatibility only: the shared-memory slots of the old architecture are gone
 * and the value is ignored.  The structured JSON side can be added later with
 * fluxipc_register_json(); re-registering the same path merges instead of
 * overwriting.
 *
 * @return borrowed leaf handle, or NULL with errno set.
 */
fluxipc_node_t *fluxipc_register(const char *path, const char *usage,
                                 size_t slot_sz,
                                 fluxipc_handler_fn handler, void *data);

/**
 * Register the structured JSON side of an endpoint (the @c ipc field is
 * normally NULL).  Requires @c method->json.  Merges into an existing leaf of
 * the same path, so it is the natural companion for an earlier
 * fluxipc_register() call:
 *
 *     fluxipc_register     ("/greet/hello", "Say hello", 0, hello_ipc, NULL);
 *     fluxipc_register_json("/greet/hello", &FLUXIPC_METHOD(NULL, hello_json, ...));
 *
 * @return borrowed leaf handle, or NULL with errno set.
 */
fluxipc_node_t *fluxipc_register_json(const char *path,
                                      const fluxipc_method_t *method);

/** Backward-compatible alias for fluxipc_register_json(). */
fluxipc_node_t *fluxipc_register_http(const char *path,
                                      const fluxipc_method_t *method);

/** One row of a static registration table (see fluxipc_register_all()). */
typedef struct {
    const char       *path;    /* absolute namespace path, e.g. "/a/b" */
    fluxipc_method_t  method;
} fluxipc_endpoint_t;

/**
 * Register an array of endpoints in one call.  Every row is registered with
 * fluxipc_register_method(); the first failure stops the loop.
 * @return 0 on success, or the negative errno of the offending entry.
 */
int fluxipc_register_all(const fluxipc_endpoint_t *endpoints, size_t count);

/** Main-branch alias for fluxipc_node_remove(). */
void fluxipc_unregister(fluxipc_node_t *node);

/* ─── Compile-time registration (main-branch FLUXIPC_STATIC) ──────────────── */

/**
 * One compile-time endpoint.  Created by FLUXIPC_STATIC() and queued by a
 * constructor at load time; fluxipc_server_init() then registers every queued
 * entry.  The opaque @c next link is owned by the registry — do not touch it.
 */
typedef struct fluxipc_entry {
    const char           *path;
    const char           *usage;
    size_t                slot_data_sz;
    fluxipc_handler_fn    handler;
    void                 *data;
    struct fluxipc_entry *next;
} fluxipc_entry_t;

/** Internal: called by the FLUXIPC_STATIC() constructor.  Not for direct use. */
void fluxipc_static_add(fluxipc_entry_t *entry);

#define FLUXIPC_PP_CAT_(a, b) a##b
#define FLUXIPC_PP_CAT(a, b)  FLUXIPC_PP_CAT_(a, b)

/**
 * FLUXIPC_STATIC – declare a compile-time IPC endpoint (main-branch API).
 *
 * The entry is queued before @c main() and registered by
 * fluxipc_server_init(), so no @c -rdynamic / linker section is required.
 * @p usage becomes the leaf summary; @p slot_sz is accepted and ignored.
 *
 *     static int hello(void *d, int argc, char **argv,
 *                      void *out, size_t cap, size_t *len) { ... }
 *     FLUXIPC_STATIC("/greet/hello", "say hello", 0, hello, NULL);
 */
#define FLUXIPC_STATIC(ipc_path, usage_str, slot_sz, handler_fn, ctx)        \
    static fluxipc_entry_t FLUXIPC_PP_CAT(_fluxipc_static_, __LINE__) = {    \
        .path         = (ipc_path),                                          \
        .usage        = (usage_str),                                         \
        .slot_data_sz = (size_t)(slot_sz),                                   \
        .handler      = (handler_fn),                                        \
        .data         = (void *)(ctx),                                       \
    };                                                                       \
    __attribute__((constructor, used))                                       \
    static void FLUXIPC_PP_CAT(_fluxipc_static_ctor_, __LINE__)(void)        \
    {                                                                        \
        fluxipc_static_add(&FLUXIPC_PP_CAT(_fluxipc_static_, __LINE__));     \
    }

/** Remove a leaf, or a whole container subtree.  Prunes empty ancestors. */
void fluxipc_node_remove(fluxipc_node_t *node);

/** Callback for fluxipc_node_leaves(). Non-zero stops; runs under tree lock. */
typedef int (*fluxipc_leaf_cb)(const fluxipc_node_t *leaf, void *user);

/** Enumerate every leaf at or below @p node. @return count, or negative errno. */
int fluxipc_node_leaves(fluxipc_node_t *node,
                        fluxipc_leaf_cb cb, void *user);

/* ─── Node accessors (no locking) ─────────────────────────────────────────── */
const char             *fluxipc_node_path(const fluxipc_node_t *node);
const char             *fluxipc_node_name(const fluxipc_node_t *node);
const fluxipc_method_t *fluxipc_node_method(const fluxipc_node_t *node);
int                     fluxipc_node_is_leaf(const fluxipc_node_t *node);

/* ─── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * Initialise a FluxIPC server.
 * @param prog_name  Unique short server name (socket / run-dir).
 * @param http_port  TCP port for HTTP, or 0 to disable HTTP.
 * @return 0 on success, negative errno (-EALREADY if already initialised).
 */
int  fluxipc_server_init(const char *prog_name, uint16_t http_port);
int  fluxipc_interactive_init(const char *prog_name);

/* ─── Event loop ──────────────────────────────────────────────────────────── */
int  fluxipc_poll(void *unused);

/* ─── Shutdown ────────────────────────────────────────────────────────────── */
void fluxipc_stop(void);
void fluxipc_destroy(void);

/* ─── Help ────────────────────────────────────────────────────────────────── */
void fluxipc_usage(const char *prog);

/* ─── Request observation hooks ───────────────────────────────────────────── */
/** Read-only view of one request passed to the pre/post hooks. */
typedef struct {
    const char  *transport; /* "ipc" | "http"                                  */
    const char  *method;    /* "CALL" | "GET" | ...                            */
    const char  *path;      /* namespace path                                  */
    int          matched;   /* 1 if a handler was found                        */
    int          status;    /* handler return code — valid in POST hook only   */
    const cJSON *json;      /* HTTP response body — valid in POST hook only    */
    const void  *raw;       /* IPC response bytes — valid in POST hook only    */
    size_t       raw_len;
} fluxipc_event_t;

typedef void (*fluxipc_request_hook_fn)(const fluxipc_event_t *ev, void *user);

/**
 * Register global pre/post request hooks.  Either may be NULL.  Must be called
 * after fluxipc_server_init().  Calling again replaces the previous hooks.
 */
void fluxipc_set_request_hooks(fluxipc_request_hook_fn pre,
                               fluxipc_request_hook_fn post,
                               void                   *user);

/* ─── Client API ──────────────────────────────────────────────────────────── */

/** Callback for fluxipc_list_endpoints: receives one active endpoint. */
typedef void (*fluxipc_list_cb)(const char *path, uint32_t id,
                                const char *usage, void *userdata);

/**
 * Enumerate active IPC endpoints for a running server by querying its
 * Unix socket for the leaf manifest.
 * @return 0 on success, negative errno if the server cannot be reached.
 */
int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata);

/**
 * Low-level single IPC call to a Unix socket.
 * @return status from the handler (0 on success, negative errno on error).
 */
int fluxipc_call(const char *sock_path, const char *ipc_path,
                 int argc, char **argv,
                 void *out_buf, size_t out_cap, size_t *out_len);

/**
 * Call an endpoint over the Unix socket and return its JSON representation.
 *
 * The server reuses the leaf's JSON handler, so there is no HTTP/TCP traffic
 * and no duplicated JSON construction.  If the leaf has no JSON handler the
 * call returns -ENOSYS and *out still contains a valid JSON error document.
 *
 * @param sock_path  server socket path
 * @param ipc_path   endpoint path, e.g. "/system/info"
 * @param method     verb for the handler (GET/POST/...); NULL/empty → "GET"
 * @param paramc     number of request parameters (endpoint path excluded)
 * @param params     one JSON document, several "key=value" query pairs, or
 *                   positional values bound to the leaf's schema properties
 * @param out        receives a malloc'd, NUL-terminated JSON document
 * @param out_len    receives the document length (may be NULL)
 * @return status from the handler (0 on success, negative errno on error)
 */
int fluxipc_call_json(const char *sock_path, const char *ipc_path,
                      const char *method,
                      int paramc, char **params,
                      char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
