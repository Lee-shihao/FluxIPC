/**
 * test_core.c – lifecycle, registration, manifest, dispatch and hooks.
 *
 * Uses cmocka per-test fixtures so a fresh server (and a cleared g_ctx) is
 * guaranteed even when an assertion aborts a test halfway through.
 */

#include "ut_common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

/* ── fixtures ────────────────────────────────────────────────────────────── */

static int core_setup(void **state)
{
    (void)state;
    ut_server_up();
    return 0;
}

static int core_teardown(void **state)
{
    (void)state;
    ut_server_down();
    return 0;
}

/* ── callbacks / handlers ────────────────────────────────────────────────── */

static int g_ipc_calls;
static char g_ipc_last[128];

static int ipc_handler(void *data, int argc, char **argv,
                       void *out, size_t out_cap, size_t *out_len)
{
    g_ipc_calls++;
    const char *seed = data ? (const char *)data : "h";
    if (argc > 1) snprintf(g_ipc_last, sizeof(g_ipc_last), "%s", argv[1]);
    int n = snprintf(out, out_cap, "%s:%s", seed,
                     argc > 1 ? argv[1] : "");
    if (out_len) *out_len = (size_t)n;
    return 0;
}

static int http_handler(void *data, const fluxipc_http_request_t *req,
                        fluxipc_http_response_t *resp)
{
    resp->json = cJSON_CreateObject();
    if (resp->json) {
        cJSON_AddStringToObject(resp->json, "handler",
                                data ? (const char *)data : "h");
        cJSON_AddNumberToObject(resp->json, "query", (double)req->query_count);
    }
    return 0;
}

/* distinct callbacks used to prove a merge never overwrites an existing side */
static int ipc_handler2(void *d, int c, char **v, void *o, size_t n, size_t *l)
{ (void)d; (void)c; (void)v; (void)o; (void)n; (void)l; return 0; }

static int http_handler2(void *d, const fluxipc_http_request_t *r,
                         fluxipc_http_response_t *resp)
{ (void)d; (void)r; (void)resp; return 0; }

static cJSON *find_leaf_named(cJSON *manifest, const char *name)
{
    cJSON *leaves = cJSON_GetObjectItem(manifest, "leaves");
    cJSON *it;
    cJSON_ArrayForEach(it, leaves) {
        cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            return it;
    }
    return NULL;
}

static int count_cb(const fluxipc_node_t *leaf, void *user)
{
    (void)leaf;
    (*(int *)user)++;
    return 0;
}

static int stop_cb(const fluxipc_node_t *leaf, void *user)
{
    (void)leaf;
    (*(int *)user)++;
    return 1;   /* stop immediately */
}

static fluxipc_event_t g_pre, g_post;
static int g_pre_n, g_post_n;

static void pre_hook(const fluxipc_event_t *ev, void *user)
{
    (void)user;
    g_pre = *ev;
    g_pre_n++;
}

static void post_hook(const fluxipc_event_t *ev, void *user)
{
    (void)user;
    g_post = *ev;
    g_post_n++;
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

static void test_init_invalid(void **state)
{
    (void)state;
    ut_server_down();   /* guarantee no active context */
    assert_int_equal(fluxipc_server_init(NULL, 0), -EINVAL);
    assert_int_equal(fluxipc_server_init("", 0), -EINVAL);
    assert_null(g_ctx);
}

static void test_init_double(void **state)
{
    (void)state;
    ut_server_down();
    const char *n = ut_next_name();
    assert_int_equal(fluxipc_server_init(n, 0), 0);
    assert_non_null(g_ctx);
    assert_int_equal(fluxipc_server_init(ut_next_name(), 0), -EALREADY);
    assert_int_equal(g_ctx->server_fd >= 0, 1);
    assert_int_equal(g_ctx->http_fd, -1);
    fluxipc_destroy();
    assert_null(g_ctx);
}

static void test_init_http_port_in_use(void **state)
{
    (void)state;
    ut_server_down();

    /* Occupy an ephemeral TCP port, then ask the server to use it. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(s >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    assert_int_equal(bind(s, (struct sockaddr *)&a, sizeof(a)), 0);
    assert_int_equal(listen(s, 8), 0);
    socklen_t alen = sizeof(a);
    assert_int_equal(getsockname(s, (struct sockaddr *)&a, &alen), 0);
    uint16_t port = ntohs(a.sin_port);

    const char *n = ut_next_name();
    /* HTTP failure is non-fatal: init still succeeds, HTTP stays disabled. */
    ut_capture_begin();
    assert_int_equal(fluxipc_server_init(n, port), 0);
    const char *err = ut_capture_end();
    assert_non_null(g_ctx);
    assert_int_equal(g_ctx->http_fd, -1);
    assert_non_null(strstr(err, "HTTP start"));   /* failure is reported */
    fluxipc_destroy();
    close(s);
}

static void test_init_run_dir_exists(void **state)
{
    (void)state;
    ut_server_down();

    /* fresh dir + errno=0: mkdir succeeds, no error must be logged */
    const char *n1 = ut_next_name();
    errno = 0;
    ut_capture_begin();
    assert_int_equal(fluxipc_server_init(n1, 0), 0);
    const char *err = ut_capture_end();
    assert_null(strstr(err, "mkdir run_dir"));
    fluxipc_destroy();

    /* dir already exists: EEXIST must be tolerated silently */
    const char *n2 = ut_next_name();
    char dir[FLUXIPC_PATH_MAX];
    snprintf(dir, sizeof(dir), "/run/user/%d/%s-fluxipc", (int)getuid(), n2);
    assert_int_equal(mkdir(dir, 0755), 0);
    ut_capture_begin();
    assert_int_equal(fluxipc_server_init(n2, 0), 0);
    err = ut_capture_end();
    assert_null(strstr(err, "mkdir run_dir"));
    fluxipc_destroy();
}

static void test_poll_and_stop(void **state)
{
    (void)state;
    ut_server_down();
    assert_int_equal(fluxipc_poll(NULL), -EINVAL);   /* before init */

    ut_server_up();
    assert_true(fluxipc_poll(NULL) >= 0);
    fluxipc_stop();
    assert_int_equal(g_ctx->running, 0);
    assert_int_equal(fluxipc_poll(NULL), -EINVAL);   /* after stop */
    ut_server_down();

    /* destroy with no context is a no-op */
    fluxipc_destroy();
    assert_null(g_ctx);
}

static void test_socket_file_permissions(void **state)
{
    (void)state;
    ut_server_up();
    struct stat st;
    assert_int_equal(stat(ut_server_sock(), &st), 0);
    assert_int_equal((st.st_mode & 0777), 0666);
    ut_server_down();
    assert_int_equal(stat(ut_server_sock(), &st), -1);  /* removed */
}

/* ── method/mask helpers ─────────────────────────────────────────────────── */

static void test_method_masks(void **state)
{
    (void)state;
    assert_int_equal(fluxipc_method_to_mask("GET"),    FLUXIPC_M_GET);
    assert_int_equal(fluxipc_method_to_mask("POST"),   FLUXIPC_M_POST);
    assert_int_equal(fluxipc_method_to_mask("PUT"),    FLUXIPC_M_PUT);
    assert_int_equal(fluxipc_method_to_mask("PATCH"),  FLUXIPC_M_PATCH);
    assert_int_equal(fluxipc_method_to_mask("DELETE"), FLUXIPC_M_DELETE);
    assert_int_equal(fluxipc_method_to_mask("get"),    0);
    assert_int_equal(fluxipc_method_to_mask(NULL),     0);

    assert_string_equal(fluxipc_mask_to_method(FLUXIPC_M_GET),    "GET");
    assert_string_equal(fluxipc_mask_to_method(FLUXIPC_M_POST),   "POST");
    assert_string_equal(fluxipc_mask_to_method(FLUXIPC_M_PUT),    "PUT");
    assert_string_equal(fluxipc_mask_to_method(FLUXIPC_M_PATCH),  "PATCH");
    assert_string_equal(fluxipc_mask_to_method(FLUXIPC_M_DELETE), "DELETE");
    assert_null(fluxipc_mask_to_method(0));
    assert_null(fluxipc_mask_to_method(1u << 20));
}

/* ── registration ────────────────────────────────────────────────────────── */

static void test_registration(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    assert_non_null(root);

    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    assert_non_null(a);
    fluxipc_node_t *b = fluxipc_node_mkdir(a, "b");
    assert_non_null(b);

    fluxipc_method_t m = { .ipc = ipc_handler, .json = http_handler,
                           .summary = "sum" };
    fluxipc_node_t *c = fluxipc_node_leaf(b, "c", &m);
    fluxipc_node_t *d = fluxipc_node_leaf(b, "d", &m);
    assert_non_null(c);
    assert_non_null(d);
    assert_true(c->id >= 1);
    assert_int_equal(d->id, c->id + 1);          /* monotonic ids */

    assert_ptr_equal(fluxipc_node_find(root, "a"), a);
    assert_ptr_equal(fluxipc_node_find(a, "b"), b);
    assert_ptr_equal(fluxipc_node_find(b, "c"), c);
    assert_null(fluxipc_node_find(root, "zzz"));

    /* duplicates are rejected */
    errno = 0;
    assert_null(fluxipc_node_leaf(b, "c", &m));
    assert_int_equal(errno, EEXIST);
    errno = 0;
    assert_null(fluxipc_node_mkdir(b, "c"));
    assert_int_equal(errno, EEXIST);

    /* invalid segments */
    errno = 0;
    assert_null(fluxipc_node_mkdir(root, "bad/name"));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_node_mkdir(root, ""));
    assert_int_equal(errno, EINVAL);
    char toolong[FLUXIPC_NAME_MAX + 4];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    errno = 0;
    assert_null(fluxipc_node_mkdir(root, toolong));
    assert_int_equal(errno, EINVAL);

    /* cannot nest under a leaf */
    errno = 0;
    assert_null(fluxipc_node_mkdir(c, "child"));
    assert_int_equal(errno, EINVAL);

    /* cannot register a handler-less leaf */
    fluxipc_method_t empty = { .summary = "x" };
    errno = 0;
    assert_null(fluxipc_node_leaf(b, "empty", &empty));
    assert_int_equal(errno, EINVAL);
}

/* ── path-based registration ─────────────────────────────────────────────── */

static void test_register_path(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ipc_handler, .json = http_handler,
                           .summary = "sum" };

    /* deep path auto-creates every intermediate container */
    fluxipc_node_t *leaf = fluxipc_register_method("/a/b/c", &m);
    assert_non_null(leaf);
    assert_true(fluxipc_node_is_leaf(leaf));
    assert_string_equal(fluxipc_node_path(leaf), "/a/b/c");
    assert_true(leaf->id >= 1);

    /* repeated slashes are tolerated */
    assert_non_null(fluxipc_register_method("//a//b//d", &m));
    assert_non_null(fluxipc_node_find(
        fluxipc_node_find(fluxipc_node_find(fluxipc_tree_root(), "a"), "b"),
        "d"));

    /* duplicate leaf path merges instead of failing */
    assert_ptr_equal(fluxipc_register_method("/a/b/c", &m), leaf);

    /* a container name cannot later become a leaf */
    errno = 0;
    assert_null(fluxipc_register_method("/a/b", &m));
    assert_int_equal(errno, EEXIST);

    /* descending through a leaf is rejected */
    errno = 0;
    assert_null(fluxipc_register_method("/a/b/c/deep", &m));
    assert_int_equal(errno, EEXIST);

    /* malformed paths */
    errno = 0;
    assert_null(fluxipc_register_method(NULL, &m));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register_method("no-slash", &m));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register_method("", &m));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register_method("/", &m));
    assert_int_equal(errno, EINVAL);

    /* trailing slash is invalid and the created container is rolled back */
    errno = 0;
    assert_null(fluxipc_register_method("/t/", &m));
    assert_int_equal(errno, EINVAL);
    assert_null(fluxipc_node_find(fluxipc_tree_root(), "t"));

    /* over-long component rolls back already-created ancestors */
    char bad[FLUXIPC_PATH_MAX];
    memcpy(bad, "/new/", 5);
    memset(bad + 5, 'x', FLUXIPC_NAME_MAX);
    bad[5 + FLUXIPC_NAME_MAX] = '\0';
    errno = 0;
    assert_null(fluxipc_register_method(bad, &m));
    assert_int_equal(errno, EINVAL);
    assert_null(fluxipc_node_find(fluxipc_tree_root(), "new"));

    /* rollback must never prune pre-existing empty ancestors */
    fluxipc_node_t *keep = fluxipc_node_mkdir(fluxipc_tree_root(), "keep");
    assert_non_null(keep);
    char deep_bad[FLUXIPC_PATH_MAX];
    memcpy(deep_bad, "/keep/new/", 10);
    memset(deep_bad + 10, 'y', FLUXIPC_NAME_MAX);
    deep_bad[10 + FLUXIPC_NAME_MAX] = '\0';
    errno = 0;
    assert_null(fluxipc_register_method(deep_bad, &m));
    assert_int_equal(errno, EINVAL);
    assert_non_null(fluxipc_node_find(fluxipc_tree_root(), "keep"));
    assert_null(fluxipc_node_find(keep, "new"));

    /* invalid method */
    errno = 0;
    assert_null(fluxipc_register_method("/x", NULL));
    assert_int_equal(errno, EINVAL);
    fluxipc_method_t empty = { .summary = "x" };
    errno = 0;
    assert_null(fluxipc_register_method("/x", &empty));
    assert_int_equal(errno, EINVAL);

    /* path longer than FLUXIPC_PATH_MAX */
    char huge[FLUXIPC_PATH_MAX + 8];
    huge[0] = '/';
    memset(huge + 1, 'a', sizeof(huge) - 2);
    huge[sizeof(huge) - 1] = '\0';
    errno = 0;
    assert_null(fluxipc_register_method(huge, &m));
    assert_int_equal(errno, EINVAL);
}

static void test_register_all_table(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ipc_handler };
    const fluxipc_endpoint_t table[] = {
        { "/t/a",     m },
        { "/t/b",     m },
        { "/t/sub/c", m },
    };

    assert_int_equal(fluxipc_register_all(table, 3), 0);
    fluxipc_node_t *t = fluxipc_node_find(fluxipc_tree_root(), "t");
    assert_non_null(t);
    int n = 0;
    assert_int_equal(fluxipc_node_leaves(t, count_cb, &n), 3);
    assert_int_equal(n, 3);

    /* first failure is reported as the offending negative errno: "sub" is a
     * container, so it cannot be turned into a leaf */
    const fluxipc_endpoint_t clash[] = { { "/t/sub", m } };
    assert_int_equal(fluxipc_register_all(clash, 1), -EEXIST);

    /* re-registering an existing leaf merges and succeeds */
    assert_int_equal(fluxipc_register_all(table, 1), 0);

    /* empty table is a no-op; NULL + count is invalid */
    assert_int_equal(fluxipc_register_all(NULL, 0), 0);
    assert_int_equal(fluxipc_register_all(NULL, 1), -EINVAL);
}

static void test_register_merge(void **state)
{
    (void)state;
    int ctx = 42;

    /* 1) IPC-only registration (the compatibility case) */
    fluxipc_method_t ipc_only = { .ipc = ipc_handler, .summary = "combined" };
    fluxipc_node_t *n = fluxipc_register_method("/m/x", &ipc_only);
    assert_non_null(n);
    assert_ptr_equal(n->method.ipc, ipc_handler);
    assert_null(n->method.json);
    assert_int_equal(n->method.methods, FLUXIPC_M_DEFAULT);
    assert_string_equal(n->method.summary, "combined");

    /* 2) JSON side registered separately: same node, both handlers present */
    fluxipc_method_t json_only = {
        .json = http_handler, .data = &ctx, .methods = FLUXIPC_M_PUT,
    };
    assert_ptr_equal(fluxipc_register_json("/m/x", &json_only), n);
    assert_ptr_equal(n->method.ipc, ipc_handler);      /* kept */
    assert_ptr_equal(n->method.json, http_handler);
    assert_ptr_equal(n->method.data, &ctx);
    assert_string_equal(n->method.summary, "combined");
    assert_int_equal(n->method.methods, FLUXIPC_M_PUT);

    /* 3) a later registration must not overwrite anything already set */
    fluxipc_method_t other = {
        .ipc = ipc_handler2, .http = http_handler2,   /* legacy alias */
        .data = (void *)1, .summary = "ignored", .methods = FLUXIPC_M_DELETE,
    };
    assert_ptr_equal(fluxipc_register_method("/m/x", &other), n);
    assert_ptr_equal(n->method.ipc, ipc_handler);
    assert_ptr_equal(n->method.json, http_handler);
    assert_ptr_equal(n->method.data, &ctx);
    assert_string_equal(n->method.summary, "combined");
    assert_int_equal(n->method.methods, FLUXIPC_M_PUT);

    /* 4) reverse order: JSON first, then IPC (legacy alias) */
    fluxipc_node_t *r = fluxipc_register_http("/m/y", &json_only);
    assert_non_null(r);
    assert_null(r->method.ipc);
    fluxipc_method_t ipc_only2 = { .ipc = ipc_handler2 };
    assert_ptr_equal(fluxipc_register_method("/m/y", &ipc_only2), r);
    assert_ptr_equal(r->method.ipc, ipc_handler2);
    assert_ptr_equal(r->method.json, http_handler);

    /* 5) fluxipc_register_json requires a JSON handler */
    errno = 0;
    assert_null(fluxipc_register_http("/m/z", &ipc_only));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register_http("/m/z", NULL));
    assert_int_equal(errno, EINVAL);
}

/* classic main-branch fluxipc_register(path, usage, slot_sz, handler, data) */
static void test_register_legacy(void **state)
{
    (void)state;
    int ctx = 7;

    fluxipc_node_t *n = fluxipc_register("/legacy/a", "[args]", 0,
                                         ipc_handler, &ctx);
    assert_non_null(n);
    assert_true(fluxipc_node_is_leaf(n));
    assert_ptr_equal(n->method.ipc, ipc_handler);
    assert_string_equal(n->method.summary, "[args]");
    assert_ptr_equal(n->method.data, &ctx);
    assert_null(n->method.json);
    assert_int_equal(n->method.methods, FLUXIPC_M_DEFAULT);

    /* NULL handler is rejected; NULL usage is accepted (empty summary) */
    errno = 0;
    assert_null(fluxipc_register("/legacy/b", "x", 0, NULL, NULL));
    assert_int_equal(errno, EINVAL);
    assert_non_null(fluxipc_register("/legacy/c", NULL, 0, ipc_handler, NULL));

    /* the JSON side can complete a classic registration, same node */
    fluxipc_node_t *h = fluxipc_register_json("/legacy/a",
        &FLUXIPC_METHOD(NULL, http_handler, "ignored", NULL, FLUXIPC_M_GET));
    assert_ptr_equal(h, n);
    assert_ptr_equal(n->method.ipc, ipc_handler);
    assert_ptr_equal(n->method.json, http_handler);
    assert_string_equal(n->method.summary, "[args]");

    /* fluxipc_unregister() is the main alias for fluxipc_node_remove() */
    fluxipc_node_t *g = fluxipc_register("/legacyrm/a", "u", 0, ipc_handler, NULL);
    assert_non_null(g);
    fluxipc_unregister(g);
    assert_null(fluxipc_node_find(fluxipc_tree_root(), "legacyrm"));
    fluxipc_unregister(NULL);            /* must be a no-op */
}

static void test_static_add_null(void **state)
{
    (void)state;
    fluxipc_static_add(NULL);            /* must be a no-op, no pollution */
}

static void test_noregistration_without_ctx(void **state)
{
    (void)state;
    ut_server_down();
    errno = 0;
    assert_null(fluxipc_node_mkdir((fluxipc_node_t *)1, "a"));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_node_leaf((fluxipc_node_t *)1, "a", NULL));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register_method("/a", (const fluxipc_method_t *)1));
    assert_int_equal(errno, EINVAL);
    errno = 0;
    assert_null(fluxipc_register("/a", "u", 0, ipc_handler, NULL));
    assert_int_equal(errno, EINVAL);
    assert_null(fluxipc_node_find((fluxipc_node_t *)1, "a"));
    assert_null(fluxipc_tree_root());
}

static void test_name_length_boundary(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    char name[FLUXIPC_NAME_MAX + 2];

    memset(name, 'a', FLUXIPC_NAME_MAX - 1);
    name[FLUXIPC_NAME_MAX - 1] = '\0';
    assert_non_null(fluxipc_node_mkdir(root, name));       /* max valid */

    memset(name, 'b', FLUXIPC_NAME_MAX);
    name[FLUXIPC_NAME_MAX] = '\0';
    errno = 0;
    assert_null(fluxipc_node_mkdir(root, name));           /* one too long */
    assert_int_equal(errno, EINVAL);
}

static void test_remove_middle_sibling(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler };
    fluxipc_node_t *a = fluxipc_node_leaf(root, "a", &m);
    fluxipc_node_t *b = fluxipc_node_leaf(root, "b", &m);
    fluxipc_node_t *c = fluxipc_node_leaf(root, "c", &m);
    assert_non_null(a); assert_non_null(b); assert_non_null(c);

    fluxipc_node_remove(b);                 /* not the head of the list */
    assert_non_null(fluxipc_node_find(root, "a"));
    assert_non_null(fluxipc_node_find(root, "c"));
    assert_null(fluxipc_node_find(root, "b"));
}

static void test_manifest_all_methods(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler, .json = http_handler,
                           .methods = FLUXIPC_M_ALL };
    fluxipc_node_leaf(root, "all", &m);

    cJSON *mf = fluxipc_build_manifest(root, 1, 0);
    cJSON *leaf = find_leaf_named(mf, "all");
    assert_non_null(leaf);
    assert_int_equal(cJSON_GetArraySize(cJSON_GetObjectItem(leaf, "methods")), 5);
    cJSON_Delete(mf);
}

static void test_remove_and_prune(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_node_t *b = fluxipc_node_mkdir(a, "b");
    fluxipc_method_t m = { .ipc = ipc_handler };
    fluxipc_node_t *c = fluxipc_node_leaf(b, "c", &m);
    assert_non_null(c);

    fluxipc_node_remove(c);
    /* /a and /a/b are now empty and must be pruned */
    assert_null(fluxipc_node_find(root, "a"));

    /* removing root or NULL is a no-op */
    fluxipc_node_remove(root);
    fluxipc_node_remove(NULL);
    assert_non_null(fluxipc_node_mkdir(root, "keep"));
}

/* ── leaves / manifest ───────────────────────────────────────────────────── */

static void test_node_leaves(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ipc_handler };
    fluxipc_node_leaf(root, "r", &m);
    fluxipc_node_leaf(a, "x", &m);
    fluxipc_node_leaf(a, "y", &m);

    int n = 0;
    assert_int_equal(fluxipc_node_leaves(root, count_cb, &n), 3);
    assert_int_equal(n, 3);

    int stopped = 0;
    assert_int_equal(fluxipc_node_leaves(root, stop_cb, &stopped), 1);
    assert_int_equal(stopped, 1);

    assert_int_equal(fluxipc_node_leaves(root, NULL, &n), -EINVAL);
    assert_int_equal(fluxipc_node_leaves(NULL, count_cb, &n), -EINVAL);
}

static void test_node_leaves_without_ctx(void **state)
{
    (void)state;
    ut_server_down();
    int n = 0;
    assert_int_equal(fluxipc_node_leaves((fluxipc_node_t *)1, count_cb, &n),
                     -EINVAL);
}

static void test_manifest(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_node_t *b = fluxipc_node_mkdir(a, "b");

    fluxipc_method_t m = { .ipc = ipc_handler, .json = http_handler,
                           .summary = "S", .description = "D",
                           .schema = "{\"type\":\"object\"}",
                           .methods = FLUXIPC_M_GET | FLUXIPC_M_POST };
    fluxipc_node_leaf(a, "leaf1", &m);
    fluxipc_node_leaf(b, "leaf2", &m);

    cJSON *mf = fluxipc_build_manifest(a, 1, 1);
    assert_non_null(mf);
    assert_string_equal(cJSON_GetObjectItem(mf, "kind")->valuestring,
                        "collection");
    assert_string_equal(cJSON_GetObjectItem(mf, "path")->valuestring, "/a");
    assert_int_equal(cJSON_GetObjectItem(mf, "count")->valueint, 2);
    cJSON *leaves = cJSON_GetObjectItem(mf, "leaves");
    assert_int_equal(cJSON_GetArraySize(leaves), 2);

    cJSON *first = find_leaf_named(mf, "leaf1");
    assert_non_null(first);
    assert_string_equal(cJSON_GetObjectItem(first, "summary")->valuestring, "S");
    assert_string_equal(cJSON_GetObjectItem(first, "description")->valuestring,
                        "D");
    assert_non_null(cJSON_GetObjectItem(first, "schema"));
    assert_int_equal(
        cJSON_GetArraySize(cJSON_GetObjectItem(first, "methods")), 2);
    /* relative path when the base is not root */
    assert_string_equal(cJSON_GetObjectItem(first, "path")->valuestring,
                        "/a/leaf1");
    cJSON_Delete(mf);

    /* non-recursive lists only direct leaves */
    mf = fluxipc_build_manifest(a, 0, 0);
    assert_int_equal(cJSON_GetObjectItem(mf, "count")->valueint, 1);
    cJSON_Delete(mf);

    /* verbose=0 omits description/schema */
    mf = fluxipc_build_manifest(a, 1, 0);
    cJSON *l0 = cJSON_GetArrayItem(cJSON_GetObjectItem(mf, "leaves"), 0);
    assert_null(cJSON_GetObjectItem(l0, "description"));
    assert_null(cJSON_GetObjectItem(l0, "schema"));
    cJSON_Delete(mf);

    /* invalid schema strings are omitted rather than fatal */
    fluxipc_method_t bad = { .ipc = ipc_handler, .summary = "bad",
                             .schema = "{not json" };
    fluxipc_node_leaf(a, "bad", &bad);
    mf = fluxipc_build_manifest(a, 1, 1);
    cJSON *bl = find_leaf_named(mf, "bad");
    assert_non_null(bl);
    assert_null(cJSON_GetObjectItem(bl, "schema"));
    cJSON_Delete(mf);

    assert_null(fluxipc_build_manifest(NULL, 1, 0));
}

static void test_manifest_root_paths(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "r" };
    fluxipc_node_leaf(root, "top", &m);

    cJSON *mf = fluxipc_build_manifest(root, 1, 0);
    cJSON *leaf = cJSON_GetArrayItem(cJSON_GetObjectItem(mf, "leaves"), 0);
    /* base "/" → path is the absolute path of the leaf */
    assert_string_equal(cJSON_GetObjectItem(leaf, "path")->valuestring, "/top");
    cJSON_Delete(mf);
}

/* ── accessors ───────────────────────────────────────────────────────────── */

static void test_accessors(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "s" };
    fluxipc_node_t *c = fluxipc_node_leaf(a, "c", &m);

    assert_string_equal(fluxipc_node_path(c), "/a/c");
    assert_string_equal(fluxipc_node_name(c), "c");
    assert_ptr_equal(fluxipc_node_method(c), &c->method);
    assert_true(fluxipc_node_is_leaf(c));
    assert_false(fluxipc_node_is_leaf(a));

    /* container has no method; NULL-safe accessors */
    assert_null(fluxipc_node_method(a));
    assert_null(fluxipc_node_path(NULL));
    assert_null(fluxipc_node_name(NULL));
    assert_null(fluxipc_node_method(NULL));
    assert_false(fluxipc_node_is_leaf(NULL));
}

/* ── dispatch ────────────────────────────────────────────────────────────── */

static void test_dispatch_ipc(void **state)
{
    (void)state;
    g_ipc_calls = 0;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ipc_handler, .data = (void *)"D" };
    fluxipc_node_leaf(a, "c", &m);

    char out[64];
    size_t len = 123;
    char *argv[] = { (char *)"/a/c", (char *)"42" };
    int rc = fluxipc_dispatch_ipc(g_ctx, "/a/c", 2, argv, out, sizeof(out), &len);
    assert_int_equal(rc, 0);
    assert_int_equal(g_ipc_calls, 1);
    assert_string_equal(out, "D:42");
    assert_int_equal(len, strlen("D:42"));
    assert_string_equal(g_ipc_last, "42");

    /* missing path → -ENOENT, out_len reset */
    size_t len2 = 77;
    rc = fluxipc_dispatch_ipc(g_ctx, "/nope", 0, NULL, out, sizeof(out), &len2);
    assert_int_equal(rc, -ENOENT);
    assert_int_equal(len2, 0);

    /* container is not callable */
    rc = fluxipc_dispatch_ipc(g_ctx, "/a", 0, NULL, out, sizeof(out), NULL);
    assert_int_equal(rc, -ENOENT);
}

static void test_dispatch_http(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ipc_handler, .json = http_handler,
                           .data = (void *)"H" };
    fluxipc_node_t *c = fluxipc_node_leaf(a, "c", &m);

    fluxipc_http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path   = "/a/c";
    fluxipc_http_response_t resp;
    memset(&resp, 0, sizeof(resp));

    int rc = fluxipc_dispatch_http(g_ctx, &req, &resp);
    assert_int_equal(rc, 0);
    assert_ptr_equal(req.node, c);
    assert_ptr_equal(req.data, (void *)"H");
    assert_non_null(resp.json);
    assert_string_equal(cJSON_GetObjectItem(resp.json, "handler")->valuestring,
                        "H");
    cJSON_Delete(resp.json);

    /* path with no JSON handler → -ENOSYS */
    fluxipc_method_t ipc_only = { .ipc = ipc_handler };
    fluxipc_node_leaf(a, "ipconly", &ipc_only);
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path   = "/a/ipconly";
    memset(&resp, 0, sizeof(resp));
    assert_int_equal(fluxipc_dispatch_http(g_ctx, &req, &resp), -ENOSYS);
}

static void test_hooks(void **state)
{
    (void)state;
    g_pre_n = g_post_n = 0;
    memset(&g_pre, 0, sizeof(g_pre));
    memset(&g_post, 0, sizeof(g_post));

    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler };
    fluxipc_node_leaf(root, "hooked", &m);

    fluxipc_set_request_hooks(pre_hook, post_hook, NULL);

    char out[32];
    size_t len = 0;
    char *argv[] = { (char *)"/hooked" };
    int rc = fluxipc_dispatch_ipc(g_ctx, "/hooked", 1, argv,
                                  out, sizeof(out), &len);
    assert_int_equal(rc, 0);
    assert_int_equal(g_pre_n, 1);
    assert_int_equal(g_post_n, 1);
    assert_string_equal(g_pre.transport, "ipc");
    assert_string_equal(g_pre.method, "CALL");
    assert_string_equal(g_pre.path, "/hooked");
    assert_int_equal(g_pre.matched, 1);
    assert_int_equal(g_post.status, 0);
    assert_int_equal(g_post.raw_len, len);

    /* unmatched request reports matched=0 */
    fluxipc_dispatch_ipc(g_ctx, "/missing", 0, NULL, out, sizeof(out), NULL);
    assert_int_equal(g_pre.matched, 0);

    /* replacing hooks works; NULL clears */
    fluxipc_set_request_hooks(NULL, NULL, NULL);
    int before = g_pre_n;
    fluxipc_dispatch_ipc(g_ctx, "/hooked", 0, NULL, out, sizeof(out), NULL);
    assert_int_equal(g_pre_n, before);
}

/* ── usage ───────────────────────────────────────────────────────────────── */

static void test_usage(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "does things" };
    fluxipc_node_leaf(root, "thing", &m);

    ut_capture_begin();
    fluxipc_usage("mytool");
    const char *out = ut_capture_end();

    assert_non_null(strstr(out, "mytool"));
    assert_non_null(strstr(out, "/thing"));
    assert_non_null(strstr(out, "does things"));
}

static void test_usage_no_ctx(void **state)
{
    (void)state;
    ut_server_down();
    ut_capture_begin();
    fluxipc_usage(NULL);
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "(none)"));
}

/* ── list_endpoints ──────────────────────────────────────────────────────── */

static int g_list_count;
static void list_cb(const char *path, uint32_t id, const char *usage,
                    void *ud)
{
    (void)id; (void)usage; (void)ud;
    if (path && path[0]) g_list_count++;
}

static void test_list_endpoints_errors(void **state)
{
    (void)state;
    assert_int_equal(fluxipc_list_endpoints(NULL, list_cb, NULL), -EINVAL);
    assert_int_equal(fluxipc_list_endpoints("x", NULL, NULL), -EINVAL);
    /* unreachable server → connect fails with ENOENT */
    assert_int_equal(fluxipc_list_endpoints("no_such_server_xyz", list_cb, NULL),
                     -ENOENT);
}

static void test_usage_zero_leaves(void **state)
{
    (void)state;
    ut_capture_begin();
    fluxipc_usage("empty");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "(none)"));
}

/* ── positional JSON binding ────────────────────────────────────────────── */

static void test_bind_positional(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    cJSON *o = NULL;
    int rc;

    fluxipc_method_t types = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(
            FLUXIPC_PROP_INT("i") "," FLUXIPC_PROP_NUMBER("n") ","
            FLUXIPC_PROP_BOOL("b") "," FLUXIPC_PROP_STR("s") ","
            FLUXIPC_PROP_ARRAY("arr", "string")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_types", &types));
    char *args[] = { (char *)"7", (char *)"1.5", (char *)"false",
                     (char *)"hello", (char *)"x", (char *)"y" };
    rc = fluxipc_json_bind_positional(g_ctx, "/bind_types", 6, args, &o);
    assert_int_equal(rc, 0);
    assert_int_equal(cJSON_GetObjectItem(o, "i")->valueint, 7);
    assert_true(cJSON_GetObjectItem(o, "n")->valuedouble == 1.5);
    assert_false(cJSON_IsTrue(cJSON_GetObjectItem(o, "b")));
    assert_string_equal(cJSON_GetObjectItem(o, "s")->valuestring, "hello");
    assert_int_equal(cJSON_GetArraySize(cJSON_GetObjectItem(o, "arr")), 2);
    cJSON_Delete(o);
    o = NULL;

    /* strtod fallback for numbers JSON does not accept (leading zero). */
    fluxipc_method_t fallback = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(FLUXIPC_PROP_INT("i")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_fallback", &fallback));
    char *fargs[] = { (char *)"007" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_fallback",
                                                  1, fargs, &o), 0);
    assert_int_equal(cJSON_GetObjectItem(o, "i")->valueint, 7);
    cJSON_Delete(o);
    o = NULL;

    /* invalid boolean */
    fluxipc_method_t boolbad = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(FLUXIPC_PROP_BOOL("b")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_boolbad", &boolbad));
    char *bargs[] = { (char *)"maybe" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_boolbad",
                                                  1, bargs, &o), -EINVAL);

    /* object + non-trailing array type checks */
    fluxipc_method_t objarr = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(
            FLUXIPC_PROP_ARRAY("arr", "integer") ","
            FLUXIPC_PROP("obj", "object")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_objarr", &objarr));
    char *oargs[] = { (char *)"[1,2]", (char *)"{\"x\":1}" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_objarr",
                                                  2, oargs, &o), 0);
    assert_true(cJSON_IsArray(cJSON_GetObjectItem(o, "arr")));
    assert_true(cJSON_IsObject(cJSON_GetObjectItem(o, "obj")));
    cJSON_Delete(o);
    o = NULL;

    char *obad[] = { (char *)"[1,2]", (char *)"[]" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_objarr",
                                                  2, obad, &o), -EINVAL);
    char *abad[] = { (char *)"{}", (char *)"{\"x\":1}" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_objarr",
                                                  2, abad, &o), -EINVAL);

    /* trailing untyped array collects strings */
    fluxipc_method_t noitems = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(FLUXIPC_PROP("arr", "array")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_noitems", &noitems));
    char *nargs[] = { (char *)"a", (char *)"b" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_noitems",
                                                  2, nargs, &o), 0);
    cJSON *arr = cJSON_GetObjectItem(o, "arr");
    assert_int_equal(cJSON_GetArraySize(arr), 2);
    assert_string_equal(cJSON_GetArrayItem(arr, 0)->valuestring, "a");
    cJSON_Delete(o);
    o = NULL;

    /* invalid array item */
    fluxipc_method_t baditem = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(FLUXIPC_PROP_ARRAY("arr", "integer")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_baditem", &baditem));
    char *iargs[] = { (char *)"1", (char *)"x" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_baditem",
                                                  2, iargs, &o), -EINVAL);

    /* schema without properties / malformed schema */
    fluxipc_method_t empty = {
        .json = http_handler,
        .schema = "{\"type\":\"object\"}",
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_empty", &empty));
    char *eargs[] = { (char *)"x" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_empty",
                                                  1, eargs, &o), -EINVAL);

    fluxipc_method_t badschema = {
        .json = http_handler,
        .schema = "not-json",
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_badschema", &badschema));
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_badschema",
                                                  1, eargs, &o), -EINVAL);

    /* more arguments than properties */
    fluxipc_method_t one = {
        .json = http_handler,
        .schema = FLUXIPC_SCHEMA(FLUXIPC_PROP_INT("i")),
    };
    assert_non_null(fluxipc_node_leaf(root, "bind_toomany", &one));
    char *targs[] = { (char *)"1", (char *)"2" };
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_toomany",
                                                  2, targs, &o), -E2BIG);

    /* no schema at all */
    fluxipc_method_t noschema = { .json = http_handler };
    assert_non_null(fluxipc_node_leaf(root, "bind_noschema", &noschema));
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_noschema",
                                                  1, eargs, &o), -EINVAL);

    /* argument validation */
    assert_int_equal(fluxipc_json_bind_positional(NULL, "/bind_types",
                                                  1, args, &o), -EINVAL);
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, NULL,
                                                  1, args, &o), -EINVAL);
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_types",
                                                  1, args, NULL), -EINVAL);
    assert_int_equal(fluxipc_json_bind_positional(g_ctx, "/bind_types",
                                                  0, NULL, &o), -EINVAL);
}

int ut_core(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init_invalid),
        cmocka_unit_test(test_init_double),
        cmocka_unit_test(test_init_http_port_in_use),
        cmocka_unit_test(test_init_run_dir_exists),
        cmocka_unit_test(test_poll_and_stop),
        cmocka_unit_test(test_socket_file_permissions),
        cmocka_unit_test(test_method_masks),
        cmocka_unit_test_setup_teardown(test_registration,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_register_path,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_register_all_table,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_register_merge,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_register_legacy,
                                        core_setup, core_teardown),
        cmocka_unit_test(test_static_add_null),
        cmocka_unit_test(test_noregistration_without_ctx),
        cmocka_unit_test_setup_teardown(test_name_length_boundary,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_remove_middle_sibling,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_manifest_all_methods,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_remove_and_prune,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_node_leaves,
                                        core_setup, core_teardown),
        cmocka_unit_test(test_node_leaves_without_ctx),
        cmocka_unit_test_setup_teardown(test_manifest,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_manifest_root_paths,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_accessors,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_ipc,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_http,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_bind_positional,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_hooks,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_usage,
                                        core_setup, core_teardown),
        cmocka_unit_test(test_usage_no_ctx),
        cmocka_unit_test_setup_teardown(test_list_endpoints_errors,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_usage_zero_leaves,
                                        core_setup, core_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
