#pragma once

#include "fluxipc.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>

/* ─── Paths ───────────────────────────────────────────────────────────────── */
/*
 * Per-app runtime layout:
 *   socket  : /run/<appname>-fluxipc/<appname>.sock
 *   symlinks: /run/<appname>-fluxipc/<ipc-path>   (one symlink per endpoint)
 *   shm     : POSIX /fluxipc.<appname>             (/dev/shm/fluxipc.<appname>)
 *
 * The per-app directory name is "<appname>-fluxipc".
 * This gives each server its own directory under /run, avoids collisions,
 * and lets ordinary users invoke client symlinks without privilege.
 *
 * Directory permissions are 0755 (root creates, world-readable).
 * Socket permissions are 0666 so unprivileged clients can connect.
 * SHM permissions are 0644 so unprivileged clients can mmap read-only,
 * but the rwlock embedded in shm is opened RDWR (kernel allows this for
 * futex-based locks even with read-only mapping on many kernels; we open
 * RDWR from client side for the rwlock to work, which is safe because
 * clients only call pthread_rwlock_rdlock).
 */

/* Build the per-app run directory path: /run/<appname>-fluxipc */
#define FLUXIPC_APP_RUN_DIR(appname_buf, sz, appname) \
    snprintf((appname_buf), (sz), "/run/%s-fluxipc", (appname))

/* sun_path is 108 bytes; "/run/<63-char-name>-fluxipc/<63>.sock" = ~80 chars max */
#define FLUXIPC_SOCK_SUFFIX   ".sock"

/* POSIX shm name: /fluxipc.<appname> */
#define FLUXIPC_SHM_PREFIX    "/fluxipc."

/* ─── Shared-memory layout ────────────────────────────────────────────────── */
#define FLUXIPC_SHM_MAGIC         0x464C5849UL   /* "FLXI" */
#define FLUXIPC_SHM_VERSION       1
#define FLUXIPC_SHM_MAX_ENTRIES   512

#define FLUXIPC_FLAG_ACTIVE   (1u << 0)
#define FLUXIPC_FLAG_STATIC   (1u << 1)

typedef struct {
    char     path[FLUXIPC_PATH_MAX];
    char     usage[FLUXIPC_USAGE_MAX];
    uint32_t id;
    uint32_t flags;
    size_t   slot_data_sz;
    pid_t    owner_pid;
    char     sock_path[FLUXIPC_PATH_MAX];
} fluxipc_shm_entry_t;

typedef struct {
    uint32_t             magic;
    uint32_t             version;
    char                 prog_name[FLUXIPC_NAME_MAX];
    char                 sock_path[FLUXIPC_PATH_MAX];
    atomic_uint          entry_count;
    atomic_uint          next_id;
    pthread_rwlock_t     lock;
    fluxipc_shm_entry_t  entries[FLUXIPC_SHM_MAX_ENTRIES];
} fluxipc_shm_t;

/* ─── Wire protocol ───────────────────────────────────────────────────────── */
#define FLUXIPC_PROTO_MAGIC   0x464C5850UL  /* "FLXP" */
#define FLUXIPC_PROTO_VERSION 1

/** Request sent over Unix socket (followed by payload bytes for args) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t id;
    char     path[FLUXIPC_PATH_MAX];
    int      argc;
    uint32_t total_arg_len;
} __attribute__((packed)) fluxipc_req_hdr_t;

/** Response sent back over socket */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  status;
    uint32_t data_len;
} __attribute__((packed)) fluxipc_resp_hdr_t;

/* ─── Tree node ───────────────────────────────────────────────────────────── */
struct fluxipc_node {
    char                name[FLUXIPC_NAME_MAX];
    char                full_path[FLUXIPC_PATH_MAX];
    char                usage[FLUXIPC_USAGE_MAX];
    size_t              slot_data_sz;
    fluxipc_handler_fn  handler;
    void               *data;
    uint32_t            id;
    uint32_t            shm_idx;
    int                 is_leaf;

    struct fluxipc_node *parent;
    struct fluxipc_node *children;
    struct fluxipc_node *next_sibling;
};

/* ─── Global server context ───────────────────────────────────────────────── */
typedef struct {
    char              prog_name[FLUXIPC_NAME_MAX];
    char              run_dir[FLUXIPC_PATH_MAX];
    char              sock_path[FLUXIPC_PATH_MAX];
    char              shm_name[FLUXIPC_NAME_MAX + 16];
    int               server_fd;
    int               shm_fd;
    fluxipc_shm_t    *shm;
    size_t            shm_size;
    fluxipc_node_t   *root;
    pthread_mutex_t   tree_lock;
    volatile int      running;
    uint32_t          next_id;
} fluxipc_ctx_t;

extern fluxipc_ctx_t *g_ctx;

/* ─── Internal helpers ────────────────────────────────────────────────────── */
fluxipc_node_t *tree_find(fluxipc_node_t *root, const char *path);
fluxipc_node_t *tree_insert(fluxipc_node_t *root, const char *path);
void            tree_remove(fluxipc_node_t *node);
void            tree_destroy(fluxipc_node_t *root);

int  shm_open_create(fluxipc_ctx_t *ctx);
int  shm_add_entry(fluxipc_ctx_t *ctx, fluxipc_node_t *node, int is_static);
void shm_remove_entry(fluxipc_ctx_t *ctx, uint32_t shm_idx);
void shm_close(fluxipc_ctx_t *ctx);
fluxipc_shm_t *shm_open_existing(const char *prog_name);
void           shm_close_existing(fluxipc_shm_t *shm);

int  sock_server_create(fluxipc_ctx_t *ctx);
int  sock_server_poll(fluxipc_ctx_t *ctx);
void sock_server_close(fluxipc_ctx_t *ctx);

int  symlink_create(const char *prog_name, const char *ipc_path,
                    const char *run_dir);
void symlink_remove(const char *prog_name, const char *ipc_path,
                    const char *run_dir);
void symlink_remove_all(const char *run_dir);

int  client_ipc_path(const char *argv0, const char *prog_name,
                     char *out_path, size_t path_sz);
