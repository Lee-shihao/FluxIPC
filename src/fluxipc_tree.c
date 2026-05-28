/**
 * fluxipc_tree.c – namespace/object tree management
 *
 * The tree mirrors the UNIX filesystem hierarchy.  Each internal node
 * represents a path component; leaf nodes carry a handler + metadata.
 * All operations are O(depth × siblings), which is fine for typical IPC
 * namespaces (depth ≤ 8, fanout ≤ 64).
 */

#include "fluxipc_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Allocation helpers ──────────────────────────────────────────────────── */

static fluxipc_node_t *node_alloc(const char *name)
{
    fluxipc_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    snprintf(n->name, FLUXIPC_NAME_MAX, "%s", name);
    n->shm_idx = UINT32_MAX;
    return n;
}

/* ─── Path tokeniser ──────────────────────────────────────────────────────── */

/**
 * Split an absolute path into components.
 * tokens[] is filled with pointers into a copy of path.
 * Caller owns the returned copy and must free() it.
 * Returns number of tokens, or -1 on error.
 */
static int path_split(const char *path, char **tokens, int max_tokens, char **copy_out)
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

/* ─── Find or create ──────────────────────────────────────────────────────── */

/**
 * Find the node at path, or NULL if not present.
 */
fluxipc_node_t *tree_find(fluxipc_node_t *root, const char *path)
{
    if (!root || !path || path[0] != '/') return NULL;
    if (strcmp(path, "/") == 0) return root;

    char *tokens[64];
    char *buf;
    int   n = path_split(path, tokens, 64, &buf);
    if (n < 0) return NULL;

    fluxipc_node_t *cur = root;
    for (int i = 0; i < n; i++) {
        fluxipc_node_t *child = cur->children;
        fluxipc_node_t *found = NULL;
        while (child) {
            if (strcmp(child->name, tokens[i]) == 0) { found = child; break; }
            child = child->next_sibling;
        }
        if (!found) { free(buf); return NULL; }
        cur = found;
    }
    free(buf);
    return cur;
}

/**
 * Insert a node at path, creating intermediate nodes as needed.
 * Returns the leaf node (new or existing).
 */
fluxipc_node_t *tree_insert(fluxipc_node_t *root, const char *path)
{
    if (!root || !path || path[0] != '/') return NULL;
    if (strcmp(path, "/") == 0) return root;

    char *tokens[64];
    char *buf;
    int   n = path_split(path, tokens, 64, &buf);
    if (n < 0) return NULL;

    fluxipc_node_t *cur = root;
    char             partial[FLUXIPC_PATH_MAX];
    partial[0] = '\0';

    for (int i = 0; i < n; i++) {
        /* build partial path for diagnostics */
        strncat(partial, "/", sizeof(partial) - strlen(partial) - 1);
        strncat(partial, tokens[i], sizeof(partial) - strlen(partial) - 1);

        /* search existing children */
        fluxipc_node_t *child = cur->children;
        fluxipc_node_t *found = NULL;
        while (child) {
            if (strcmp(child->name, tokens[i]) == 0) { found = child; break; }
            child = child->next_sibling;
        }

        if (!found) {
            found = node_alloc(tokens[i]);
            if (!found) { free(buf); return NULL; }
            snprintf(found->full_path, FLUXIPC_PATH_MAX, "%s", partial);
            found->parent = cur;
            /* prepend to children list */
            found->next_sibling = cur->children;
            cur->children = found;
        }
        cur = found;
    }
    free(buf);
    return cur;
}

/**
 * Remove a leaf node from the tree.
 * Prunes now-empty ancestor internal nodes (no handler, no children).
 */
void tree_remove(fluxipc_node_t *node)
{
    if (!node || !node->parent) return; /* never remove root */

    fluxipc_node_t *cur = node;
    while (cur && cur->parent) {
        fluxipc_node_t *parent = cur->parent;

        /* unlink cur from parent's children list */
        if (parent->children == cur) {
            parent->children = cur->next_sibling;
        } else {
            fluxipc_node_t *prev = parent->children;
            while (prev && prev->next_sibling != cur) prev = prev->next_sibling;
            if (prev) prev->next_sibling = cur->next_sibling;
        }
        cur->next_sibling = NULL;
        cur->parent = NULL;
        free(cur);

        /* continue up only if parent is now an empty internal node */
        if (parent->handler || parent->children || !parent->parent) break;
        cur = parent;
    }
}

/* ─── Recursive destroy ───────────────────────────────────────────────────── */

void tree_destroy(fluxipc_node_t *node)
{
    if (!node) return;
    fluxipc_node_t *child = node->children;
    while (child) {
        fluxipc_node_t *next = child->next_sibling;
        tree_destroy(child);
        child = next;
    }
    free(node);
}

/* ─── Debug dump (optional) ───────────────────────────────────────────────── */

static void tree_dump_r(const fluxipc_node_t *node, int depth, FILE *out)
{
    if (!node) return;
    for (int i = 0; i < depth; i++) fprintf(out, "  ");
    fprintf(out, "%s%s  [id=%u%s]\n",
            node->name,
            node->is_leaf ? "()" : "/",
            node->id,
            node->is_leaf ? "" : " ns");
    fluxipc_node_t *child = node->children;
    while (child) { tree_dump_r(child, depth + 1, out); child = child->next_sibling; }
}

void tree_dump(const fluxipc_node_t *root, FILE *out)
{
    fprintf(out, "/\n");
    if (root) {
        fluxipc_node_t *child = root->children;
        while (child) { tree_dump_r(child, 1, out); child = child->next_sibling; }
    }
}
