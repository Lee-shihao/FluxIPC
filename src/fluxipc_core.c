/**
 * fluxipc_core.c – library entry points
 *
 * Owns the global context, the namespace tree, dynamic registration, the
 * unified dispatch path, and the lifecycle / event loop.
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

/* ─── Method <-> mask helpers ─────────────────────────────────────────────── */

int fluxipc_method_to_mask(const char *method)
{
    if (!method) return 0;
    if (strcmp(method, "GET")     == 0) return FLUXIPC_M_GET;
    if (strcmp(method, "POST")    == 0) return FLUXIPC_M_POST;
    if (strcmp(method, "PUT")     == 0) return FLUXIPC_M_PUT;
    if (strcmp(method, "PATCH")   == 0) return FLUXIPC_M_PATCH;
    if (strcmp(method, "DELETE")  == 0) return FLUXIPC_M_DELETE;
    return 0;
}

const char *fluxipc_mask_to_method(uint32_t bit)
{
    switch (bit) {
        case FLUXIPC_M_GET:    return "GET";
        case FLUXIPC_M_POST:   return "POST";
        case FLUXIPC_M_PUT:    return "PUT";
        case FLUXIPC_M_PATCH:  return "PATCH";
        case FLUXIPC_M_DELETE: return "DELETE";
        default:               return NULL;
    }
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
    ctx->http_fd   = -1;
    ctx->next_id   = 1;
    ctx->running   = 1;
    pthread_mutex_init(&ctx->tree_lock, NULL);

    ctx->root = calloc(1, sizeof(fluxipc_node_t));
    if (!ctx->root) { free(ctx); return NULL; }
    ctx->root->name[0]      = '/';
    ctx->root->name[1]      = '\0';
    ctx->root->full_path[0] = '/';
    ctx->root->full_path[1] = '\0';
    return ctx;
}

/* ─── fluxipc_server_init ─────────────────────────────────────────────────── */

int fluxipc_server_init(const char *prog_name, uint16_t http_port)
{
    if (g_ctx) return -EALREADY;
    if (!prog_name || !prog_name[0]) return -EINVAL;

    fluxipc_ctx_t *ctx = ctx_alloc(prog_name);
    if (!ctx) return -ENOMEM;

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
    if (rc < 0) { free(ctx->root); free(ctx); return rc; }

    g_ctx = ctx;

    if (http_port > 0) {
        rc = fluxipc_http_start(ctx, http_port);
        if (rc < 0)
            fprintf(stderr, "fluxipc: HTTP start on :%u failed: %d "
                            "(continuing without HTTP)\n", http_port, rc);
        /* Non-fatal: the server works fine without HTTP. */
    }

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
    if (!g_ctx || !parent || !valid_segment(name) || parent->is_leaf) {
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
        !method || (!method->ipc && !method->http)) {
        errno = EINVAL;
        return NULL;
    }
    if (parent->is_leaf) { errno = EINVAL; return NULL; }

    pthread_mutex_lock(&g_ctx->tree_lock);
    fluxipc_node_t *n = tree_leaf(parent, name, method);
    if (n) n->id = g_ctx->next_id++;
    pthread_mutex_unlock(&g_ctx->tree_lock);

    if (!n) { errno = EEXIST; return NULL; }
    return n;
}

/* ─── Path-based registration ─────────────────────────────────────────────── */

static void free_subtree(fluxipc_node_t *node);   /* defined below */

fluxipc_node_t *fluxipc_register(const char *path,
                                 const fluxipc_method_t *method)
{
    if (!g_ctx || !path || path[0] != '/' || !method ||
        (!method->ipc && !method->http) ||
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

        if (!valid_segment(seg)) goto fail;

        if (!slash) {                       /* final segment → leaf */
            fluxipc_node_t *leaf = tree_leaf(cur, seg, method);
            if (!leaf) { err = EEXIST; goto fail; }
            leaf->id = g_ctx->next_id++;
            pthread_mutex_unlock(&g_ctx->tree_lock);
            return leaf;
        }

        fluxipc_node_t *next = tree_find_child(cur, seg);
        if (next && next->is_leaf) { err = EEXIST; goto fail; }
        if (!next) {
            next = tree_mkdir(cur, seg);
            if (!next) { err = ENOMEM; goto fail; }   /* only alloc can fail here */
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
        if (!fluxipc_register(endpoints[i].path, &endpoints[i].method))
            return -errno;
    }
    return 0;
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
    while (p && p->parent && !p->is_leaf && !p->children) {
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
    if (node->is_leaf) {
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

/* ─── Manifest (all leaves under a node) ──────────────────────────────────── */

typedef struct {
    cJSON      *arr;
    const char *base;
    int         recursive;
    int         verbose;
} manifest_ctx_t;

static int manifest_leaf_cb(const fluxipc_node_t *leaf, void *user)
{
    manifest_ctx_t *mc = user;

    if (!mc->recursive) {
        const char *rel = (strcmp(mc->base, "/") == 0)
                            ? leaf->full_path + 1
                            : leaf->full_path + strlen(mc->base) + 1;
        if (strchr(rel, '/')) return 0;   /* deeper than one level */
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) return 0;
    cJSON_AddStringToObject(o, "path", leaf->full_path);
    cJSON_AddStringToObject(o, "name", leaf->name);
    cJSON_AddNumberToObject(o, "id", leaf->id);

    cJSON *methods = cJSON_AddArrayToObject(o, "methods");
    for (int i = 0; i < 5 && methods; i++) {
        uint32_t bit = 1u << i;
        if (leaf->method.methods & bit) {
            const char *nm = fluxipc_mask_to_method(bit);
            if (nm) cJSON_AddItemToArray(methods, cJSON_CreateString(nm));
        }
    }
    if (leaf->method.summary)
        cJSON_AddStringToObject(o, "summary", leaf->method.summary);
    if (mc->verbose && leaf->method.description)
        cJSON_AddStringToObject(o, "description", leaf->method.description);
    if (mc->verbose && leaf->method.schema) {
        cJSON *schema = cJSON_Parse(leaf->method.schema);
        if (schema) cJSON_AddItemToObject(o, "schema", schema);
    }
    cJSON_AddItemToArray(mc->arr, o);
    return 0;
}

cJSON *fluxipc_build_manifest(fluxipc_node_t *node, int recursive, int verbose)
{
    if (!node) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "path", node->full_path);
    cJSON_AddStringToObject(root, "kind", "collection");
    cJSON *arr = cJSON_AddArrayToObject(root, "leaves");
    if (!arr) { cJSON_Delete(root); return NULL; }

    manifest_ctx_t mc = { arr, node->full_path, recursive, verbose };
    fluxipc_node_leaves(node, manifest_leaf_cb, &mc);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
    return root;
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
    return (n && n->is_leaf) ? &n->method : NULL;
}

int fluxipc_node_is_leaf(const fluxipc_node_t *n)
{
    return n && n->is_leaf;
}

/* ─── Dispatch ────────────────────────────────────────────────────────────── */

int fluxipc_dispatch_ipc(fluxipc_ctx_t *ctx, const char *path,
                         int argc, char **argv,
                         char *out, size_t out_cap, size_t *out_len)
{
    fluxipc_handler_fn handler = NULL;
    void              *data    = NULL;

    if (out_len) *out_len = 0;

    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find_path(ctx->root, path);
    if (node && node->is_leaf) {
        handler = node->method.ipc;
        data    = node->method.data;
    }
    pthread_mutex_unlock(&ctx->tree_lock);

    fluxipc_event_t ev = {
        .transport = "ipc",
        .method    = "CALL",
        .path      = path,
        .matched   = handler ? 1 : 0,
    };
    if (ctx->pre_hook) ctx->pre_hook(&ev, ctx->hook_user);

    int rc = handler ? handler(data, argc, argv, out, out_cap, out_len)
                     : -ENOENT;

    ev.status  = rc;
    ev.raw     = out;
    ev.raw_len = out_len ? *out_len : 0;
    if (ctx->post_hook) ctx->post_hook(&ev, ctx->hook_user);
    return rc;
}

int fluxipc_dispatch_http(fluxipc_ctx_t *ctx, fluxipc_http_request_t *req,
                          fluxipc_http_response_t *resp)
{
    fluxipc_http_handler_fn handler = NULL;
    void                   *data    = NULL;

    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find_path(ctx->root, req->path);
    req->node = node;
    if (node && node->is_leaf) {
        handler   = node->method.http;
        data      = node->method.data;
        req->data = data;
    }
    pthread_mutex_unlock(&ctx->tree_lock);

    fluxipc_event_t ev = {
        .transport = "http",
        .method    = req->method,
        .path      = req->path,
        .matched   = handler ? 1 : 0,
    };
    if (ctx->pre_hook) ctx->pre_hook(&ev, ctx->hook_user);

    int rc = handler ? handler(data, req, resp) : -ENOSYS;

    ev.status = rc;
    ev.json   = resp->json;
    if (ctx->post_hook) ctx->post_hook(&ev, ctx->hook_user);
    return rc;
}

/* ─── fluxipc_set_request_hooks ────────────────────────────────────────────── */

void fluxipc_set_request_hooks(fluxipc_request_hook_fn pre,
                               fluxipc_request_hook_fn post, void *user)
{
    if (!g_ctx) return;
    g_ctx->pre_hook  = pre;
    g_ctx->post_hook = post;
    g_ctx->hook_user = user;
}

/* ─── Poll / stop / destroy ───────────────────────────────────────────────── */

int fluxipc_poll(void *unused)
{
    (void)unused;
    if (!g_ctx || !g_ctx->running) return -EINVAL;
    return conn_poll(g_ctx);
}

void fluxipc_stop(void)  { if (g_ctx) g_ctx->running = 0; }

void fluxipc_destroy(void)
{
    if (!g_ctx) return;
    fluxipc_ctx_t *ctx = g_ctx; g_ctx = NULL;
    conn_pool_reset();
    fluxipc_http_stop(ctx);
    sock_server_close(ctx);
    rmdir(ctx->run_dir);   /* remove the now-empty per-app runtime dir */
    pthread_mutex_lock(&ctx->tree_lock);
    tree_destroy(ctx->root);
    ctx->root = NULL;
    pthread_mutex_unlock(&ctx->tree_lock);
    pthread_mutex_destroy(&ctx->tree_lock);
    free(ctx);
}

/* ─── List endpoints (socket introspection) ───────────────────────────────── */

int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata)
{
    if (!prog_name || !prog_name[0] || !cb) return -EINVAL;

    char sock_path[FLUXIPC_PATH_MAX];
    snprintf(sock_path, sizeof(sock_path),
             "/run/user/%d/%s-fluxipc/%s.sock", getuid(), prog_name, prog_name);

    char *out = NULL;
    size_t out_len = 0;
    int rc = sock_call_alloc(sock_path, FLUXIPC_OP_LIST, "", 0, NULL,
                             &out, &out_len);
    if (rc < 0) return rc;

    cJSON *manifest = cJSON_ParseWithLength(out, out_len);
    free(out);
    if (!manifest) return -EIO;

    const cJSON *leaves = cJSON_GetObjectItemCaseSensitive(manifest, "leaves");
    const cJSON *leaf;
    cJSON_ArrayForEach(leaf, leaves) {
        const cJSON *p  = cJSON_GetObjectItemCaseSensitive(leaf, "path");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(leaf, "id");
        const cJSON *u  = cJSON_GetObjectItemCaseSensitive(leaf, "summary");
        if (cJSON_IsString(p) && p->valuestring)
            cb(p->valuestring,
               cJSON_IsNumber(id) ? (uint32_t)id->valuedouble : 0,
               (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "",
               userdata);
    }
    cJSON_Delete(manifest);
    return 0;
}

/* ─── Usage ───────────────────────────────────────────────────────────────── */

static int usage_leaf_cb(const fluxipc_node_t *leaf, void *user)
{
    (void)user;
    const fluxipc_method_t *m = fluxipc_node_method(leaf);
    printf("  %-40s  %s\n", leaf->full_path,
           (m && m->summary && m->summary[0]) ? m->summary : "(no description)");
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
