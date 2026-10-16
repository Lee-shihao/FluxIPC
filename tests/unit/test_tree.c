/**
 * test_tree.c – semantic tests for the namespace tree (no server required).
 *
 * Covers creation, lookup, name-slot collisions, ownership of method strings,
 * deep paths (the 64-component token cap) and destruction.
 */

#include "ut_common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static fluxipc_node_t *make_root(void)
{
    fluxipc_node_t *root = calloc(1, sizeof(*root));
    assert_non_null(root);
    root->name[0] = '/';
    root->name[1] = '\0';
    root->full_path[0] = '/';
    root->full_path[1] = '\0';
    return root;
}

static int dummy_ipc(void *d, int c, char **v, void *o, size_t n, size_t *l)
{
    (void)d; (void)c; (void)v; (void)o; (void)n; (void)l;
    return 0;
}

static const fluxipc_method_t M = { .summary = "m", .ipc = dummy_ipc };

/* ── mkdir ───────────────────────────────────────────────────────────────── */

static void test_mkdir_basic(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();

    fluxipc_node_t *a = tree_mkdir(root, "a");
    fluxipc_node_t *b = tree_mkdir(a, "b");
    fluxipc_node_t *c = tree_leaf(b, "c", &M);

    assert_non_null(a);
    assert_non_null(b);
    assert_non_null(c);
    assert_string_equal(a->full_path, "/a");
    assert_string_equal(b->full_path, "/a/b");
    assert_string_equal(c->full_path, "/a/b/c");
    assert_true(c->is_leaf);
    assert_false(b->is_leaf);
    assert_ptr_equal(c->parent, b);

    tree_destroy(root);
}

static void test_mkdir_invalid(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();

    errno = 0;
    assert_null(tree_mkdir(NULL, "a"));
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_null(tree_mkdir(root, NULL));
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_null(tree_mkdir(root, ""));
    assert_int_equal(errno, EINVAL);

    tree_destroy(root);
}

static void test_mkdir_idempotent(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *a1 = tree_mkdir(root, "a");
    fluxipc_node_t *a2 = tree_mkdir(root, "a");
    assert_ptr_equal(a1, a2);
    tree_destroy(root);
}

static void test_mkdir_on_leaf_fails(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *b = tree_mkdir(root, "b");
    assert_non_null(tree_leaf(b, "c", &M));

    errno = 0;
    assert_null(tree_mkdir(b, "c"));
    assert_int_equal(errno, EEXIST);

    tree_destroy(root);
}

/* ── leaf ────────────────────────────────────────────────────────────────── */

static void test_leaf_invalid(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *a = tree_mkdir(root, "a");

    /* no handlers at all */
    fluxipc_method_t empty = { .summary = "x" };
    assert_null(tree_leaf(a, "nohandler", &empty));
    /* NULL args */
    assert_null(tree_leaf(NULL, "x", &M));
    assert_null(tree_leaf(a, NULL, &M));
    assert_null(tree_leaf(a, "x", NULL));
    assert_null(tree_leaf(a, "", &M));

    tree_destroy(root);
}

static void test_leaf_copies_method(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();

    char summary[32];
    strcpy(summary, "original");
    char desc[32];
    strcpy(desc, "d");
    char schema[32];
    strcpy(schema, "{}");
    fluxipc_method_t m = {
        .ipc = dummy_ipc, .summary = summary,
        .description = desc, .schema = schema, .methods = FLUXIPC_M_GET,
    };
    fluxipc_node_t *n = tree_leaf(root, "n", &m);
    assert_non_null(n);

    /* the node owns copies, mutating the source must not leak through */
    strcpy(summary, "CHANGED");
    strcpy(desc, "CHANGED");
    strcpy(schema, "CHANGED");
    assert_string_equal(n->method.summary, "original");
    assert_string_equal(n->method.description, "d");
    assert_string_equal(n->method.schema, "{}");
    assert_int_equal(n->method.methods, FLUXIPC_M_GET);
    assert_ptr_equal(n->method.ipc, dummy_ipc);

    /* NULL optional strings stay NULL */
    fluxipc_method_t m2 = { .ipc = dummy_ipc };
    fluxipc_node_t *n2 = tree_leaf(root, "n2", &m2);
    assert_null(n2->method.summary);
    assert_null(n2->method.description);
    assert_null(n2->method.schema);

    tree_destroy(root);
}

static void test_leaf_default_methods(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_method_t m = { .ipc = dummy_ipc, .methods = 0 };
    fluxipc_node_t *n = tree_leaf(root, "n", &m);
    assert_non_null(n);
    assert_int_equal(n->method.methods, FLUXIPC_M_DEFAULT);
    tree_destroy(root);
}

static void test_leaf_existing_name_fails(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *a = tree_mkdir(root, "a");
    assert_non_null(tree_leaf(a, "b", &M));
    assert_null(tree_leaf(a, "b", &M));   /* leaf name taken */
    assert_null(tree_leaf(root, "a", &M)); /* container name taken */
    tree_destroy(root);
}

/* ── lookup ──────────────────────────────────────────────────────────────── */

static void test_find_child_and_path(void **state)
{
    (void)state;
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *a  = tree_mkdir(root, "a");
    fluxipc_node_t *b  = tree_mkdir(a, "b");
    fluxipc_node_t *c  = tree_leaf(b, "c", &M);
    fluxipc_node_t *d  = tree_leaf(b, "d", &M);

    assert_ptr_equal(tree_find_child(root, "a"), a);
    assert_ptr_equal(tree_find_child(a, "b"), b);
    assert_ptr_equal(tree_find_child(b, "c"), c);
    assert_null(tree_find_child(root, "zzz"));
    assert_null(tree_find_child(NULL, "a"));
    assert_null(tree_find_child(root, NULL));

    assert_ptr_equal(tree_find_path(root, "/a/b/c"), c);
    assert_ptr_equal(tree_find_path(root, "/a/b/d"), d);
    assert_ptr_equal(tree_find_path(root, "/"), root);
    assert_ptr_equal(tree_find_path(root, "/a"), a);
    assert_ptr_equal(tree_find_path(root, "/a/b/"), b);  /* trailing slash */
    assert_null(tree_find_path(root, "/a/x"));
    assert_null(tree_find_path(root, "a/b"));
    assert_null(tree_find_path(root, NULL));
    assert_null(tree_find_path(NULL, "/a"));

    tree_destroy(root);
}

static void test_deep_path_token_cap(void **state)
{
    (void)state;
    /* Build a 70-level chain; tree_find_path only walks 64 components. */
    fluxipc_node_t *root = make_root();
    fluxipc_node_t *cur  = root;
    for (int i = 0; i < 70; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "n%d", i);
        cur = tree_mkdir(cur, nm);
        assert_non_null(cur);
    }
    /* full path exists all the way down */
    const char *full = cur->full_path;
    fluxipc_node_t *hit = tree_find_path(root, full);
    assert_non_null(hit);            /* capped at 64 components, still a node */
    tree_destroy(root);
}

static void test_destroy_handles_null(void **state)
{
    (void)state;
    tree_destroy(NULL);              /* must not crash */
    fluxipc_node_t *root = make_root();
    tree_destroy(root);
}

int ut_tree(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mkdir_basic),
        cmocka_unit_test(test_mkdir_invalid),
        cmocka_unit_test(test_mkdir_idempotent),
        cmocka_unit_test(test_mkdir_on_leaf_fails),
        cmocka_unit_test(test_leaf_invalid),
        cmocka_unit_test(test_leaf_copies_method),
        cmocka_unit_test(test_leaf_default_methods),
        cmocka_unit_test(test_leaf_existing_name_fails),
        cmocka_unit_test(test_find_child_and_path),
        cmocka_unit_test(test_deep_path_token_cap),
        cmocka_unit_test(test_destroy_handles_null),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
