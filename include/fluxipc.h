#pragma once

/*
 * FluxIPC – namespace-tree IPC + standard HTTP framework.
 *
 * A FluxIPC server owns a namespace tree.  Every node is a container
 * (namespace) or a leaf (callable method).  A leaf exposes two independent
 * handlers invoked through two transports:
 *
 *   • IPC  (Unix socket, CLI)  – positional argv, raw bytes out
 *   • HTTP (REST, JSON)        – parsed request, cJSON response
 *
 * The two return styles are deliberately separate: IPC keeps the original
 * "formatted text buffer" contract, HTTP uses structured JSON.
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

/* ─── HTTP method bitmask ─────────────────────────────────────────────────── */
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
 * @param out     output buffer
 * @param out_cap capacity of @p out
 * @param out_len set to the number of bytes written
 * @return 0 on success, negative errno on error
 */
typedef int (*fluxipc_handler_fn)(void   *data,
                                  int     argc,
                                  char  **argv,
                                  char   *out,
                                  size_t  out_cap,
                                  size_t *out_len);

/* ─── HTTP request / response ─────────────────────────────────────────────── */
/**
 * Parsed HTTP request handed to a leaf's HTTP handler.
 * All pointers are borrowed for the duration of the call.
 */
typedef struct {
    const char          *method;      /* "GET","POST",...                    */
    const char          *path;        /* decoded namespace path, e.g. /a/b    */
    fluxipc_node_t      *node;        /* matched leaf                         */
    const fluxipc_kv_t  *query;       /* query parameters                     */
    size_t               query_count;
    const fluxipc_kv_t  *headers;     /* HTTP headers                         */
    size_t               header_count;
    const char          *content_type;
    const char          *body;        /* raw request body                     */
    size_t               body_len;
    const cJSON         *json;        /* parsed request body (may be NULL)    */
    void                *data;        /* the method's data pointer            */
} fluxipc_http_request_t;

/**
 * Filled by the HTTP handler.  Ownership of @c json transfers to the server,
 * which serialises and then cJSON_Delete()s it.  Leave @c json NULL for an
 * empty body.  @c status defaults to 200 when 0.
 */
typedef struct {
    int         status;       /* HTTP status code; 0 → 200                    */
    cJSON      *json;         /* response body (server takes ownership)       */
    const char *content_type; /* NULL → "application/json; charset=utf-8"     */
} fluxipc_http_response_t;

/** HTTP leaf handler. @return 0 or negative errno (→ HTTP 5xx). */
typedef int (*fluxipc_http_handler_fn)(void                          *data,
                                       const fluxipc_http_request_t  *req,
                                       fluxipc_http_response_t       *resp);

/* ─── Method descriptor ───────────────────────────────────────────────────── */
/**
 * Metadata + implementation of a leaf.
 *
 * Both @c ipc and @c http are optional, but a leaf must provide at least one.
 * @c summary / @c description / @c schema are copied by the library.  @c schema
 * is an optional JSON Schema (draft 2020-12) string for the HTTP body.
 */
typedef struct {
    const char             *summary;
    const char             *description;
    const char             *schema;
    uint32_t                methods;   /* FLUXIPC_M_*; 0 → FLUXIPC_M_DEFAULT */
    fluxipc_handler_fn      ipc;       /* socket / CLI (raw bytes)           */
    fluxipc_http_handler_fn http;      /* HTTP (JSON)                        */
    void                   *data;
} fluxipc_method_t;

/**
 * Convenience initialiser: FLUXIPC_METHOD(ipc_fn, http_fn, summary, schema,
 * methods).  Produces a compound literal, so it can be passed by address to
 * fluxipc_node_leaf() / fluxipc_register() or stored in a static table:
 *
 *     fluxipc_register("/greet/hello", &FLUXIPC_METHOD(
 *         hello_ipc, hello_http, "Say hello", NULL, FLUXIPC_M_GET));
 */
#define FLUXIPC_METHOD(ipc_fn, http_fn, sum, schema_json, masks) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .http = (http_fn), \
                        .summary = (sum), .schema = (schema_json), \
                        .methods = (masks) }

/** Like FLUXIPC_METHOD() but also sets the per-leaf @c data pointer. */
#define FLUXIPC_METHOD_D(ipc_fn, http_fn, data_ptr, sum, schema_json, masks) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .http = (http_fn), \
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
 * fluxipc_register() unless you already hold the parent container.
 * Returns a borrowed handle, or NULL (errno set; EEXIST if already present).
 */
fluxipc_node_t *fluxipc_node_leaf(fluxipc_node_t *parent,
                                  const char     *name,
                                  const fluxipc_method_t *method);

/** Look up a direct child.  Returns a borrowed handle or NULL. */
fluxipc_node_t *fluxipc_node_find(fluxipc_node_t *parent, const char *name);

/* ─── High-level registration ─────────────────────────────────────────────── */

/**
 * Register a leaf at an absolute namespace path, creating every missing
 * intermediate container on the way.  This is the concise alternative to
 * hand-walking fluxipc_node_mkdir():
 *
 *     fluxipc_register("/module/sensor/status", &FLUXIPC_METHOD(
 *         status_ipc, status_http, "Read the sensor", NULL, FLUXIPC_M_GET));
 *
 * The method is copied exactly like fluxipc_node_leaf(), so passing a stack
 * temporary or a compound literal is safe.  The call is atomic: on failure no
 * half-created containers are left behind.
 *
 * @param path    absolute path, e.g. "/a/b/c" (leading '/' required).
 * @param method  metadata + at least one handler.
 * @return borrowed leaf handle, or NULL with errno set
 *         (EINVAL bad path/method, EEXIST name already taken, ENOMEM).
 */
fluxipc_node_t *fluxipc_register(const char *path,
                                 const fluxipc_method_t *method);

/** One row of a static registration table (see fluxipc_register_all()). */
typedef struct {
    const char       *path;    /* absolute namespace path, e.g. "/a/b" */
    fluxipc_method_t  method;
} fluxipc_endpoint_t;

/**
 * Register an array of endpoints in one call.  Every row is registered with
 * fluxipc_register(); the first failure stops the loop.
 * @return 0 on success, or the negative errno of the offending entry.
 */
int fluxipc_register_all(const fluxipc_endpoint_t *endpoints, size_t count);

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

#ifdef __cplusplus
}
#endif
