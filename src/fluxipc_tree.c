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

/* ─── Cached schema fields for positional CALL_JSON ─────────────────────── */

static uint8_t schema_type_from_name(const char *s)
{
    if (!s) return FLUXIPC_SCHEMA_ANY;
    if (strcmp(s, "string")  == 0) return FLUXIPC_SCHEMA_STRING;
    if (strcmp(s, "integer") == 0) return FLUXIPC_SCHEMA_INTEGER;
    if (strcmp(s, "number")  == 0) return FLUXIPC_SCHEMA_NUMBER;
    if (strcmp(s, "boolean") == 0) return FLUXIPC_SCHEMA_BOOLEAN;
    if (strcmp(s, "object")  == 0) return FLUXIPC_SCHEMA_OBJECT;
    if (strcmp(s, "array")   == 0) return FLUXIPC_SCHEMA_ARRAY;
    return FLUXIPC_SCHEMA_ANY;
}

static uint8_t schema_type_from_prop(const cJSON *prop)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(prop, "type");
    return cJSON_IsString(t) ? schema_type_from_name(t->valuestring)
                             : FLUXIPC_SCHEMA_ANY;
}

static uint8_t schema_item_type(const cJSON *prop)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(prop, "items");
    return cJSON_IsObject(items) ? schema_type_from_prop(items)
                                 : FLUXIPC_SCHEMA_ANY;
}

static void node_schema_fields_free(fluxipc_node_t *n)
{
    if (!n) return;
    for (uint16_t i = 0; i < n->schema_field_count; i++)
        free(n->schema_fields[i].name);
    free(n->schema_fields);
    n->schema_fields = NULL;
    n->schema_field_count = 0;
    n->schema_fields_built = 0;
}

/** Parse method.schema once, on the first positional CALL_JSON. */
static void node_schema_fields_build(fluxipc_node_t *n)
{
    n->schema_fields_built = 1;
    if (!n->method.schema) return;

    cJSON *schema = cJSON_Parse(n->method.schema);
    if (!schema) return;

    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!cJSON_IsObject(props) || !props->child) {
        cJSON_Delete(schema);
        return;
    }

    size_t count = 0;
    for (cJSON *p = props->child; p; p = p->next) count++;
    if (count == 0 || count > UINT16_MAX) {
        cJSON_Delete(schema);
        return;
    }

    fluxipc_schema_field_t *fields = calloc(count, sizeof(*fields));
    if (!fields) { cJSON_Delete(schema); return; }

    size_t i = 0;
    for (cJSON *p = props->child; p; p = p->next) {
        if (!p->string) continue;
        char *name = strdup(p->string);
        if (!name) {
            for (size_t j = 0; j < i; j++) free(fields[j].name);
            free(fields);
            cJSON_Delete(schema);
            return;
        }
        fields[i].name      = name;
        fields[i].type      = schema_type_from_prop(p);
        fields[i].item_type = schema_item_type(p);
        i++;
    }

    cJSON_Delete(schema);
    if (i == 0) { free(fields); return; }

    n->schema_fields       = fields;
    n->schema_field_count  = (uint16_t)i;
}

const fluxipc_schema_field_t *tree_node_schema_fields(fluxipc_node_t *node,
                                                      size_t *count)
{
    if (count) *count = 0;
    if (!node || !node->is_leaf) return NULL;
    if (!node->schema_fields_built) node_schema_fields_build(node);
    if (count) *count = node->schema_field_count;
    return node->schema_fields;
}

/** Free a node's owned memory (method strings + cached schema fields). */
void tree_node_free(fluxipc_node_t *n)
{
    if (!n) return;
    node_schema_fields_free(n);
    free((char *)n->method.summary);
    free((char *)n->method.description);
    free((char *)n->method.schema);
    free(n);
}

/* ─── Path helpers ────────────────────────────────────────────────────────── */

/**
 * Split an absolute path into components in place.
 * tokens[] points into @p path (which must be writable).
 * Returns token count.
 */
static int path_split(char *path, char **tokens, int max_tokens)
{
    int n = 0;
    char *p = path + 1; /* skip leading '/' */
    while (*p && n < max_tokens) {
        tokens[n++] = p;
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = '\0';
        p = slash + 1;
    }
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

    /* Paths are bounded by FLUXIPC_PATH_MAX; avoid a heap allocation on
     * every request by splitting a stack copy in place. */
    char buf[FLUXIPC_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return NULL;
    memcpy(buf, path, len + 1);

    char *tokens[64];
    int n = path_split(buf, tokens, 64);

    fluxipc_node_t *cur = root;
    for (int i = 0; i < n && cur; i++)
        cur = tree_find_child(cur, tokens[i]);
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
        (!method->ipc && !method->json))
        return NULL;

    if (tree_find_child(parent, name)) return NULL;   /* EEXIST */

    fluxipc_node_t *n = node_alloc(name);
    if (!n) return NULL;

    n->method.ipc         = method->ipc;
    n->method.json        = method->json;
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
 * Merge @p src into an already-registered leaf.  This lets the IPC and JSON
 * sides of one endpoint be registered independently (and in either order):
 * only fields that are still empty are filled, so the first registration of a
 * given handler/metadata value is the one that is kept.  The method mask is
 * owned by whichever call first attaches a JSON handler.
 */
void tree_leaf_merge(fluxipc_node_t *node, const fluxipc_method_t *src)
{
    if (!node->method.ipc  && src->ipc)  node->method.ipc  = src->ipc;

    int had_json = node->method.json != NULL;
    if (!node->method.json && src->json) node->method.json = src->json;
    if (src->json && src->methods && !had_json)
        node->method.methods = src->methods;   /* the JSON side owns the mask */

    if (!node->method.data && src->data) node->method.data = src->data;

    if (!node->method.summary     && src->summary)
        node->method.summary     = dup_or_null(src->summary);
    if (!node->method.description && src->description)
        node->method.description = dup_or_null(src->description);
    if (!node->method.schema      && src->schema) {
        node->method.schema = dup_or_null(src->schema);
        /* A schema added by a later merge invalidates any earlier cache. */
        node_schema_fields_free(node);
    }
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
