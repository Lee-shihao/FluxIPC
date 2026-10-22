/**
 * test_core.c – lifecycle, registration, manifest, dispatch, hooks and the
 * host-owned event-loop/threading contract.
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
#include <sys/stat.h>
#include <pthread.h>

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

static int ipc_handler2(void *d, int c, char **v, void *o, size_t n, size_t *l)
{ (void)d; (void)c; (void)v; (void)o; (void)n; (void)l; return 0; }

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

static fluxipc_request_t g_pre, g_post;
static int g_pre_n, g_post_n;

static void pre_hook(const fluxipc_request_t *ev, void *user)
{
    (void)user;
    g_pre = *ev;
    g_pre_n++;
}

static void post_hook(const fluxipc_request_t *ev, void *user)
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
    assert_int_equal(fluxipc_server_init(NULL), -EINVAL);
    assert_int_equal(fluxipc_server_init(""), -EINVAL);
    assert_int_equal(fluxipc_server_init("bad/name"), -EINVAL);
    assert_int_equal(fluxipc_server_init("validname"), 0);
    fluxipc_destroy();
    assert_null(g_ctx);
}

static void test_init_double(void **state)
{
    (void)state;
    ut_server_down();
    const char *n = ut_next_name();
    assert_int_equal(fluxipc_server_init(n), 0);
    assert_non_null(g_ctx);
    assert_int_equal(fluxipc_server_init(ut_next_name()), -EALREADY);
    assert_int_equal(g_ctx->server_fd >= 0, 1);
    fluxipc_destroy();
    assert_null(g_ctx);
}

static void test_init_run_dir_exists(void **state)
{
    (void)state;
    ut_server_down();

    /* fresh dir + errno=0: mkdir succeeds, no error must be logged */
    const char *n1 = ut_next_name();
    errno = 0;
    ut_capture_begin();
    assert_int_equal(fluxipc_server_init(n1), 0);
    const char *err = ut_capture_end();
    assert_null(strstr(err, "mkdir run_dir"));
    fluxipc_destroy();

    /* dir already exists: EEXIST must be tolerated silently */
    const char *n2 = ut_next_name();
    char dir[FLUXIPC_PATH_MAX];
    snprintf(dir, sizeof(dir), "/run/user/%d/%s-fluxipc", (int)getuid(), n2);
    assert_int_equal(mkdir(dir, 0755), 0);
    ut_capture_begin();
    assert_int_equal(fluxipc_server_init(n2), 0);
    err = ut_capture_end();
    assert_null(strstr(err, "mkdir run_dir"));
    fluxipc_destroy();
}

static void *stop_after_delay(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    fluxipc_stop();
    return NULL;
}

static void test_poll_and_stop(void **state)
{
    (void)state;
    ut_server_down();
    assert_int_equal(fluxipc_poll(), -EINVAL);   /* before init */

    ut_server_up();

    /* conn_poll() blocks on select(); fluxipc_stop() wakes it via eventfd. */
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, stop_after_delay, NULL), 0);
    assert_true(fluxipc_poll() >= 0);
    pthread_join(th, NULL);
    assert_int_equal(atomic_load(&g_ctx->running), 0);
    assert_int_equal(fluxipc_poll(), -EINVAL);   /* after stop */
    assert_int_equal(fluxipc_is_running(), 0);
    ut_server_down();

    /* destroy with no context is a no-op */
    fluxipc_destroy();
    assert_null(g_ctx);
}

static void test_poll_timeout_nonblocking(void **state)
{
    (void)state;
    ut_server_up();
    assert_int_equal(fluxipc_poll_timeout(0), 0);
    ut_server_down();
}

static void *run_thread(void *arg)
{
    int *rc = arg;
    *rc = fluxipc_run();
    return NULL;
}

static void test_run_in_host_thread(void **state)
{
    (void)state;
    ut_server_up();

    int run_rc = 12345;
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, run_thread, &run_rc), 0);

    struct timespec ts = { 0, 30 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    fluxipc_wakeup();          /* does not stop the loop */
    assert_int_equal(fluxipc_is_running(), 1);

    fluxipc_stop();
    pthread_join(th, NULL);
    assert_int_equal(run_rc, 0);
    ut_server_down();
}

static void *poll_thread(void *arg)
{
    int *rc = arg;
    *rc = fluxipc_poll();
    return NULL;
}

static void test_single_poller_contract(void **state)
{
    (void)state;
    ut_server_up();

    int poll_rc = 12345;
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, poll_thread, &poll_rc), 0);

    /* Wait until the helper is actually inside conn_poll(). */
    for (int i = 0; i < 1000 && atomic_load(&g_ctx->poll_busy) == 0; i++) {
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert_int_equal(atomic_load(&g_ctx->poll_busy), 1);

    /* A second polling thread is rejected instead of corrupting the pool. */
    assert_int_equal(fluxipc_poll_timeout(0), -EBUSY);

    fluxipc_stop();
    pthread_join(th, NULL);
    assert_true(poll_rc >= 0);
    ut_server_down();
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

    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "sum" };
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
    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "sum" };

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

    /* 1) first registration */
    fluxipc_method_t first = {
        .ipc = ipc_handler, .summary = "combined",
    };
    fluxipc_node_t *n = fluxipc_register_method("/m/x", &first);
    assert_non_null(n);
    assert_ptr_equal(n->method.ipc, ipc_handler);
    assert_null(n->method.data);
    assert_string_equal(n->method.summary, "combined");

    /* 2) merge fills empty fields only */
    fluxipc_method_t second = {
        .ipc = ipc_handler2, .data = &ctx, .summary = "ignored",
    };
    assert_ptr_equal(fluxipc_register_method("/m/x", &second), n);
    assert_ptr_equal(n->method.ipc, ipc_handler);      /* kept */
    assert_ptr_equal(n->method.data, &ctx);
    assert_string_equal(n->method.summary, "combined"); /* kept */
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

    /* NULL handler is rejected; NULL usage is accepted (empty summary) */
    errno = 0;
    assert_null(fluxipc_register("/legacy/b", "x", 0, NULL, NULL));
    assert_int_equal(errno, EINVAL);
    assert_non_null(fluxipc_register("/legacy/c", NULL, 0, ipc_handler, NULL));

    /* a later registration is merged without overwriting anything */
    fluxipc_node_t *h = fluxipc_register_method("/legacy/a",
        &FLUXIPC_METHOD(ipc_handler2, "ignored"));
    assert_ptr_equal(h, n);
    assert_ptr_equal(n->method.ipc, ipc_handler);
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

static int count_tabs(const char *line)
{
    int n = 0;
    for (; *line && *line != '\n'; line++)
        if (*line == '\t') n++;
    return n;
}

static void test_manifest(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_node_t *b = fluxipc_node_mkdir(a, "b");

    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "S" };
    fluxipc_node_leaf(a, "leaf1", &m);
    fluxipc_node_leaf(b, "leaf2", &m);

    size_t len = 0;
    char *mf = fluxipc_build_manifest(a, &len);
    assert_non_null(mf);
    assert_true(len > 0);
    assert_int_equal(strlen(mf), len);
    assert_non_null(strstr(mf, "/a/leaf1"));
    assert_non_null(strstr(mf, "/a/b/leaf2"));
    assert_non_null(strstr(mf, "\tS\n"));

    /* every emitted line has exactly two delimiters */
    const char *p = mf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        assert_non_null(nl);
        assert_int_equal(count_tabs(p), 2);
        p = nl + 1;
    }
    free(mf);

    /* Tabs/newlines in user metadata must not break the framing. */
    fluxipc_method_t dirty = { .ipc = ipc_handler,
                               .summary = "bad\tsum\nary" };
    fluxipc_node_leaf(a, "dirty", &dirty);
    mf = fluxipc_build_manifest(a, &len);
    assert_non_null(mf);
    assert_non_null(strstr(mf, "bad sum ary"));
    for (const char *q = mf; *q; q++)
        assert_true(*q != '\r');
    p = mf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        assert_non_null(nl);
        assert_int_equal(count_tabs(p), 2);
        p = nl + 1;
    }
    free(mf);

    /* empty subtree -> empty (but valid) manifest */
    fluxipc_node_t *empty = fluxipc_node_mkdir(root, "empty");
    assert_non_null(empty);
    mf = fluxipc_build_manifest(empty, &len);
    assert_non_null(mf);
    assert_int_equal(len, 0);
    assert_string_equal(mf, "");
    free(mf);

    len = 99;
    assert_null(fluxipc_build_manifest(NULL, &len));
    assert_int_equal(len, 0);
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

    /* invalid arguments */
    assert_int_equal(fluxipc_dispatch_ipc(NULL, "/a/c", 0, NULL,
                                          out, sizeof(out), &len), -EINVAL);
    assert_int_equal(fluxipc_dispatch_ipc(g_ctx, NULL, 0, NULL,
                                          out, sizeof(out), &len), -EINVAL);
}

static void test_hooks(void **state)
{
    (void)state;
    g_pre_n = g_post_n = 0;
    memset(&g_pre, 0, sizeof(g_pre));
    memset(&g_post, 0, sizeof(g_post));

    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ipc_handler, .data = (void *)"D" };
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
    assert_string_equal(g_pre.path, "/hooked");
    assert_int_equal(g_pre.matched, 1);
    assert_int_equal(g_post.status, 0);
    assert_ptr_equal(g_post.out_buf, out);
    assert_int_equal(g_post.out_len, len);

    /* unmatched request reports matched=0 and no response */
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

static void test_usage_zero_leaves(void **state)
{
    (void)state;
    ut_capture_begin();
    fluxipc_usage("empty");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "(none)"));
}

/* ── list_endpoints ──────────────────────────────────────────────────────── */

static int g_list_count;
static char g_list_last[FLUXIPC_PATH_MAX];

static void list_cb(const char *path, uint32_t id, const char *usage,
                    void *ud)
{
    (void)id; (void)usage; (void)ud;
    if (path && path[0]) {
        g_list_count++;
        snprintf(g_list_last, sizeof(g_list_last), "%s", path);
    }
}

static void test_list_endpoints_errors(void **state)
{
    (void)state;
    assert_int_equal(fluxipc_list_endpoints(NULL, list_cb, NULL), -EINVAL);
    assert_int_equal(fluxipc_list_endpoints("x", NULL, NULL), -EINVAL);
    assert_int_equal(fluxipc_list_endpoints("bad/name", list_cb, NULL), -EINVAL);
    /* unreachable server → connect fails with ENOENT */
    assert_int_equal(fluxipc_list_endpoints("no_such_server_xyz", list_cb, NULL),
                     -ENOENT);
}

static void test_list_endpoints_success(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ipc_handler, .summary = "ping me" };
    assert_non_null(fluxipc_register_method("/ping", &m));

    ut_server_pump_start();

    g_list_count = 0;
    g_list_last[0] = '\0';
    int rc = fluxipc_list_endpoints(ut_server_name(), list_cb, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(g_list_count, 1);
    assert_string_equal(g_list_last, "/ping");

    ut_server_pump_stop();
}

static void test_concurrent_registration(void **state)
{
    (void)state;
    ut_server_pump_start();

    /* Register from the host thread while the poll thread is running. */
    for (int i = 0; i < 50; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bulk/e%02d", i);
        assert_non_null(fluxipc_register_method(
            path, &FLUXIPC_METHOD(ipc_handler, "bulk")));
    }

    g_list_count = 0;
    assert_int_equal(fluxipc_list_endpoints(ut_server_name(), list_cb, NULL), 0);
    assert_int_equal(g_list_count, 50);

    ut_server_pump_stop();
}

int ut_core(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init_invalid),
        cmocka_unit_test(test_init_double),
        cmocka_unit_test(test_init_run_dir_exists),
        cmocka_unit_test(test_poll_and_stop),
        cmocka_unit_test(test_poll_timeout_nonblocking),
        cmocka_unit_test(test_run_in_host_thread),
        cmocka_unit_test(test_single_poller_contract),
        cmocka_unit_test(test_socket_file_permissions),
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
        cmocka_unit_test_setup_teardown(test_remove_and_prune,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_node_leaves,
                                        core_setup, core_teardown),
        cmocka_unit_test(test_node_leaves_without_ctx),
        cmocka_unit_test_setup_teardown(test_manifest,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_accessors,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_ipc,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_hooks,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_usage,
                                        core_setup, core_teardown),
        cmocka_unit_test(test_usage_no_ctx),
        cmocka_unit_test_setup_teardown(test_usage_zero_leaves,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_list_endpoints_errors,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_list_endpoints_success,
                                        core_setup, core_teardown),
        cmocka_unit_test_setup_teardown(test_concurrent_registration,
                                        core_setup, core_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
