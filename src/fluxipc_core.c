/**
 * fluxipc_core.c – library entry points
 */

#include "fluxipc_internal.h"
#include "fluxipc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

fluxipc_ctx_t *g_ctx = NULL;

/* ─── Context allocation ──────────────────────────────────────────────────── */

static fluxipc_ctx_t *ctx_alloc(const char *prog_name)
{
    fluxipc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    snprintf(ctx->prog_name, FLUXIPC_NAME_MAX, "%s", prog_name);
    /* /run/<prog>-fluxipc */
    snprintf(ctx->run_dir, sizeof(ctx->run_dir), "/run/%s-fluxipc", prog_name);
    ctx->server_fd = -1;
    ctx->shm_fd    = -1;
    ctx->next_id   = 1;
    ctx->running   = 1;
    pthread_mutex_init(&ctx->tree_lock, NULL);

    ctx->root = calloc(1, sizeof(fluxipc_node_t));
    if (!ctx->root) { free(ctx); return NULL; }
    ctx->root->name[0]      = '/';
    ctx->root->name[1]      = '\0';
    ctx->root->full_path[0] = '/';
    ctx->root->full_path[1] = '\0';
    ctx->root->shm_idx      = UINT32_MAX;
    return ctx;
}

/* ─── Static entries ──────────────────────────────────────────────────────── */

static void register_static_entries(fluxipc_ctx_t *ctx)
{
    if (!__start_fluxipc_registry || !__stop_fluxipc_registry) return;

    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {
        if (!e->path || !e->handler) continue;

        pthread_mutex_lock(&ctx->tree_lock);
        fluxipc_node_t *node = tree_insert(ctx->root, e->path);
        if (node) {
            snprintf(node->full_path, FLUXIPC_PATH_MAX, "%s", e->path);
            snprintf(node->usage, FLUXIPC_USAGE_MAX, "%s",
                     e->usage ? e->usage : "");
            node->slot_data_sz = e->slot_data_sz;
            node->handler      = e->handler;
            node->data         = e->data;
            node->is_leaf      = 1;
        }
        pthread_mutex_unlock(&ctx->tree_lock);

        if (node) {
            shm_add_entry(ctx, node, 1);
            symlink_create(ctx->prog_name, e->path, ctx->run_dir);
        }
    }
}

/* ─── fluxipc_server_init ─────────────────────────────────────────────────── */

int fluxipc_server_init(const char *prog_name)
{
    if (g_ctx) return -EALREADY;

    fluxipc_ctx_t *ctx = ctx_alloc(prog_name);
    if (!ctx) return -ENOMEM;

    /* Create /run/<prog>-fluxipc/ with mode 0755 so ordinary users can enter */
    if (mkdir(ctx->run_dir, 0755) < 0 && errno != EEXIST) {
        perror("fluxipc: mkdir run_dir");
        /* non-fatal in case parent dir creation needed */
    }
    /* Ensure permissions even if it already existed */
    chmod(ctx->run_dir, 0755);

    int rc = sock_server_create(ctx);
    if (rc < 0) { free(ctx->root); free(ctx); return rc; }

    rc = shm_open_create(ctx);
    if (rc < 0) {
        sock_server_close(ctx);
        free(ctx->root); free(ctx);
        return rc;
    }

    g_ctx = ctx;
    register_static_entries(ctx);
    return 0;
}

/* ─── fluxipc_client_dispatch ──────────────────────────────────────────────── */

int fluxipc_client_dispatch(int argc, char **argv)
{
    if (argc < 1 || !argv || !argv[0]) return -1;

    char prog[FLUXIPC_NAME_MAX], sock[FLUXIPC_PATH_MAX], shm[64];
    if (fluxipc_client_resolve(argv[0], prog, sizeof(prog),
                       sock, sizeof(sock), shm, sizeof(shm)) != 0)
        return -1;

    char ipc_path[FLUXIPC_PATH_MAX];
    if (client_ipc_path(argv[0], prog, ipc_path, sizeof(ipc_path)) != 0)
        return -1;

    char *call_argv[FLUXIPC_MAX_ARGS + 1];
    int call_argc = 1;
    call_argv[0] = ipc_path;
    for (int i = 1; i < argc && call_argc <= FLUXIPC_MAX_ARGS; i++)
        call_argv[call_argc++] = argv[i];

    char out[65536];
    size_t out_len = 0;
    int rc = fluxipc_call(sock, ipc_path, call_argc, call_argv,
                          out, sizeof(out), &out_len);
    if (rc < 0) {
        fprintf(stderr, "fluxipc error %d: %s\n", rc, strerror(-rc));
        return rc;
    }
    if (out_len > 0) fwrite(out, 1, out_len, stdout);
    return 0;
}

/* ─── fluxipc_register ────────────────────────────────────────────────────── */

fluxipc_node_t *fluxipc_register(const char *path, const char *usage,
                                  size_t slot_sz, fluxipc_handler_fn handler,
                                  void *data)
{
    if (!g_ctx || !path || !handler) return NULL;

    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *node = tree_insert(g_ctx->root, path);
    if (!node) { pthread_mutex_unlock(&g_ctx->tree_lock); return NULL; }
    snprintf(node->full_path, FLUXIPC_PATH_MAX, "%s", path);
    snprintf(node->usage, FLUXIPC_USAGE_MAX, "%s", usage ? usage : "");
    node->slot_data_sz = slot_sz;
    node->handler      = handler;
    node->data         = data;
    node->is_leaf      = 1;
    pthread_mutex_unlock(&g_ctx->tree_lock);

    shm_add_entry(g_ctx, node, 0);
    symlink_create(g_ctx->prog_name, path, g_ctx->run_dir);
    return node;
}

/* ─── fluxipc_unregister ──────────────────────────────────────────────────── */

void fluxipc_unregister(fluxipc_node_t *node)
{
    if (!node || !g_ctx) return;
    symlink_remove(g_ctx->prog_name, node->full_path, g_ctx->run_dir);
    if (node->shm_idx != UINT32_MAX)
        shm_remove_entry(g_ctx, node->shm_idx);
    pthread_mutex_lock(&g_ctx->tree_lock);
    tree_remove(node);
    pthread_mutex_unlock(&g_ctx->tree_lock);
}

/* ─── Poll / stop / destroy ───────────────────────────────────────────────── */

int fluxipc_poll(void *unused)
{
    (void)unused;
    if (!g_ctx || !g_ctx->running) return -EINVAL;
    return sock_server_poll(g_ctx);
}

void fluxipc_stop(void)  { if (g_ctx) g_ctx->running = 0; }

void fluxipc_destroy(void)
{
    if (!g_ctx) return;
    fluxipc_ctx_t *ctx = g_ctx; g_ctx = NULL;
    sock_server_close(ctx);
    shm_close(ctx);
    symlink_remove_all(ctx->run_dir);
    pthread_mutex_lock(&ctx->tree_lock);
    tree_destroy(ctx->root); ctx->root = NULL;
    pthread_mutex_unlock(&ctx->tree_lock);
    pthread_mutex_destroy(&ctx->tree_lock);
    free(ctx);
}

/* ─── List endpoints ─────────────────────────────────────────────────────── */

int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata)
{
    fluxipc_shm_t *shm = shm_open_existing(prog_name);
    if (!shm) return -ENOENT;

    pthread_rwlock_rdlock((pthread_rwlock_t *)&shm->lock);
    for (int i = 0; i < FLUXIPC_SHM_MAX_ENTRIES; i++) {
        const fluxipc_shm_entry_t *e = &shm->entries[i];
        if (e->flags & FLUXIPC_FLAG_ACTIVE)
            cb(e->path, e->id, e->usage[0] ? e->usage : "", userdata);
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&shm->lock);
    shm_close_existing(shm);
    return 0;
}

/* ─── Usage ───────────────────────────────────────────────────────────────── */

static void usage_tree_r(const fluxipc_node_t *node, int depth)
{
    if (!node) return;
    if (node->is_leaf)
        printf("  %-40s  %s\n", node->full_path,
               node->usage[0] ? node->usage : "(no description)");
    fluxipc_node_t *c = node->children;
    while (c) { usage_tree_r(c, depth + 1); c = c->next_sibling; }
}

void fluxipc_usage(const char *prog)
{
    printf("Usage: %s <ipc-path> [args...]\n\n", prog ? prog : "fluxipc");
    printf("Registered IPC endpoints:\n");
    if (g_ctx && g_ctx->root) usage_tree_r(g_ctx->root, 0);
    else printf("  (none)\n");
    printf("\n");
}
