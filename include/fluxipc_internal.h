#pragma once

#include "fluxipc.h"
#include <pthread.h>

/* ─── Paths ───────────────────────────────────────────────────────────────── */
/*
 * Per-app runtime layout:
 *   socket: /run/user/<uid>/<appname>-fluxipc/<appname>.sock
 *
 * Directory permissions are 0755 (world-readable).
 * Socket permissions are 0666 so unprivileged clients can connect.
 */

/* ─── Wire protocol (minimum viable, 12-byte header) ──────────────────────── */
/*
 * Request  = fluxipc_req_hdr_t, then `path_len` path bytes, then `arg_len`
 *            bytes of NUL-terminated argv strings.
 * Response = fluxipc_resp_hdr_t, then `data_len` payload bytes.
 *
 * op == FLUXIPC_OP_CALL      → invoke the leaf's IPC handler (raw byte reply)
 * op == FLUXIPC_OP_LIST      → return the whole leaf manifest (JSON reply)
 * op == FLUXIPC_OP_CALL_JSON → invoke the leaf's JSON handler, reply is JSON
 *
 * For CALL_JSON the argument block is `method\0` followed by NUL-terminated
 * parameters.  A single parameter starting with '{' or '[' is the JSON body;
 * `key=value` parameters become query pairs; bare parameters are positional
 * and are bound to the leaf's JSON Schema properties in declaration order.
 */
#define FLUXIPC_PROTO_MAGIC   0x464C5832UL  /* "FLX2" (bump on protocol change) */
#define FLUXIPC_OP_CALL       1
#define FLUXIPC_OP_LIST       2
#define FLUXIPC_OP_CALL_JSON  3

typedef struct {
    uint32_t magic;
    uint16_t op;
    uint16_t path_len;
    uint32_t arg_len;
} __attribute__((packed)) fluxipc_req_hdr_t;   /* 12 bytes */

typedef struct {
    uint32_t magic;
    int32_t  status;
    uint32_t data_len;
} __attribute__((packed)) fluxipc_resp_hdr_t;  /* 12 bytes */

/* ─── Cached JSON Schema fields (positional CALL_JSON) ───────────────────── */

typedef enum {
    FLUXIPC_SCHEMA_ANY = 0,
    FLUXIPC_SCHEMA_STRING,
    FLUXIPC_SCHEMA_INTEGER,
    FLUXIPC_SCHEMA_NUMBER,
    FLUXIPC_SCHEMA_BOOLEAN,
    FLUXIPC_SCHEMA_OBJECT,
    FLUXIPC_SCHEMA_ARRAY,
} fluxipc_schema_type_t;

typedef struct {
    char    *name;       /* owned by the node */
    uint8_t  type;
    uint8_t  item_type;  /* array items, otherwise ANY */
} fluxipc_schema_field_t;

/* ─── Tree node ───────────────────────────────────────────────────────────── */
/*
 * Hot lookup fields (name + sibling/children links) are kept at the front so
 * tree_find_child() touches as few cache lines as possible.  full_path and the
 * method descriptor are cold until a match is found.
 */
struct fluxipc_node {
    char                 name[FLUXIPC_NAME_MAX];
    struct fluxipc_node *next_sibling;
    struct fluxipc_node *children;
    struct fluxipc_node *parent;
    uint32_t             id;
    int                  is_leaf;

    fluxipc_method_t     method;      /* copied; strings owned by the node */

    /* Lazily-built cache for positional CALL_JSON binding. */
    fluxipc_schema_field_t *schema_fields;
    uint16_t             schema_field_count;
    uint8_t              schema_fields_built;

    char                 full_path[FLUXIPC_PATH_MAX];
};

/* ─── Global server context ───────────────────────────────────────────────── */
typedef struct fluxipc_ctx_s {
    char              prog_name[FLUXIPC_NAME_MAX];
    char              run_dir[FLUXIPC_PATH_MAX];
    char              sock_path[FLUXIPC_PATH_MAX];
    int               server_fd;
    fluxipc_node_t   *root;
    pthread_mutex_t   tree_lock;
    volatile int      running;
    uint32_t          next_id;
    int               http_fd;     /* TCP listener fd; -1 if HTTP disabled */
    int               wake_fd;     /* eventfd used by fluxipc_stop(); -1 if off */

    /* Reused per-call IPC output buffer (single-threaded dispatch). */
    char             *ipc_out;

    /* Global request observation hooks */
    fluxipc_request_hook_fn pre_hook;
    fluxipc_request_hook_fn post_hook;
    void                   *hook_user;
} fluxipc_ctx_t;

extern fluxipc_ctx_t *g_ctx;

/* ─── Unified connection layer (fluxipc_conn.c) ───────────────────────────── */
#define FLUXIPC_MAX_CONNS   32
#define FLUXIPC_RBUF_MAX    (512 * 1024)   /* max request bytes per conn */
#define FLUXIPC_IDLE_TIMEOUT_MS 30000      /* slowloris / idle connection cap */

typedef enum { FLUXIPC_CONN_UNIX = 0, FLUXIPC_CONN_HTTP } fluxipc_conn_kind_t;
typedef enum { FLUXIPC_CONN_READING = 0, FLUXIPC_CONN_WRITING } fluxipc_conn_state_t;

typedef struct fluxipc_conn {
    int                  fd;
    fluxipc_conn_kind_t  kind;
    uint64_t             last_active_ms;
    fluxipc_conn_state_t state;

    char   *rbuf; size_t rlen, rcap;   /* request accumulation  */
    char   *wbuf; size_t wlen, woff, wcap; /* response to flush */

    size_t  need;      /* UNIX: complete request size once header parsed */
    size_t  hdr_end;   /* HTTP: offset past \r\n\r\n                    */
    size_t  body_len;  /* HTTP: Content-Length                          */
} fluxipc_conn_t;

/** Single-select event loop over both listeners and all connections. */
int  conn_poll(fluxipc_ctx_t *ctx);
/** Queue a response for @p c (copies @p len bytes); returns 0/-1. */
int  conn_respond(fluxipc_conn_t *c, const void *data, size_t len);
/** Queue @p buf as the response and take ownership (no copy). */
int  conn_respond_own(fluxipc_conn_t *c, char *buf, size_t len);
/** Close and free a connection slot. */
void conn_close(fluxipc_conn_t *c);
/** Close every pooled connection (shutdown). */
void conn_pool_reset(void);
/** Initialize the pool (call once before the event loop starts). */
void conn_pool_init(void);
/** Number of free connection slots (for tests / diagnostics). */
int  conn_pool_free_count(void);

/* Test-only hooks (compiled only with -DFLUXIPC_TESTING).  They expose the
 * otherwise-static connection pool and state-machine steps so the unit suite
 * can drive accept/read/write deterministically over socketpairs. */
#ifdef FLUXIPC_TESTING
fluxipc_conn_t *fluxipc_ut_conn(int idx);
int             fluxipc_ut_conn_alloc(int fd, int kind);
void            fluxipc_ut_conn_read(fluxipc_ctx_t *ctx, fluxipc_conn_t *c);
void            fluxipc_ut_conn_write(fluxipc_conn_t *c);
int             fluxipc_ut_pool_size(void);
#endif

/** Per-kind request processor: 0 = need more, 1 = handled, -1 = error. */
int  sock_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c);
int  http_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c);

/* ─── HTTP server (listener only) ─────────────────────────────────────────── */
int  fluxipc_http_start(fluxipc_ctx_t *ctx, uint16_t port);
void fluxipc_http_stop(fluxipc_ctx_t *ctx);
#define fluxipc_http_poll(ctx) conn_poll(ctx)

/* ─── Dispatch ────────────────────────────────────────────────────────────── */
/** Invoke a leaf's IPC handler (raw bytes). @return handler rc or -ENOENT. */
int fluxipc_dispatch_ipc(fluxipc_ctx_t *ctx, const char *path,
                         int argc, char **argv,
                         void *out, size_t out_cap, size_t *out_len);

/** Invoke a leaf's JSON handler. @return handler rc or -ENOSYS. */
int fluxipc_dispatch_http(fluxipc_ctx_t *ctx, fluxipc_json_request_t *req,
                          fluxipc_json_response_t *resp);

/**
 * Invoke a leaf's JSON handler from the local JSON transport and serialise the
 * response body.  *out is a malloc'd, NUL-terminated JSON document owned by
 * the caller.  Error results are returned as a JSON problem document too.
 * @return handler rc (0 or negative errno).
 */
int fluxipc_dispatch_json(fluxipc_ctx_t *ctx, const char *path,
                          const char *method,
                          const fluxipc_kv_t *query, size_t query_count,
                          const char *body, size_t body_len,
                          const cJSON *json,
                          char **out, size_t *out_len);

/**
 * Bind positional CALL_JSON arguments to the leaf's JSON Schema properties,
 * in declaration order.  A trailing array property consumes all remaining
 * arguments.  Returns 0 and a new cJSON object, or a negative errno:
 * -EINVAL (no/invalid schema) or -E2BIG (more args than properties).
 */
int fluxipc_json_bind_positional(fluxipc_ctx_t *ctx, const char *path,
                                 int argc, char **argv, cJSON **out);

/**
 * Build a JSON manifest of every leaf at or below @p node.
 * @p recursive 0 lists only direct leaves; @p verbose adds description/schema.
 * @return a cJSON object owned by the caller, or NULL on allocation failure.
 */
cJSON *fluxipc_build_manifest(fluxipc_node_t *node, int recursive, int verbose);

/* ─── Tree helpers ────────────────────────────────────────────────────────── */
fluxipc_node_t *tree_find_child(fluxipc_node_t *parent, const char *name);
fluxipc_node_t *tree_find_path(fluxipc_node_t *root, const char *path);

/**
 * Return the lazily-built positional binding fields for @p node.
 * Must be called while holding g_ctx->tree_lock.  @p count may be NULL.
 */
const fluxipc_schema_field_t *tree_node_schema_fields(fluxipc_node_t *node,
                                                      size_t *count);
fluxipc_node_t *tree_mkdir(fluxipc_node_t *parent, const char *name);
fluxipc_node_t *tree_leaf(fluxipc_node_t *parent, const char *name,
                          const fluxipc_method_t *method);
/** Merge @p src into an existing leaf: fill only empty handlers/metadata. */
void            tree_leaf_merge(fluxipc_node_t *node,
                                const fluxipc_method_t *src);
void            tree_node_free(fluxipc_node_t *node);
void            tree_destroy(fluxipc_node_t *root);

/* ─── Unix socket transport (listener + client) ───────────────────────────── */
int  sock_server_create(fluxipc_ctx_t *ctx);
void sock_server_close(fluxipc_ctx_t *ctx);

/** Like fluxipc_call() but mallocs the response body (caller frees *out). */
int  sock_call_alloc(const char *sock_path, uint16_t op, const char *ipc_path,
                     int argc, char **argv, char **out, size_t *out_len);

/* ─── Pure helpers (fluxipc_util.c) ───────────────────────────────────────── */
size_t fluxipc_url_decode(const char *src, size_t slen, char *dst, size_t cap);
int    fluxipc_parse_query(const char *raw, size_t rawlen,
                           fluxipc_kv_t *kv, int max_kv,
                           char *arena, size_t arena_cap);
int    fluxipc_find_content_length(const char *buf, size_t hbytes,
                                   size_t *out);

/* ─── Misc helpers ────────────────────────────────────────────────────────── */
int  fluxipc_method_to_mask(const char *method);
const char *fluxipc_mask_to_method(uint32_t bit);
