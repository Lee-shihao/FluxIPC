#pragma once

#include "fluxipc.h"

#include <pthread.h>
#include <stdatomic.h>

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
 * op == FLUXIPC_OP_CALL → invoke the leaf's IPC handler (raw byte reply)
 * op == FLUXIPC_OP_LIST → return the leaf manifest as text:
 *                         `<id>\t<path>\t<summary>\n` per line
 */
#define FLUXIPC_PROTO_MAGIC   0x464C5832UL  /* "FLX2" (bump on protocol change) */
#define FLUXIPC_OP_CALL       1
#define FLUXIPC_OP_LIST       2

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

    fluxipc_method_t     method;      /* copied; strings owned by the node */

    char                 full_path[FLUXIPC_PATH_MAX];
};

/** A node is a leaf iff it has an IPC handler (the only leaf kind). */
static inline int node_is_leaf(const fluxipc_node_t *n)
{
    return n && n->method.ipc != NULL;
}

/* ─── Global server context ───────────────────────────────────────────────── */
typedef struct fluxipc_ctx_s {
    char              prog_name[FLUXIPC_NAME_MAX];
    char              run_dir[FLUXIPC_PATH_MAX];
    char              sock_path[FLUXIPC_PATH_MAX];
    int               server_fd;
    fluxipc_node_t   *root;
    pthread_mutex_t   tree_lock;
    atomic_int        running;
    uint32_t          next_id;
    int               wake_fd;     /* eventfd used by fluxipc_wakeup(); -1 if off */

    /* Guards the single-poller invariant. */
    atomic_int        poll_busy;

    /* Reused per-call IPC output buffer (single-threaded dispatch). */
    char             *ipc_out;

    /* Global request observation hooks (protected by hook_lock). */
    pthread_mutex_t        hook_lock;
    fluxipc_request_hook_fn pre_hook;
    fluxipc_request_hook_fn post_hook;
    void                  *hook_user;
} fluxipc_ctx_t;

extern fluxipc_ctx_t *g_ctx;

/* ─── Unified connection layer (fluxipc_conn.c) ───────────────────────────── */
#define FLUXIPC_MAX_CONNS   32
#define FLUXIPC_RBUF_MAX    (512 * 1024)   /* max request bytes per conn */
#define FLUXIPC_IDLE_TIMEOUT_MS 30000      /* slowloris / idle connection cap */

typedef enum { FLUXIPC_CONN_READING = 0, FLUXIPC_CONN_WRITING } fluxipc_conn_state_t;

typedef struct fluxipc_conn {
    int                  fd;
    uint64_t             last_active_ms;
    fluxipc_conn_state_t state;

    char   *rbuf; size_t rlen, rcap;   /* request accumulation */
    char   *wbuf; size_t wlen, woff;   /* response to flush    */
} fluxipc_conn_t;

/**
 * Single-select event loop over the Unix listener and all connections.
 * @param timeout_ms <0 infinite, 0 non-blocking, >0 upper bound.
 * @return 1 activity handled, 0 timeout, negative errno on fatal error.
 */
int  conn_poll(fluxipc_ctx_t *ctx, int timeout_ms);
/** Queue @p buf as the response and take ownership (no copy). */
int  conn_respond(fluxipc_conn_t *c, char *buf, size_t len);
/** Close and free a connection slot. */
void conn_close(fluxipc_conn_t *c);
/** Close every pooled connection (shutdown). */
void conn_pool_reset(void);
/** Initialize the pool (call once before the event loop starts). */
void conn_pool_init(void);

/* Test-only hooks (compiled only with -DFLUXIPC_TESTING).  They expose the
 * otherwise-static connection pool and state-machine steps so the unit suite
 * can drive accept/read/write deterministically over socketpairs. */
#ifdef FLUXIPC_TESTING
fluxipc_conn_t *fluxipc_ut_conn(int idx);
int             fluxipc_ut_conn_alloc(int fd);
void            fluxipc_ut_conn_read(fluxipc_ctx_t *ctx, fluxipc_conn_t *c);
void            fluxipc_ut_conn_write(fluxipc_conn_t *c);
#endif

/** Request processor for a Unix-socket connection.
 *  @return 0 = need more, 1 = handled, -1 = error. */
int  sock_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c);

/* ─── Dispatch ────────────────────────────────────────────────────────────── */
/** Invoke a leaf's IPC handler (raw bytes). @return handler rc or -ENOENT. */
int fluxipc_dispatch_ipc(fluxipc_ctx_t *ctx, const char *path,
                         int argc, char **argv,
                         void *out, size_t out_cap, size_t *out_len);

/**
 * Build the text manifest of every leaf at or below @p node.
 * Lines are `<id>\t<path>\t<summary>\n`; tabs/newlines inside a field are
 * replaced with spaces so the format stays parseable.
 *
 * @param out_len receives the byte length (may be NULL).
 * @return a malloc'd, NUL-terminated buffer owned by the caller, or NULL.
 */
char *fluxipc_build_manifest(fluxipc_node_t *node, size_t *out_len);

/* ─── Tree helpers ────────────────────────────────────────────────────────── */
fluxipc_node_t *tree_find_child(fluxipc_node_t *parent, const char *name);
fluxipc_node_t *tree_find_path(fluxipc_node_t *root, const char *path);
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
