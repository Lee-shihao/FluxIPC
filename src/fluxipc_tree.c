/**
 * fluxipc_tree.c – namespace/object tree management
 *
 * The tree mirrors the UNIX filesystem hierarchy.  Internal nodes are
 * containers; leaf nodes carry a method (metadata + handler).  Registration is
 * performed by walking node by node, never with a full path string.
 *
 * All operations are O(depth × siblings), which is fine for typical IPC
 * namespaces (depth ≤ 8, fanout ≤ 64).
 */

#include "fluxipc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ─── Allocation helpers ──────────────────────────────────────────────────── */

static fluxipc_node_t *node_alloc(const char *name)
{
    fluxipc_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    snprintf(n->name, FLUXIPC_NAME_MAX, "%s", name);
    return n;
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

/** Free a node's owned memory (method strings first). */
void tree_node_free(fluxipc_node_t *n)
{
    if (!n) return;
    free((char *)n->method.summary);
    free((char *)n->method.description);
    free((char *)n->method.schema);
    free(n);
}

/* ─── Path helpers ────────────────────────────────────────────────────────── */

/**
 * Split an absolute path into components.
 * tokens[] points into a copy of path owned by the caller (free *copy_out).
 * Returns token count, or -1 on error.
 */
static int path_split(const char *path, char **tokens, int max_tokens,
                      char **copy_out)
{
    if (!path || path[0] != '/') return -1;

    char *buf = strdup(path);
    if (!buf) return -1;

    int n = 0;
    char *p = buf + 1; /* skip leading '/' */
    while (*p && n < max_tokens) {
        tokens[n++] = p;
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = '\0';
        p = slash + 1;
    }
    *copy_out = buf;
    return n;
}

/* ─── Lookup ──────────────────────────────────────────────────────────────── */

fluxipc_node_t *tree_find_child(fluxipc_node_t *parent, const char *name)
{
    if (!parent || !name) return NULL;
    for (fluxipc_node_t *c = parent->children; c; c = c->next_sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

fluxipc_node_t *tree_find_path(fluxipc_node_t *root, const char *path)
{
    if (!root || !path || path[0] != '/') return NULL;
    if (strcmp(path, "/") == 0) return root;

    char *tokens[64];
    char *buf;
    int   n = path_split(path, tokens, 64, &buf);
    if (n < 0) return NULL;

    fluxipc_node_t *cur = root;
    for (int i = 0; i < n && cur; i++)
        cur = tree_find_child(cur, tokens[i]);

    free(buf);
    return cur;
}

/* ─── Creation ────────────────────────────────────────────────────────────── */

/** Build full_path from parent + name, returning the node. */
static void set_full_path(fluxipc_node_t *node, fluxipc_node_t *parent,
                          const char *name)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    if (!parent || strcmp(parent->full_path, "/") == 0)
        snprintf(node->full_path, FLUXIPC_PATH_MAX, "/%s", name);
    else
        snprintf(node->full_path, FLUXIPC_PATH_MAX, "%s/%s",
                 parent->full_path, name);
#pragma GCC diagnostic pop
}

fluxipc_node_t *tree_mkdir(fluxipc_node_t *parent, const char *name)
{
    if (!parent || !name || !name[0]) { errno = EINVAL; return NULL; }

    fluxipc_node_t *found = tree_find_child(parent, name);
    if (found) {
        /* a leaf and a container share one name slot in a directory */
        if (found->is_leaf) { errno = EEXIST; return NULL; }
        return found;                 /* idempotent get-or-create */
    }

    fluxipc_node_t *n = node_alloc(name);
    if (!n) { errno = ENOMEM; return NULL; }
    n->parent = parent;
    n->is_leaf = 0;
    set_full_path(n, parent, name);

    n->next_sibling = parent->children;
    parent->children = n;
    return n;
}

fluxipc_node_t *tree_leaf(fluxipc_node_t *parent, const char *name,
                          const fluxipc_method_t *method)
{
    if (!parent || !name || !name[0] || !method ||
        (!method->ipc && !method->http))
        return NULL;

    if (tree_find_child(parent, name)) return NULL;   /* EEXIST */

    fluxipc_node_t *n = node_alloc(name);
    if (!n) return NULL;

    n->method.ipc         = method->ipc;
    n->method.http        = method->http;
    n->method.data        = method->data;
    n->method.methods     = method->methods ? method->methods
                                            : FLUXIPC_M_DEFAULT;
    n->method.summary     = dup_or_null(method->summary);
    n->method.description = dup_or_null(method->description);
    n->method.schema      = dup_or_null(method->schema);

    n->parent = parent;
    n->is_leaf = 1;
    set_full_path(n, parent, name);

    n->next_sibling = parent->children;
    parent->children = n;
    return n;
}

/*
 * Merge @p src into an already-registered leaf.  This lets the IPC and HTTP
 * sides of one endpoint be registered independently (and in either order):
 * only fields that are still empty are filled, so the first registration of a
 * given handler/metadata value is the one that is kept.  The HTTP method mask
 * is owned by whichever call first attaches an HTTP handler.
 */
void tree_leaf_merge(fluxipc_node_t *node, const fluxipc_method_t *src)
{
    if (!node->method.ipc  && src->ipc)  node->method.ipc  = src->ipc;

    int had_http = node->method.http != NULL;
    if (!node->method.http && src->http) node->method.http = src->http;
    if (src->http && src->methods && !had_http)
        node->method.methods = src->methods;   /* the HTTP side owns the mask */

    if (!node->method.data && src->data) node->method.data = src->data;

    if (!node->method.summary     && src->summary)
        node->method.summary     = dup_or_null(src->summary);
    if (!node->method.description && src->description)
        node->method.description = dup_or_null(src->description);
    if (!node->method.schema      && src->schema)
        node->method.schema      = dup_or_null(src->schema);
}

/* ─── Destroy ─────────────────────────────────────────────────────────────── */

void tree_destroy(fluxipc_node_t *node)
{
    if (!node) return;
    fluxipc_node_t *child = node->children;
    while (child) {
        fluxipc_node_t *next = child->next_sibling;
        tree_destroy(child);
        child = next;
    }
    tree_node_free(node);
}
