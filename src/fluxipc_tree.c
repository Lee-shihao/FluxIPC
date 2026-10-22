/**
 * fluxipc_tree.c – namespace/object tree management
 *
 * The tree mirrors the UNIX filesystem hierarchy.  Internal nodes are
 * containers; leaf nodes carry an IPC method (metadata + handler).
 * Registration is performed by walking node by node, never with a full path
 * string.
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

/** Free a node's owned memory. */
void tree_node_free(fluxipc_node_t *n)
{
    if (!n) return;
    free((char *)n->method.summary);
    free(n);
}

/* ─── Lookup ──────────────────────────────────────────────────────────────── */

static fluxipc_node_t *tree_find_child_n(fluxipc_node_t *parent,
                                         const char *name, size_t len)
{
    if (!parent || !name || len >= FLUXIPC_NAME_MAX) return NULL;
    for (fluxipc_node_t *c = parent->children; c; c = c->next_sibling)
        if (c->name[len] == '\0' && memcmp(c->name, name, len) == 0)
            return c;
    return NULL;
}

fluxipc_node_t *tree_find_child(fluxipc_node_t *parent, const char *name)
{
    return name ? tree_find_child_n(parent, name, strlen(name)) : NULL;
}

fluxipc_node_t *tree_find_path(fluxipc_node_t *root, const char *path)
{
    if (!root || !path || path[0] != '/') return NULL;
    if (path[1] == '\0') return root;

    /* Walk components in place: no path copy and no token array. */
    fluxipc_node_t *cur = root;
    const char *p = path + 1;
    while (*p && cur) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        cur = tree_find_child_n(cur, p, len);
        if (!cur || !slash) break;
        p = slash + 1;
    }
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
        if (node_is_leaf(found)) { errno = EEXIST; return NULL; }
        return found;                 /* idempotent get-or-create */
    }

    fluxipc_node_t *n = node_alloc(name);
    if (!n) { errno = ENOMEM; return NULL; }
    n->parent = parent;
    set_full_path(n, parent, name);

    n->next_sibling = parent->children;
    parent->children = n;
    return n;
}

fluxipc_node_t *tree_leaf(fluxipc_node_t *parent, const char *name,
                          const fluxipc_method_t *method)
{
    if (!parent || !name || !name[0] || !method || !method->ipc) {
        errno = EINVAL;
        return NULL;
    }
    if (tree_find_child(parent, name)) { errno = EEXIST; return NULL; }

    fluxipc_node_t *n = node_alloc(name);
    if (!n) { errno = ENOMEM; return NULL; }

    n->method.ipc     = method->ipc;
    n->method.data    = method->data;
    n->method.summary = dup_or_null(method->summary);

    n->parent = parent;
    set_full_path(n, parent, name);

    n->next_sibling = parent->children;
    parent->children = n;
    return n;
}

/*
 * Merge @p src into an already-registered leaf.  Only fields that are still
 * empty are filled, so the first registration of a handler/metadata value is
 * the one that is kept.  This lets a classic fluxipc_register() call be
 * completed later with richer metadata.
 */
void tree_leaf_merge(fluxipc_node_t *node, const fluxipc_method_t *src)
{
    if (!node || !src) return;

    if (!node->method.data && src->data) node->method.data = src->data;

    if (!node->method.summary && src->summary)
        node->method.summary = dup_or_null(src->summary);
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
