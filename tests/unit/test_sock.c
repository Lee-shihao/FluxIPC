/**
 * test_sock.c – Unix-socket transport: request processor + client helpers.
 */

#include "ut_common.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

static int sock_setup(void **state)
{
    (void)state;
    ut_server_up();
    ut_server_pump_start();
    return 0;
}

static int sock_teardown(void **state)
{
    (void)state;
    ut_server_pump_stop();
    ut_server_down();
    return 0;
}

/* ── request header builder ──────────────────────────────────────────────── */

static size_t build_call(unsigned char *buf, uint16_t op,
                         const char *path, const char *args)
{
    size_t plen = path ? strlen(path) : 0;
    size_t alen = args ? strlen(args) : 0;
    fluxipc_req_hdr_t h = {
        .magic = FLUXIPC_PROTO_MAGIC, .op = op,
        .path_len = (uint16_t)plen, .arg_len = (uint32_t)alen,
    };
    memcpy(buf, &h, sizeof(h));
    size_t off = sizeof(h);
    if (plen) { memcpy(buf + off, path, plen); off += plen; }
    if (alen) { memcpy(buf + off, args, alen); off += alen; }
    return off;
}

static size_t build_call_raw(unsigned char *buf, uint16_t op,
                             const char *path, const char *args, size_t alen)
{
    size_t plen = path ? strlen(path) : 0;
    fluxipc_req_hdr_t h = {
        .magic = FLUXIPC_PROTO_MAGIC, .op = op,
        .path_len = (uint16_t)plen, .arg_len = (uint32_t)alen,
    };
    memcpy(buf, &h, sizeof(h));
    size_t off = sizeof(h);
    if (plen) { memcpy(buf + off, path, plen); off += plen; }
    if (alen) { memcpy(buf + off, args, alen); off += alen; }
    return off;
}

static void test_process_needs_header(void **state)
{
    (void)state;
    fluxipc_ctx_t *ctx = g_ctx;
    fluxipc_conn_t c;
    ut_conn_init(&c, "short", 5);
    assert_int_equal(sock_conn_process(ctx, &c), 0);   /* needs more */
    ut_conn_free(&c);
}

static void test_process_bad_magic(void **state)
{
    (void)state;
    unsigned char buf[64];
    size_t n = build_call(buf, FLUXIPC_OP_CALL, "/x", NULL);
    fluxipc_req_hdr_t *h = (fluxipc_req_hdr_t *)buf;
    h->magic = 0xDEADBEEF;

    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);
}

static void test_process_invalid_lengths(void **state)
{
    (void)state;
    unsigned char buf[64];
    fluxipc_conn_t c;

    size_t n = build_call(buf, FLUXIPC_OP_CALL, "/x", NULL);
    ((fluxipc_req_hdr_t *)buf)->path_len = FLUXIPC_PATH_MAX;   /* too big */
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);

    n = build_call(buf, FLUXIPC_OP_CALL, "/x", NULL);
    ((fluxipc_req_hdr_t *)buf)->arg_len = 2 * 1024 * 1024;     /* too big */
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);

    n = build_call(buf, 99, "/x", NULL);                       /* bad op */
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);

    n = build_call(buf, FLUXIPC_OP_CALL, "/x", NULL);
    ((fluxipc_req_hdr_t *)buf)->arg_len = FLUXIPC_RBUF_MAX;    /* total too big */
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);
}

static void test_process_call(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"hello" };
    fluxipc_node_leaf(root, "echo", &m);

    unsigned char buf[128];
    size_t n = build_call(buf, FLUXIPC_OP_CALL, "/echo", NULL);

    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), 1);
    assert_int_equal(c.wlen, sizeof(fluxipc_resp_hdr_t) + 5);
    fluxipc_resp_hdr_t rh;
    memcpy(&rh, c.wbuf, sizeof(rh));
    assert_int_equal(rh.magic, FLUXIPC_PROTO_MAGIC);
    assert_int_equal(rh.status, 0);
    assert_int_equal(rh.data_len, 5);
    assert_memory_equal(c.wbuf + sizeof(rh), "hello", 5);
    ut_conn_free(&c);
}

static void test_process_list(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .summary = "listed" };
    fluxipc_node_leaf(root, "x", &m);

    unsigned char buf[64];
    size_t n = build_call(buf, FLUXIPC_OP_LIST, "", NULL);
    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), 1);
    fluxipc_resp_hdr_t rh;
    memcpy(&rh, c.wbuf, sizeof(rh));
    assert_int_equal(rh.status, 0);
    assert_non_null(memmem(c.wbuf + sizeof(rh), rh.data_len, "/x", 2));
    assert_non_null(memmem(c.wbuf + sizeof(rh), rh.data_len, "listed", 6));
    ut_conn_free(&c);
}

static void test_client_roundtrip(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"PONG" };
    fluxipc_node_leaf(root, "ping", &m);

    char out[32];
    size_t len = 0;
    char *argv[] = { (char *)"/ping" };
    int rc = fluxipc_call(ut_server_sock(), "/ping", 1, argv,
                          out, sizeof(out), &len);
    assert_int_equal(rc, 0);
    assert_int_equal(len, 4);
    assert_memory_equal(out, "PONG", 4);

    /* missing socket */
    assert_true(fluxipc_call("/nonexistent.sock", "/ping", 0, NULL,
                             out, sizeof(out), &len) < 0);
}

static void test_client_truncation(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"0123456789" };
    fluxipc_node_leaf(root, "big", &m);

    char out[4];
    size_t len = 0;
    char *argv[] = { (char *)"/big" };
    int rc = fluxipc_call(ut_server_sock(), "/big", 1, argv,
                          out, sizeof(out), &len);
    assert_int_equal(rc, 0);
    assert_int_equal(len, 4);           /* truncated to capacity */
    assert_memory_equal(out, "0123", 4);
}

static void test_call_alloc(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"HELLO" };
    fluxipc_node_leaf(root, "e", &m);

    char *out = NULL;
    size_t len = 0;
    char *argv[] = { (char *)"/e" };
    int rc = sock_call_alloc(ut_server_sock(), FLUXIPC_OP_CALL, "/e",
                             1, argv, &out, &len);
    assert_int_equal(rc, 0);
    assert_int_equal(len, 5);
    assert_string_equal(out, "HELLO");
    free(out);
}

/* ── argument parsing ────────────────────────────────────────────────────── */

static int g_argc_seen;
static char g_argv_seen[8][32];

static int record_ipc(void *d, int argc, char **argv,
                      void *out, size_t cap, size_t *len)
{
    (void)d; (void)out; (void)cap;
    g_argc_seen = argc;
    int n = argc < 8 ? argc : 8;
    for (int i = 0; i < n; i++)
        snprintf(g_argv_seen[i], sizeof(g_argv_seen[i]), "%s", argv[i]);
    if (len) *len = 0;
    return 0;
}

static void test_process_path_nul(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = record_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "r", &m);

    const char path[] = { '/', 'r', '\0', 'x' };
    fluxipc_req_hdr_t h = {
        .magic = FLUXIPC_PROTO_MAGIC, .op = FLUXIPC_OP_CALL,
        .path_len = sizeof(path), .arg_len = 0,
    };
    unsigned char buf[64];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), path, sizeof(path));

    fluxipc_conn_t c;
    ut_conn_init(&c, buf, sizeof(h) + sizeof(path));
    assert_int_equal(sock_conn_process(g_ctx, &c), -1);
    ut_conn_free(&c);
}

static void test_process_args_multiple(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = record_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "r", &m);

    const char args[] = "a\0bb\0ccc\0";
    unsigned char buf[64];
    size_t n = build_call_raw(buf, FLUXIPC_OP_CALL, "/r", args,
                              sizeof(args) - 1);

    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), 1);
    assert_int_equal(g_argc_seen, 3);
    assert_string_equal(g_argv_seen[0], "a");
    assert_string_equal(g_argv_seen[1], "bb");
    assert_string_equal(g_argv_seen[2], "ccc");
    ut_conn_free(&c);
}

static void test_process_args_empty(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = record_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "r", &m);

    unsigned char buf[64];
    size_t n = build_call_raw(buf, FLUXIPC_OP_CALL, "/r", NULL, 0);
    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), 1);
    assert_int_equal(g_argc_seen, 0);
    ut_conn_free(&c);
}

static void test_process_args_limit(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = record_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "r", &m);

    char args[256];
    size_t alen = 0;
    for (int i = 0; i < FLUXIPC_MAX_ARGS + 5; i++) {
        char tmp[8];
        int l = snprintf(tmp, sizeof(tmp), "%d", i);
        memcpy(args + alen, tmp, (size_t)l + 1);
        alen += (size_t)l + 1;
    }

    unsigned char buf[512];
    size_t n = build_call_raw(buf, FLUXIPC_OP_CALL, "/r", args, alen);
    fluxipc_conn_t c;
    ut_conn_init(&c, buf, n);
    assert_int_equal(sock_conn_process(g_ctx, &c), 1);
    assert_int_equal(g_argc_seen, FLUXIPC_MAX_ARGS);   /* capped */
    ut_conn_free(&c);
}

/* ── client edge cases ───────────────────────────────────────────────────── */

static void test_call_null_outbuf(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"DATA" };
    fluxipc_node_leaf(fluxipc_tree_root(), "d", &m);

    char *argv[] = { (char *)"/d" };
    size_t len = 999;
    int rc = fluxipc_call(ut_server_sock(), "/d", 1, argv, NULL, 0, &len);
    assert_int_equal(rc, 0);
    assert_int_equal(len, 0);              /* no buffer → nothing reported */
}

static void test_call_alloc_null_out(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"HELLO" };
    fluxipc_node_leaf(fluxipc_tree_root(), "e", &m);

    char *argv[] = { (char *)"/e" };
    size_t len = 0;
    int rc = sock_call_alloc(ut_server_sock(), FLUXIPC_OP_CALL, "/e",
                             1, argv, NULL, &len);
    assert_int_equal(rc, 0);
    assert_int_equal(len, 5);
}

static void test_client_connect_nonsocket(void **state)
{
    (void)state;
    char out[16];
    assert_true(fluxipc_call("/etc/hostname", "/x", 0, NULL,
                             out, sizeof(out), NULL) < 0);
}

/* ── fake server: malformed / truncated responses ────────────────────────── */

static int g_fake_mode;      /* 0 = bad magic, 1 = truncated body */
static int g_fake_err;

static void *fake_server_fn(void *arg)
{
    const char *sp = arg;
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { g_fake_err = 1; return NULL; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sp);
    unlink(sp);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
        g_fake_err = 1; close(lfd); return NULL;
    }

    int c = accept(lfd, NULL, NULL);
    if (c < 0) { g_fake_err = 1; close(lfd); return NULL; }

    char sink[512];
    (void)recv(c, sink, sizeof(sink), 0);      /* consume the request */

    fluxipc_resp_hdr_t h;
    if (g_fake_mode == 0) {
        h.magic = 0xDEADBEEF; h.status = 0; h.data_len = 0;
    } else {
        h.magic = FLUXIPC_PROTO_MAGIC; h.status = 0; h.data_len = 16;
    }
    (void)send(c, &h, sizeof(h), 0);
    close(c); close(lfd); unlink(sp);
    return NULL;
}

static void fake_sock_path(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/fluxipc_fake_%d_%d.sock", (int)getpid(), g_fake_mode);
}

static void test_client_bad_magic(void **state)
{
    (void)state;
    char sp[128];
    g_fake_mode = 0; g_fake_err = 0;
    fake_sock_path(sp, sizeof(sp));
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, fake_server_fn, sp), 0);
    usleep(50 * 1000);

    char out[8];
    assert_int_equal(fluxipc_call(sp, "/x", 0, NULL, out, sizeof(out), NULL),
                     -EPROTO);
    pthread_join(th, NULL);
    assert_int_equal(g_fake_err, 0);
}

static void test_call_alloc_bad_magic(void **state)
{
    (void)state;
    char sp[128];
    g_fake_mode = 0; g_fake_err = 0;
    fake_sock_path(sp, sizeof(sp));
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, fake_server_fn, sp), 0);
    usleep(50 * 1000);

    char *p = NULL; size_t l = 0;
    assert_int_equal(sock_call_alloc(sp, FLUXIPC_OP_CALL, "/x", 0, NULL, &p, &l),
                     -EPROTO);
    pthread_join(th, NULL);
    assert_int_equal(g_fake_err, 0);
}

static void test_client_truncated_body(void **state)
{
    (void)state;
    char sp[128];
    g_fake_mode = 1; g_fake_err = 0;
    fake_sock_path(sp, sizeof(sp));
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, fake_server_fn, sp), 0);
    usleep(50 * 1000);

    char out[64];
    /* header promises 16 bytes, none arrive → -EIO */
    assert_int_equal(fluxipc_call(sp, "/x", 0, NULL, out, sizeof(out), NULL),
                     -EIO);
    pthread_join(th, NULL);
    assert_int_equal(g_fake_err, 0);
}

static void test_call_alloc_truncated_body(void **state)
{
    (void)state;
    char sp[128];
    g_fake_mode = 1; g_fake_err = 0;
    fake_sock_path(sp, sizeof(sp));
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, fake_server_fn, sp), 0);
    usleep(50 * 1000);

    char *p = NULL; size_t l = 0;
    assert_int_equal(sock_call_alloc(sp, FLUXIPC_OP_CALL, "/x", 0, NULL, &p, &l),
                     -EIO);
    pthread_join(th, NULL);
    assert_int_equal(g_fake_err, 0);
}

static void test_client_null_path_and_len(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"Z" };
    fluxipc_node_leaf(fluxipc_tree_root(), "z", &m);

    /* NULL path exercises the plen==0 branch and yields -ENOENT */
    char out[8];
    assert_int_equal(fluxipc_call(ut_server_sock(), NULL, 0, NULL,
                                  out, sizeof(out), NULL), -ENOENT);

    char *argv[] = { (char *)"/z" };
    assert_int_equal(fluxipc_call(ut_server_sock(), "/z", 1, argv,
                                  out, sizeof(out), NULL), 0);
}

static void test_server_create_bind_failure(void **state)
{
    (void)state;
    /* Point the run dir at a non-existent location so bind() fails. */
    char saved_run[FLUXIPC_PATH_MAX];
    char saved_sock[FLUXIPC_PATH_MAX];
    int  saved_fd = g_ctx->server_fd;
    snprintf(saved_run, sizeof(saved_run), "%s", g_ctx->run_dir);
    snprintf(saved_sock, sizeof(saved_sock), "%s", g_ctx->sock_path);

    snprintf(g_ctx->run_dir, sizeof(g_ctx->run_dir),
             "/nonexistent-fluxipc-dir-xyz");
    ut_capture_begin();
    int rc = sock_server_create(g_ctx);
    const char *err = ut_capture_end();

    assert_true(rc < 0);
    assert_int_equal(g_ctx->server_fd, -1);
    assert_non_null(strstr(err, "bind"));

    /* restore so teardown unlinks the real socket */
    snprintf(g_ctx->run_dir, sizeof(g_ctx->run_dir), "%s", saved_run);
    snprintf(g_ctx->sock_path, sizeof(g_ctx->sock_path), "%s", saved_sock);
    g_ctx->server_fd = saved_fd;
}

int ut_sock(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_process_needs_header,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_bad_magic,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_invalid_lengths,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_call,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_list,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_roundtrip,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_truncation,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_call_alloc,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_path_nul,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_args_multiple,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_args_empty,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_process_args_limit,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_call_null_outbuf,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_call_alloc_null_out,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_connect_nonsocket,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_server_create_bind_failure,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_bad_magic,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_call_alloc_bad_magic,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_truncated_body,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_call_alloc_truncated_body,
                                        sock_setup, sock_teardown),
        cmocka_unit_test_setup_teardown(test_client_null_path_and_len,
                                        sock_setup, sock_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
