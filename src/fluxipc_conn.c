/**
 * fluxipc_conn.c – unified non-blocking connection layer
 *
 * One single-threaded select() loop drives both listeners (Unix socket and
 * optional HTTP) and every active connection.  Both transports share the same
 * buffered, non-blocking read / queued-write state machine, so a slow or
 * malicious peer can never block the server.
 *
 * Lifecycle of a connection:
 *   accept → READING (accumulate request) → process → WRITING (flush response)
 *          → close (exactly one request/response per connection)
 */

#include "fluxipc_internal.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define CONN_IO_CHUNK 8192
#define CONN_POLL_MS  10

static fluxipc_conn_t g_conns[FLUXIPC_MAX_CONNS];

/* ─── helpers ───────────────────────────────────────────────────────────── */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -errno : 0;
}

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
    c->need = c->hdr_end = c->body_len = 0;
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

static fluxipc_conn_t *conn_alloc(int fd, fluxipc_conn_kind_t kind)
{
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd >= 0) continue;
        memset(c, 0, sizeof(*c));
        c->fd   = fd;
        c->kind = kind;
        c->state = FLUXIPC_CONN_READING;
        c->rbuf = malloc(CONN_IO_CHUNK);
        if (!c->rbuf) { c->fd = -1; return NULL; }
        c->rcap = CONN_IO_CHUNK;
        return c;
    }
    return NULL;   /* pool exhausted */
}

int conn_respond(fluxipc_conn_t *c, const void *data, size_t len)
{
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

/* ─── accept ────────────────────────────────────────────────────────────── */

static void accept_unix(fluxipc_ctx_t *ctx)
{
    for (;;) {
        struct sockaddr_un peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(ctx->server_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) return;
        set_nonblocking(fd);
        if (!conn_alloc(fd, FLUXIPC_CONN_UNIX)) close(fd);
    }
}

static void accept_http(fluxipc_ctx_t *ctx)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(ctx->http_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) return;
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (!conn_alloc(fd, FLUXIPC_CONN_HTTP)) close(fd);
    }
}

/* ─── read / write ──────────────────────────────────────────────────────── */

static void conn_write(fluxipc_conn_t *c)
{
    while (c->woff < c->wlen) {
        ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff,
                         MSG_NOSIGNAL);
        if (n > 0) { c->woff += (size_t)n; continue; }
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
        if (n > 0) { c->rlen += (size_t)n; continue; }
        if (n == 0) { peer_closed = 1; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        conn_close(c);
        return;
    }

    int rc = (c->kind == FLUXIPC_CONN_UNIX)
               ? sock_conn_process(ctx, c)
               : http_conn_process(ctx, c);

    if (rc < 0 || (rc == 0 && peer_closed)) { conn_close(c); return; }
    if (rc == 1) conn_write(c);   /* best-effort immediate flush */
}

/* ─── event loop ────────────────────────────────────────────────────────── */

int conn_poll(fluxipc_ctx_t *ctx)
{
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = -1;

    if (ctx->server_fd >= 0) {
        FD_SET(ctx->server_fd, &rfds);
        maxfd = ctx->server_fd;
    }
    if (ctx->http_fd >= 0) {
        FD_SET(ctx->http_fd, &rfds);
        if (ctx->http_fd > maxfd) maxfd = ctx->http_fd;
    }
    for (int i = 0; i < FLUXIPC_MAX_CONNS; i++) {
        fluxipc_conn_t *c = &g_conns[i];
        if (c->fd < 0) continue;
        if (c->state == FLUXIPC_CONN_READING) FD_SET(c->fd, &rfds);
        else                                  FD_SET(c->fd, &wfds);
        if (c->fd > maxfd) maxfd = c->fd;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = CONN_POLL_MS * 1000 };
    int ret = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if (ret < 0) { if (errno == EINTR) return 0; return -errno; }
    if (ret == 0) return 0;

    if (ctx->server_fd >= 0 && FD_ISSET(ctx->server_fd, &rfds))
        accept_unix(ctx);
    if (ctx->http_fd >= 0 && FD_ISSET(ctx->http_fd, &rfds))
        accept_http(ctx);

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

int fluxipc_ut_conn_alloc(int fd, int kind)
{
    fluxipc_conn_t *c = conn_alloc(fd, (fluxipc_conn_kind_t)kind);
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
