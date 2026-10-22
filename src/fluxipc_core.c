/**
 * fluxipc_core.c – library entry points
 *
 * Owns the global context, the namespace tree, dynamic registration, IPC
 * dispatch, the text manifest and the lifecycle / event loop.
 *
 * The library is deliberately single-poller: the host application decides
 * which thread runs fluxipc_run() (or drives fluxipc_poll_timeout() itself).
 * Registration and tree mutation are protected by tree_lock and may happen
 * concurrently with the polling thread.
 */

#include "fluxipc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <limits.h>

fluxipc_ctx_t *g_ctx = NULL;

static void fluxipc_static_apply(void);   /* FLUXIPC_STATIC registry, defined below */

static int valid_prog_name(const char *name)
{
    return name && name[0] &&
           strlen(name) < FLUXIPC_NAME_MAX && !strchr(name, '/');
}

/* ─── Context allocation ──────────────────────────────────────────────────── */

static fluxipc_ctx_t *ctx_alloc(const char *prog_name)
{
    fluxipc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    snprintf(ctx->prog_name, FLUXIPC_NAME_MAX, "%s", prog_name);
    snprintf(ctx->run_dir, sizeof(ctx->run_dir),
             "/run/user/%d/%s-fluxipc", getuid(), prog_name);
    ctx->server_fd = -1;
    ctx->wake_fd   = -1;
    ctx->next_id   = 1;
    atomic_init(&ctx->running, 1);
    atomic_init(&ctx->poll_busy, 0);
    pthread_mutex_init(&ctx->tree_lock, NULL);
    pthread_mutex_init(&ctx->hook_lock, NULL);

    ctx->root = calloc(1, sizeof(fluxipc_node_t));
    if (!ctx->root) {
        pthread_mutex_destroy(&ctx->hook_lock);
        pthread_mutex_destroy(&ctx->tree_lock);
        free(ctx);
        return NULL;
    }
    ctx->root->name[0]      = '/';
    ctx->root->name[1]      = '\0';
    ctx->root->full_path[0] = '/';
    ctx->root->full_path[1] = '\0';
    return ctx;
}

static void ctx_free(fluxipc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->sock_path[0]) unlink(ctx->sock_path);
    rmdir(ctx->run_dir);   /* only succeeds when the runtime dir is empty */

    pthread_mutex_lock(&ctx->tree_lock);
    tree_destroy(ctx->root);
    ctx->root = NULL;
    pthread_mutex_unlock(&ctx->tree_lock);

    if (ctx->wake_fd >= 0) close(ctx->wake_fd);
    free(ctx->ipc_out);
    pthread_mutex_destroy(&ctx->hook_lock);
    pthread_mutex_destroy(&ctx->tree_lock);
    free(ctx);
}

/* ─── fluxipc_server_init ─────────────────────────────────────────────────── */

int fluxipc_server_init(const char *prog_name)
{
    if (g_ctx) return -EALREADY;
    if (!valid_prog_name(prog_name)) return -EINVAL;

    fluxipc_ctx_t *ctx = ctx_alloc(prog_name);
    if (!ctx) return -ENOMEM;

    /* Wake select() for fluxipc_wakeup()/fluxipc_stop() without polling. */
    ctx->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    conn_pool_init();

    /* /run/user/<uid>/ then /run/user/<uid>/<prog>-fluxipc/ */
    {
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", ctx->run_dir);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; mkdir(parent, 0755); }
    }
    if (mkdir(ctx->run_dir, 0755) < 0 && errno != EEXIST)
        perror("fluxipc: mkdir run_dir");
    chmod(ctx->run_dir, 0755);

    int rc = sock_server_create(ctx);
    if (rc < 0) {
        ctx_free(ctx);
        return rc;
    }

    g_ctx = ctx;
    fluxipc_static_apply();   /* install FLUXIPC_STATIC() endpoints, if any */
    return 0;
}

/* ─── Tree registration ───────────────────────────────────────────────────── */

fluxipc_node_t *fluxipc_tree_root(void)
{
    return g_ctx ? g_ctx->root : NULL;
}

static int valid_segment(const char *name)
{
    return name && name[0] && !strchr(name, '/') &&
           strlen(name) < FLUXIPC_NAME_MAX;
}

fluxipc_node_t *fluxipc_node_mkdir(fluxipc_node_t *parent, const char *name)
{
    if (!g_ctx || !parent || !valid_segment(name) || node_is_leaf(parent)) {
        errno = EINVAL;
        return NULL;
    }
    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *n = tree_mkdir(parent, name);
    pthread_mutex_unlock(&g_ctx->tree_lock);
    return n;   /* errno set by tree_mkdir on failure */
}

fluxipc_node_t *fluxipc_node_leaf(fluxipc_node_t *parent,
                                  const char *name,
                                  const fluxipc_method_t *method)
{
    if (!g_ctx || !parent || !valid_segment(name) ||
        !method || !method->ipc) {
        errno = EINVAL;
        return NULL;
    }
    if (node_is_leaf(parent)) { errno = EINVAL; return NULL; }

    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *n = tree_leaf(parent, name, method);
    if (n) n->id = g_ctx->next_id++;
    pthread_mutex_unlock(&g_ctx->tree_lock);
    return n;
}

/* ─── Path-based registration ─────────────────────────────────────────────── */

static void free_subtree(fluxipc_node_t *node);   /* defined below */

fluxipc_node_t *fluxipc_register_method(const char *path,
                                        const fluxipc_method_t *method)
{
    if (!g_ctx || !path || path[0] != '/' || !method || !method->ipc ||
        strlen(path) >= FLUXIPC_PATH_MAX) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&g_ctx->tree_lock);

    fluxipc_node_t *cur           = g_ctx->root;
    fluxipc_node_t *first_created = NULL;
    const char     *p             = path;
    int             err           = EINVAL;

    while (*p) {
        while (*p == '/') p++;              /* tolerate repeated slashes */
        if (!*p) break;

        const char *slash = strchr(p, '/');
        size_t      len   = slash ? (size_t)(slash - p) : strlen(p);
        if (len >= FLUXIPC_NAME_MAX) goto fail;

        char seg[FLUXIPC_NAME_MAX];
        memcpy(seg, p, len);
        seg[len] = '\0';

        if (!slash) {                       /* final segment → leaf */
            fluxipc_node_t *leaf = tree_find_child(cur, seg);
            if (leaf) {
                if (!node_is_leaf(leaf)) { err = EEXIST; goto fail; }
                tree_leaf_merge(leaf, method);   /* fill still-empty metadata */
            } else {
                leaf = tree_leaf(cur, seg, method);
                if (!leaf) { err = ENOMEM; goto fail; }
                leaf->id = g_ctx->next_id++;
            }
            pthread_mutex_unlock(&g_ctx->tree_lock);
            return leaf;
        }

        fluxipc_node_t *next = tree_find_child(cur, seg);
        if (next && node_is_leaf(next)) { err = EEXIST; goto fail; }
        if (!next) {
            next = tree_mkdir(cur, seg);
            if (!next) { err = ENOMEM; goto fail; }
            if (!first_created) first_created = next;
        }
        cur = next;
        p   = slash + 1;
    }

fail:
    /* Roll back containers created by this call (path was invalid or the
     * leaf name collided); pre-existing ancestors are never pruned. */
    if (first_created) free_subtree(first_created);
    pthread_mutex_unlock(&g_ctx->tree_lock);
    errno = err;
    return NULL;
}

int fluxipc_register_all(const fluxipc_endpoint_t *endpoints, size_t count)
{
    if (!endpoints && count) return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        if (!fluxipc_register_method(endpoints[i].path, &endpoints[i].method))
            return -errno;
    }
    return 0;
}

/*
 * Classic main-branch entry point.  The shared-memory @p slot_sz is accepted
 * for source compatibility and ignored; @p usage maps to the leaf summary.
 * The name is parenthesised so the fluxipc_register() dispatch macro does not
 * rewrite this definition.
 */
fluxipc_node_t *(fluxipc_register)(const char *path, const char *usage,
                                   size_t slot_sz,
                                   fluxipc_handler_fn handler, void *data)
{
    (void)slot_sz;
    fluxipc_method_t m = {
        .ipc     = handler,
        .data    = data,
        .summary = usage,
    };
    return fluxipc_register_method(path, &m);
}

void fluxipc_unregister(fluxipc_node_t *node)
{
    fluxipc_node_remove(node);
}

/* ─── Compile-time (FLUXIPC_STATIC) registry ──────────────────────────────── */

static fluxipc_entry_t *g_static_head = NULL;
static fluxipc_entry_t *g_static_tail = NULL;
static pthread_mutex_t  g_static_lock = PTHREAD_MUTEX_INITIALIZER;

void fluxipc_static_add(fluxipc_entry_t *entry)
{
    if (!entry) return;
    entry->next = NULL;
    pthread_mutex_lock(&g_static_lock);
    if (g_static_tail) g_static_tail->next = entry;
    else               g_static_head = entry;
    g_static_tail = entry;
    pthread_mutex_unlock(&g_static_lock);
}

/* Re-register every compile-time entry into the freshly built tree.  The list
 * is kept so a later server_init() after destroy() sees the endpoints again. */
static void fluxipc_static_apply(void)
{
    pthread_mutex_lock(&g_static_lock);
    for (fluxipc_entry_t *e = g_static_head; e; e = e->next)
        fluxipc_register(e->path, e->usage, 0, e->handler, e->data);
    pthread_mutex_unlock(&g_static_lock);
}

fluxipc_node_t *fluxipc_node_find(fluxipc_node_t *parent, const char *name)
{
    if (!g_ctx || !parent || !name) return NULL;
    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *n = tree_find_child(parent, name);
    pthread_mutex_unlock(&g_ctx->tree_lock);
    return n;
}

/* ─── Removal ─────────────────────────────────────────────────────────────── */

static void unlink_from_parent(fluxipc_node_t *node)
{
    fluxipc_node_t *p = node->parent;
    if (!p) return;
    if (p->children == node) {
        p->children = node->next_sibling;
    } else {
        for (fluxipc_node_t *prev = p->children; prev; prev = prev->next_sibling)
            if (prev->next_sibling == node) { prev->next_sibling = node->next_sibling; break; }
    }
    node->next_sibling = NULL;
}

static void free_subtree(fluxipc_node_t *node)
{
    while (node->children)
        free_subtree(node->children);

    unlink_from_parent(node);
    tree_node_free(node);
}

void fluxipc_node_remove(fluxipc_node_t *node)
{
    if (!g_ctx || !node || node == g_ctx->root) return;

    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *p = node->parent;
    free_subtree(node);

    /* prune now-empty ancestors */
    while (p && p->parent && !node_is_leaf(p) && !p->children) {
        fluxipc_node_t *gp = p->parent;
        unlink_from_parent(p);
        tree_node_free(p);
        p = gp;
    }
    pthread_mutex_unlock(&g_ctx->tree_lock);
}

/* ─── Leaf listing (built-in on every node) ───────────────────────────────── */

static int walk_leaves(fluxipc_node_t *node, fluxipc_leaf_cb cb, void *user,
                       int *stop)
{
    int count = 0;
    if (node_is_leaf(node)) {
        count++;
        if (cb(node, user)) *stop = 1;
        return count;
    }
    for (fluxipc_node_t *c = node->children; c && !*stop; c = c->next_sibling)
        count += walk_leaves(c, cb, user, stop);
    return count;
}

int fluxipc_node_leaves(fluxipc_node_t *node, fluxipc_leaf_cb cb, void *user)
{
    if (!g_ctx || !node || !cb) return -EINVAL;

    int stop = 0;
    pthread_mutex_lock(&g_ctx->tree_lock);
    int count = walk_leaves(node, cb, user, &stop);
    pthread_mutex_unlock(&g_ctx->tree_lock);
    return count;
}

/* ─── Text manifest (all leaves under a node) ─────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    failed;
} strbuf_t;

static int sb_reserve(strbuf_t *b, size_t extra)
{
    if (b->failed) return -ENOMEM;
    if (extra > SIZE_MAX - b->len - 1) {
        b->failed = 1;
        return -ENOMEM;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;

    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    char *nb = realloc(b->buf, cap);
    if (!nb) { b->failed = 1; return -ENOMEM; }
    b->buf = nb;
    b->cap = cap;
    return 0;
}

static void sb_append(strbuf_t *b, const char *s, size_t len)
{
    if (len == 0 || sb_reserve(b, len) < 0) return;
    memcpy(b->buf + b->len, s, len);
    b->len += len;
    b->buf[b->len] = '\0';
}

/** Append @p s with protocol delimiters replaced by spaces. */
static void sb_append_field(strbuf_t *b, const char *s)
{
    if (!s) return;
    size_t start = b->len;
    size_t n = strlen(s);
    sb_append(b, s, n);
    if (b->failed) return;
    for (size_t i = start; i < b->len; i++)
        if (b->buf[i] == '\t' || b->buf[i] == '\n' || b->buf[i] == '\r')
            b->buf[i] = ' ';
}

static int manifest_leaf_cb(const fluxipc_node_t *leaf, void *user)
{
    strbuf_t *b = user;

    char idbuf[16];
    int  n = snprintf(idbuf, sizeof(idbuf), "%u", leaf->id);
    if (n > 0) sb_append(b, idbuf, (size_t)n);
    sb_append(b, "\t", 1);
    sb_append_field(b, leaf->full_path);
    sb_append(b, "\t", 1);
    sb_append_field(b, leaf->method.summary ? leaf->method.summary : "");
    sb_append(b, "\n", 1);

    return b->failed ? 1 : 0;
}

char *fluxipc_build_manifest(fluxipc_node_t *node, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!node) return NULL;

    strbuf_t b = { 0 };
    fluxipc_node_leaves(node, manifest_leaf_cb, &b);
    if (b.failed) { free(b.buf); return NULL; }

    if (!b.buf) {
        b.buf = strdup("");
        if (!b.buf) return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.buf;
}

/* ─── Accessors ───────────────────────────────────────────────────────────── */

const char *fluxipc_node_path(const fluxipc_node_t *n)
{
    return n ? n->full_path : NULL;
}

const char *fluxipc_node_name(const fluxipc_node_t *n)
{
    return n ? n->name : NULL;
}

const fluxipc_method_t *fluxipc_node_method(const fluxipc_node_t *n)
{
    return node_is_leaf(n) ? &n->method : NULL;
}

int fluxipc_node_is_leaf(const fluxipc_node_t *n)
{
    return node_is_leaf(n);
}

/* ─── Dispatch ────────────────────────────────────────────────────────────── */

int fluxipc_dispatch_ipc(fluxipc_ctx_t *ctx, const char *path,
                         int argc, char **argv,
                         void *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!ctx || !path) return -EINVAL;

    fluxipc_handler_fn handler = NULL;
    void              *data    = NULL;

    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find_path(ctx->root, path);
    if (node_is_leaf(node)) {
        handler = node->method.ipc;
        data    = node->method.data;
    }
    pthread_mutex_unlock(&ctx->tree_lock);

    pthread_mutex_lock(&ctx->hook_lock);
    fluxipc_request_hook_fn pre  = ctx->pre_hook;
    fluxipc_request_hook_fn post = ctx->post_hook;
    void                   *user = ctx->hook_user;
    pthread_mutex_unlock(&ctx->hook_lock);

    fluxipc_event_t ev = {
        .path    = path,
        .matched = handler ? 1 : 0,
    };
    if (pre) pre(&ev, user);

    int rc = handler ? handler(data, argc, argv, out, out_cap, out_len)
                     : -ENOENT;

    ev.status  = rc;
    ev.raw     = out;
    ev.raw_len = out_len ? *out_len : 0;
    if (post) post(&ev, user);
    return rc;
}

/* ─── fluxipc_set_request_hooks ────────────────────────────────────────────── */

void fluxipc_set_request_hooks(fluxipc_request_hook_fn pre,
                               fluxipc_request_hook_fn post, void *user)
{
    if (!g_ctx) return;
    pthread_mutex_lock(&g_ctx->hook_lock);
    g_ctx->pre_hook  = pre;
    g_ctx->post_hook = post;
    g_ctx->hook_user = user;
    pthread_mutex_unlock(&g_ctx->hook_lock);
}

/* ─── Poll / run / stop / destroy ─────────────────────────────────────────── */

int fluxipc_poll_timeout(int timeout_ms)
{
    if (!g_ctx) return -EINVAL;
    if (!atomic_load_explicit(&g_ctx->running, memory_order_acquire))
        return -EINVAL;

    /* Enforce the single-poller contract, even if the host misuses the API. */
    if (atomic_exchange_explicit(&g_ctx->poll_busy, 1,
                                 memory_order_acquire))
        return -EBUSY;

    int rc = conn_poll(g_ctx, timeout_ms);

    atomic_store_explicit(&g_ctx->poll_busy, 0, memory_order_release);
    return rc;
}

int fluxipc_poll(void)
{
    return fluxipc_poll_timeout(-1);
}

int fluxipc_run(void)
{
    if (!g_ctx) return -EINVAL;

    for (;;) {
        if (!atomic_load_explicit(&g_ctx->running, memory_order_acquire))
            return 0;

        int rc = fluxipc_poll_timeout(-1);
        if (rc < 0) {
            if (!atomic_load_explicit(&g_ctx->running, memory_order_acquire))
                return 0;
            return rc;
        }
    }
}

int fluxipc_is_running(void)
{
    return g_ctx &&
           atomic_load_explicit(&g_ctx->running, memory_order_acquire);
}

void fluxipc_wakeup(void)
{
    if (!g_ctx || g_ctx->wake_fd < 0) return;
    uint64_t one = 1;
    ssize_t  n = write(g_ctx->wake_fd, &one, sizeof(one));
    (void)n;
}

void fluxipc_stop(void)
{
    if (!g_ctx) return;
    atomic_store_explicit(&g_ctx->running, 0, memory_order_release);
    fluxipc_wakeup();
}

void fluxipc_destroy(void)
{
    if (!g_ctx) return;
    fluxipc_ctx_t *ctx = g_ctx;
    g_ctx = NULL;

    atomic_store_explicit(&ctx->running, 0, memory_order_release);
    conn_pool_reset();
    sock_server_close(ctx);
    ctx_free(ctx);
}

/* ─── List endpoints (socket introspection) ───────────────────────────────── */

int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata)
{
    if (!cb || !valid_prog_name(prog_name)) return -EINVAL;

    char sock_path[FLUXIPC_PATH_MAX];
    snprintf(sock_path, sizeof(sock_path),
             "/run/user/%d/%s-fluxipc/%s.sock", getuid(), prog_name, prog_name);

    char  *out = NULL;
    size_t out_len = 0;
    int rc = sock_call_alloc(sock_path, FLUXIPC_OP_LIST, "", 0, NULL,
                             &out, &out_len);
    if (rc < 0) return rc;
    if (!out) return 0;

    /* Parse `<id>\t<path>\t<summary>\n` in place: the buffer is ours. */
    char *p   = out;
    char *end = out + out_len;
    while (p < end) {
        char *nl  = memchr(p, '\n', (size_t)(end - p));
        char *eol = nl ? nl : end;
        *eol = '\0';

        char *tab1 = memchr(p, '\t', (size_t)(eol - p));
        if (tab1) {
            *tab1 = '\0';
            char *tab2 = memchr(tab1 + 1, '\t',
                                (size_t)(eol - (tab1 + 1)));
            if (tab2) {
                *tab2 = '\0';
                char *num_end = NULL;
                unsigned long id = strtoul(p, &num_end, 10);
                if (num_end != p && num_end && *num_end == '\0')
                    cb(tab1 + 1, (uint32_t)id, tab2 + 1, userdata);
            }
        }

        p = nl ? nl + 1 : end;
    }

    free(out);
    return 0;
}

/* ─── Usage ───────────────────────────────────────────────────────────────── */

static int usage_leaf_cb(const fluxipc_node_t *leaf, void *user)
{
    (void)user;
    const fluxipc_method_t *m = fluxipc_node_method(leaf);
    printf("  %-40s  %s\n", leaf->full_path,
           (m && m->summary && m->summary[0]) ? m->summary : "(no summary)");
    return 0;
}

void fluxipc_usage(const char *prog)
{
    printf("Usage: %s <ipc-path> [args...]\n\n", prog ? prog : "fluxipc");
    printf("Registered endpoints:\n");
    if (g_ctx && g_ctx->root) {
        if (fluxipc_node_leaves(g_ctx->root, usage_leaf_cb, NULL) == 0)
            printf("  (none)\n");
    } else {
        printf("  (none)\n");
    }
    printf("\n");
}
