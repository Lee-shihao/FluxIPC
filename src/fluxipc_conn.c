/**
 * fluxipc_conn.c – unified non-blocking connection layer
 *
 * One select() loop drives the Unix listener and every active connection.
 * The buffered, non-blocking read / queued-write state machine means a slow or
 * malicious peer can never block the server.
 *
 * Lifecycle of a connection:
 *   accept → READING (accumulate request) → process → WRITING (flush response)
 *          → close (exactly one request/response per connection)
 *
 * The loop is owned by the host application: it runs on whichever thread calls
 * fluxipc_run()/fluxipc_poll_timeout().  Registration and shutdown may be
 * driven from other threads.
 */

#include "fluxipc_internal.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#define CONN_IO_CHUNK 4096
#define CONN_POLL_MS  10

static fluxipc_conn_t g_conns[FLUXIPC_MAX_CONNS];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ─── helpers ───────────────────────────────────────────────────────────── */

void conn_close(fluxipc_conn_t *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->rbuf);
    free(c->wbuf);
    c->fd = -1;
    c->rbuf = c->wbuf = NULL;
    c->rlen = c->rcap = 0;
    c->wlen = c->woff = c->wcap = 0;
    c->need = 0;
    c->last_active_ms = 0;
}

void conn_pool_init(void)
{
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        memset(&g_conns[i], 0, sizeof(g_conns[i]));
        g_conns[i].fd = -1;   /* -1 = free slot; 0 is a valid fd! */
    }
}

void conn_pool_reset(void)
{
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++)
        conn_close(&g_conns[i]);
}

int conn_pool_free_count(void)
{
    int n = 0;
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++)
        if (g_conns[i].fd < 0) n++;
    return n;
}

static fluxipc_conn_t *conn_alloc(int fd)
{
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd >= 0) continue;
        memset(c, 0, sizeof(*c));
        c->fd   = fd;
        c->state = FLUXIPC_CONN_READING;
        c->last_active_ms = now_ms();
        c->rbuf = malloc(CONN_IO_CHUNK);
        if (!c->rbuf) { c->fd = -1; return NULL; }
        c->rcap = CONN_IO_CHUNK;
        return c;
    }
    return NULL;   /* pool exhausted */
}

int conn_respond(fluxipc_conn_t *c, const void *data, size_t len)
{
    if (!c) return -1;
    if (len > 0) {
        if (len > c->wcap) {
            char *nb = realloc(c->wbuf, len);
            if (!nb) return -1;
            c->wbuf = nb;
            c->wcap = len;
        }
        memcpy(c->wbuf, data, len);
    }
    c->wlen = len;
    c->woff = 0;
    c->state = FLUXIPC_CONN_WRITING;
    return 0;
}

int conn_respond_own(fluxipc_conn_t *c, char *buf, size_t len)
{
    if (!c) { free(buf); return -1; }
    free(c->wbuf);
    c->wbuf = buf;
    c->wcap = len;
    c->wlen = len;
    c->woff = 0;
    c->state = FLUXIPC_CONN_WRITING;
    return 0;
}

/* ─── accept ────────────────────────────────────────────────────────────── */

static void accept_unix(fluxipc_ctx_t *ctx)
{
    for (;;) {
        struct sockaddr_un peer;
        socklen_t plen = sizeof(peer);
        int fd = accept4(ctx->server_fd, (struct sockaddr *)&peer, &plen,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (!conn_alloc(fd)) close(fd);
    }
}

/* ─── read / write ──────────────────────────────────────────────────────── */

static void conn_write(fluxipc_conn_t *c)
{
    while (c->woff < c->wlen) {
        ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->woff += (size_t)n;
            c->last_active_ms = now_ms();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        conn_close(c);
        return;
    }
    conn_close(c);   /* one response per connection */
}

static void conn_read(fluxipc_ctx_t *ctx, fluxipc_conn_t *c)
{
    int peer_closed = 0;
    int got_data = 0;

    for (;;) {
        if (c->rlen == c->rcap) {
            if (c->rcap >= FLUXIPC_RBUF_MAX) break;
            size_t ncap = c->rcap * 2;
            if (ncap > FLUXIPC_RBUF_MAX) ncap = FLUXIPC_RBUF_MAX;
            char *nb = realloc(c->rbuf, ncap);
            if (!nb) { conn_close(c); return; }
            c->rbuf = nb;
            c->rcap = ncap;
        }
        ssize_t n = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
        if (n > 0) { c->rlen += (size_t)n; got_data = 1; continue; }
        if (n == 0) { peer_closed = 1; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        conn_close(c);
        return;
    }

    if (got_data) c->last_active_ms = now_ms();

    int rc = sock_conn_process(ctx, c);
    if (rc < 0 || (rc == 0 && peer_closed)) { conn_close(c); return; }
    if (rc == 1) conn_write(c);   /* best-effort immediate flush */
}

/* ─── event loop ────────────────────────────────────────────────────────── */

/** Milliseconds until the next idle deadline, or UINT64_MAX if none. */
static uint64_t conn_idle_wait_ms(void)
{
    uint64_t now = now_ms();
    uint64_t wait = UINT64_MAX;

    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd < 0) continue;

        uint64_t elapsed = now >= c->last_active_ms
                             ? now - c->last_active_ms : 0;
        uint64_t rem = elapsed >= FLUXIPC_IDLE_TIMEOUT_MS
                         ? 0 : FLUXIPC_IDLE_TIMEOUT_MS - elapsed;
        if (rem < wait) wait = rem;
    }
    return wait;
}

static void conn_sweep_idle(void)
{
    uint64_t now = now_ms();
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd < 0) continue;
        if (now >= c->last_active_ms &&
            now - c->last_active_ms >= FLUXIPC_IDLE_TIMEOUT_MS)
            conn_close(c);
    }
}

/**
 * Effective wait: min(timeout, idle deadline).  UINT64_MAX means "infinite".
 * Without an eventfd we cannot be woken, so cap an otherwise-infinite wait at
 * CONN_POLL_MS to keep fluxipc_stop() reasonably prompt.
 */
static uint64_t conn_effective_wait_ms(fluxipc_ctx_t *ctx, int timeout_ms,
                                       uint64_t idle_wait)
{
    uint64_t wait;
    if (timeout_ms < 0) {
        wait = idle_wait;
    } else {
        wait = (uint64_t)timeout_ms;
        if (idle_wait != UINT64_MAX && idle_wait < wait) wait = idle_wait;
    }

    if (wait == UINT64_MAX && ctx->wake_fd < 0) wait = CONN_POLL_MS;
    return wait;
}

int conn_poll(fluxipc_ctx_t *ctx, int timeout_ms)
{
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = -1;

    if (ctx->server_fd >= 0) {
        FD_SET(ctx->server_fd, &rfds);
        maxfd = ctx->server_fd;
    }
    if (ctx->wake_fd >= 0) {
        FD_SET(ctx->wake_fd, &rfds);
        if (ctx->wake_fd > maxfd) maxfd = ctx->wake_fd;
    }
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd < 0) continue;
        if (c->state == FLUXIPC_CONN_READING) FD_SET(c->fd, &rfds);
        else                                  FD_SET(c->fd, &wfds);
        if (c->fd > maxfd) maxfd = c->fd;
    }

    struct timeval tv;
    struct timeval *tvp = NULL;

    uint64_t wait = conn_effective_wait_ms(ctx, timeout_ms,
                                           conn_idle_wait_ms());
    if (wait != UINT64_MAX) {
        tv.tv_sec  = (time_t)(wait / 1000);
        tv.tv_usec = (suseconds_t)((wait % 1000) * 1000);
        tvp = &tv;
    }

    int ret = select(maxfd + 1, &rfds, &wfds, NULL, tvp);
    if (ret < 0) { if (errno == EINTR) return 0; return -errno; }
    if (ret == 0) {
        conn_sweep_idle();
        return 0;
    }

    if (ctx->wake_fd >= 0 && FD_ISSET(ctx->wake_fd, &rfds)) {
        uint64_t v;
        ssize_t n = read(ctx->wake_fd, &v, sizeof(v));
        (void)n;
    }

    conn_sweep_idle();

    if (ctx->server_fd >= 0 && FD_ISSET(ctx->server_fd, &rfds))
        accept_unix(ctx);

    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd < 0) continue;
        if (c->state == FLUXIPC_CONN_READING) {
            if (FD_ISSET(c->fd, &rfds)) conn_read(ctx, c);
        } else {
            if (FD_ISSET(c->fd, &wfds)) conn_write(c);
        }
    }
    return 1;
}

/* ── test-only hooks (see fluxipc_internal.h) ─────────────────────────────── */
#ifdef FLUXIPC_TESTING
fluxipc_conn_t *fluxipc_ut_conn(int idx)
{
    if (idx < 0 || idx >= FLUXIPC_MAX_CONNS) return NULL;
    return &g_conns[idx];
}

int fluxipc_ut_conn_alloc(int fd)
{
    fluxipc_conn_t *c = conn_alloc(fd);
    return c ? (int)(c - g_conns) : -1;
}

void fluxipc_ut_conn_read(fluxipc_ctx_t *ctx, fluxipc_conn_t *c)
{
    conn_read(ctx, c);
}

void fluxipc_ut_conn_write(fluxipc_conn_t *c)
{
    conn_write(c);
}

int fluxipc_ut_pool_size(void)
{
    return FLUXIPC_MAX_CONNS;
}
#endif
