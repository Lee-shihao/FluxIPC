/**
 * test_conn.c – connection pool, buffered read/write state machine.
 *
 * Uses the FLUXIPC_TESTING hooks to drive the otherwise-static connection
 * state over real socketpairs (complete, partial, closed and slow peers).
 */

#include "ut_common.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

/* ── pool ────────────────────────────────────────────────────────────────── */

static void test_pool_init_sets_free(void **state)
{
    (void)state;
    conn_pool_init();
    assert_int_equal(conn_pool_free_count(), FLUXIPC_MAX_CONNS);
    assert_int_equal(fluxipc_ut_pool_size(), FLUXIPC_MAX_CONNS);
    conn_pool_init();   /* idempotent */
    assert_int_equal(conn_pool_free_count(), FLUXIPC_MAX_CONNS);
}

static void test_pool_reset(void **state)
{
    (void)state;
    conn_pool_init();
    conn_pool_reset();
    assert_int_equal(conn_pool_free_count(), FLUXIPC_MAX_CONNS);
}

static void test_conn_slot_accessor(void **state)
{
    (void)state;
    conn_pool_init();
    assert_non_null(fluxipc_ut_conn(0));
    assert_non_null(fluxipc_ut_conn(FLUXIPC_MAX_CONNS - 1));
    assert_null(fluxipc_ut_conn(-1));
    assert_null(fluxipc_ut_conn(FLUXIPC_MAX_CONNS));
}

static void test_pool_exhaustion(void **state)
{
    (void)state;
    conn_pool_init();

    int fds[FLUXIPC_MAX_CONNS];
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fds[i] = open("/dev/null", O_RDONLY);
        assert_true(fds[i] >= 0);
        assert_true(fluxipc_ut_conn_alloc(fds[i]) >= 0);
    }
    assert_int_equal(conn_pool_free_count(), 0);

    int extra = open("/dev/null", O_RDONLY);
    assert_true(extra >= 0);
    assert_int_equal(fluxipc_ut_conn_alloc(extra), -1);
    close(extra);

    conn_pool_reset();
    assert_int_equal(conn_pool_free_count(), FLUXIPC_MAX_CONNS);
}

/* ── response buffering ──────────────────────────────────────────────────── */

static void test_conn_close_null_safe(void **state)
{
    (void)state;
    conn_close(NULL);
    fluxipc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    conn_close(&c);
    conn_close(&c);
    assert_int_equal(c.fd, -1);
}

static void test_conn_respond_basic(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;

    assert_int_equal(conn_respond(&c, "hello", 5), 0);
    assert_int_equal(c.wlen, 5);
    assert_int_equal(c.woff, 0);
    assert_int_equal(c.state, FLUXIPC_CONN_WRITING);
    assert_memory_equal(c.wbuf, "hello", 5);
    free(c.wbuf);
}

static void test_conn_respond_empty(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    assert_int_equal(conn_respond(&c, NULL, 0), 0);
    assert_int_equal(c.wlen, 0);
    assert_int_equal(c.state, FLUXIPC_CONN_WRITING);
    free(c.wbuf);
}

static void test_conn_respond_grows(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    assert_int_equal(conn_respond(&c, "abc", 3), 0);
    char big[4096];
    memset(big, 'Z', sizeof(big));
    assert_int_equal(conn_respond(&c, big, sizeof(big)), 0);
    assert_int_equal(c.wlen, sizeof(big));
    assert_memory_equal(c.wbuf, big, sizeof(big));
    free(c.wbuf);
}

static void test_conn_respond_own(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;

    char *buf = malloc(5);
    assert_non_null(buf);
    memcpy(buf, "hello", 5);

    assert_int_equal(conn_respond_own(&c, buf, 5), 0);
    assert_ptr_equal(c.wbuf, buf);
    assert_int_equal(c.wlen, 5);
    assert_int_equal(c.woff, 0);
    assert_int_equal(c.state, FLUXIPC_CONN_WRITING);

    conn_close(&c);
    assert_null(c.wbuf);

    /* NULL connection still takes ownership and frees the buffer. */
    char *orphan = malloc(1);
    assert_non_null(orphan);
    assert_int_equal(conn_respond_own(NULL, orphan, 1), -1);
}

/* ── read state machine (socketpairs) ────────────────────────────────────── */

static void make_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    assert_true(fl >= 0);
    assert_int_equal(fcntl(fd, F_SETFL, fl | O_NONBLOCK), 0);
}

static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        assert_true(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void test_read_partial_then_complete(void **state)
{
    (void)state;
    conn_pool_init();
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    make_nonblock(sv[0]);

    int idx = fluxipc_ut_conn_alloc(sv[0]);
    assert_true(idx >= 0);
    fluxipc_conn_t *c = fluxipc_ut_conn(idx);

    /* header only: processor needs more */
    fluxipc_req_hdr_t h = { .magic = FLUXIPC_PROTO_MAGIC,
                            .op = FLUXIPC_OP_CALL,
                            .path_len = 4, .arg_len = 0 };
    write_all(sv[1], &h, sizeof(h));
    fluxipc_ut_conn_read(NULL, c);
    assert_int_equal(c->state, FLUXIPC_CONN_READING);
    assert_true(c->fd >= 0);

    close(sv[1]);          /* peer closes with an incomplete request */
    fluxipc_ut_conn_read(NULL, c);
    assert_int_equal(c->fd, -1);   /* closed */
    conn_pool_reset();
}

static void test_read_peer_closed(void **state)
{
    (void)state;
    conn_pool_init();
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    make_nonblock(sv[0]);
    int idx = fluxipc_ut_conn_alloc(sv[0]);
    fluxipc_conn_t *c = fluxipc_ut_conn(idx);

    close(sv[1]);
    fluxipc_ut_conn_read(NULL, c);   /* recv == 0 → close */
    assert_int_equal(c->fd, -1);
    conn_pool_reset();
}

static void test_read_grows_request_buffer(void **state)
{
    (void)state;
    conn_pool_init();
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    make_nonblock(sv[0]);
    int idx = fluxipc_ut_conn_alloc(sv[0]);
    fluxipc_conn_t *c = fluxipc_ut_conn(idx);
    size_t initial_cap = c->rcap;

    /* claim a big arg buffer but send only part of it → stays READING */
    fluxipc_req_hdr_t h = { .magic = FLUXIPC_PROTO_MAGIC,
                            .op = FLUXIPC_OP_CALL,
                            .path_len = 2, .arg_len = 40000 };
    write_all(sv[1], &h, sizeof(h));
    write_all(sv[1], "/x", 2);
    char chunk[4096];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 3; i++) write_all(sv[1], chunk, sizeof(chunk));

    fluxipc_ut_conn_read(NULL, c);
    assert_int_equal(c->state, FLUXIPC_CONN_READING);
    assert_true(c->rcap > initial_cap);      /* buffer grew */
    assert_true(c->need > c->rlen);          /* still incomplete */

    close(sv[1]);
    fluxipc_ut_conn_read(NULL, c);
    assert_int_equal(c->fd, -1);
    conn_pool_reset();
}

/* ── write state machine ─────────────────────────────────────────────────── */

static void test_write_to_closed_peer(void **state)
{
    (void)state;
    conn_pool_init();
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    make_nonblock(sv[0]);
    int idx = fluxipc_ut_conn_alloc(sv[0]);
    fluxipc_conn_t *c = fluxipc_ut_conn(idx);

    close(sv[1]);
    assert_int_equal(conn_respond(c, "payload", 7), 0);
    fluxipc_ut_conn_write(c);       /* EPIPE → close */
    assert_int_equal(c->fd, -1);
    conn_pool_reset();
}

static void test_write_partial_then_drain(void **state)
{
    (void)state;
    conn_pool_init();
    int sv[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    /* small send buffer + nonblocking server end forces EAGAIN */
    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);

    int idx = fluxipc_ut_conn_alloc(sv[0]);
    fluxipc_conn_t *c = fluxipc_ut_conn(idx);

    size_t total = 256 * 1024;
    char *big = malloc(total);
    assert_non_null(big);
    memset(big, 'Q', total);
    assert_int_equal(conn_respond(c, big, total), 0);

    fluxipc_ut_conn_write(c);          /* partial write, must not close */
    assert_true(c->fd >= 0);
    assert_true(c->woff < c->wlen);

    /* drain the peer while nudging the writer until it finishes */
    char drain[8192];
    for (int i = 0; i < 100000 && c->fd >= 0; i++) {
        while (read(sv[1], drain, sizeof(drain)) > 0) { /* keep draining */ }
        fluxipc_ut_conn_write(c);
    }
    assert_int_equal(c->fd, -1);       /* one response per connection */

    free(big);
    conn_pool_reset();
}

/* ── poll with no descriptors ────────────────────────────────────────────── */

static void test_idle_timeout(void **state)
{
    (void)state;
    ut_server_up();
    conn_pool_init();

    int fds[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int idx = fluxipc_ut_conn_alloc(fds[0]);
    assert_true(idx >= 0);

    fluxipc_conn_t *c = fluxipc_ut_conn(idx);
    c->last_active_ms = 1;       /* far older than the monotonic clock */

    int rc = conn_poll(g_ctx, -1);   /* immediate idle deadline */
    int closed = (c->fd == -1);

    close(fds[1]);
    ut_server_down();

    assert_int_equal(rc, 0);
    assert_true(closed);
}

static void test_poll_no_descriptors(void **state)
{
    (void)state;
    ut_server_up();
    conn_pool_init();
    int saved_srv  = g_ctx->server_fd;
    int saved_wake = g_ctx->wake_fd;
    g_ctx->server_fd = -1;
    g_ctx->wake_fd   = -1;   /* exercise the timeout fallback */

    assert_int_equal(conn_poll(g_ctx, 0), 0);   /* no descriptors, no error */

    g_ctx->server_fd = saved_srv;
    g_ctx->wake_fd   = saved_wake;
    ut_server_down();
}

int ut_conn(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pool_init_sets_free),
        cmocka_unit_test(test_pool_reset),
        cmocka_unit_test(test_conn_slot_accessor),
        cmocka_unit_test(test_pool_exhaustion),
        cmocka_unit_test(test_conn_close_null_safe),
        cmocka_unit_test(test_conn_respond_basic),
        cmocka_unit_test(test_conn_respond_empty),
        cmocka_unit_test(test_conn_respond_grows),
        cmocka_unit_test(test_conn_respond_own),
        cmocka_unit_test(test_read_partial_then_complete),
        cmocka_unit_test(test_read_peer_closed),
        cmocka_unit_test(test_read_grows_request_buffer),
        cmocka_unit_test(test_write_to_closed_peer),
        cmocka_unit_test(test_write_partial_then_drain),
        cmocka_unit_test(test_idle_timeout),
        cmocka_unit_test(test_poll_no_descriptors),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
