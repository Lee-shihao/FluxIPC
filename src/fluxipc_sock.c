/**
 * fluxipc_sock.c – Unix domain socket server/client
 *
 * Socket is created with mode 0666 so unprivileged clients can connect.
 * (umask is temporarily set to 0 around bind() to ensure this.)
 */

#include "fluxipc_internal.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -errno : 0;
}

/* ─── Server ──────────────────────────────────────────────────────────────── */

int sock_server_create(fluxipc_ctx_t *ctx)
{
    /* sock path: <run_dir>/<prog>.sock.
     * run_dir ≤ ~93 chars (e.g. /run/user/4294967295/63-char-prog-fluxipc),
     * prog_name ≤ 63 → total ≤ 161, well under FLUXIPC_PATH_MAX=256.
     * Silence the false-positive -Wformat-truncation. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(ctx->sock_path, sizeof(ctx->sock_path),
             "%s/%s.sock", ctx->run_dir, ctx->prog_name);
#pragma GCC diagnostic pop

    unlink(ctx->sock_path);

    ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_fd < 0) { perror("fluxipc: socket"); return -errno; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path is 108 bytes; our path "/run/<63>-fluxipc/<63>.sock" ≤ ~90 chars */
    /* sun_path is 108 bytes; sock_path = /run/<prog>-fluxipc/<prog>.sock ≤ ~90 chars */
    memcpy(addr.sun_path, ctx->sock_path,
           strnlen(ctx->sock_path, sizeof(addr.sun_path) - 1) + 1);

    /* Set umask=0 so the socket gets mode 0666 (world-connectable) */
    mode_t old_umask = umask(0);
    int rc = bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);

    if (rc < 0) {
        perror("fluxipc: bind");
        close(ctx->server_fd); ctx->server_fd = -1;
        return -errno;
    }
    /* chmod explicitly just in case */
    chmod(ctx->sock_path, 0666);

    if (listen(ctx->server_fd, 32) < 0) {
        perror("fluxipc: listen");
        close(ctx->server_fd); ctx->server_fd = -1;
        return -errno;
    }
    set_nonblocking(ctx->server_fd);
    return 0;
}

void sock_server_close(fluxipc_ctx_t *ctx)
{
    if (ctx->server_fd >= 0) { close(ctx->server_fd); ctx->server_fd = -1; }
    if (ctx->sock_path[0]) { unlink(ctx->sock_path); ctx->sock_path[0] = '\0'; }
}

/* ─── Full send/recv ──────────────────────────────────────────────────────── */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        if (n == 0) return -EPIPE;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        if (n == 0) return -ECONNRESET;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* ─── Request handler ─────────────────────────────────────────────────────── */

#define RESP_BUF_SZ (64 * 1024)

static void handle_client(fluxipc_ctx_t *ctx, int cfd)
{
    fluxipc_req_hdr_t req;
    if (recv_all(cfd, &req, sizeof(req)) < 0) goto done;
    if (req.magic != FLUXIPC_PROTO_MAGIC)     goto done;

    char *arg_buf = NULL;
    if (req.total_arg_len > 0) {
        arg_buf = malloc(req.total_arg_len + 1);
        if (!arg_buf || recv_all(cfd, arg_buf, req.total_arg_len) < 0) {
            free(arg_buf); goto done;
        }
        arg_buf[req.total_arg_len] = '\0';
    }

    char *argv_ptrs[FLUXIPC_MAX_ARGS];
    int   argc = 0;
    if (arg_buf && req.total_arg_len > 0) {
        char *p = arg_buf, *end = arg_buf + req.total_arg_len;
        while (p < end && argc < FLUXIPC_MAX_ARGS) {
            argv_ptrs[argc++] = p;
            p += strlen(p) + 1;
        }
    }

    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find(ctx->root, req.path);
    pthread_mutex_unlock(&ctx->tree_lock);

    fluxipc_resp_hdr_t resp;
    resp.magic   = FLUXIPC_PROTO_MAGIC;
    resp.version = FLUXIPC_PROTO_VERSION;

    char *out_buf = malloc(RESP_BUF_SZ);
    if (!out_buf) {
        resp.status = -ENOMEM; resp.data_len = 0;
    } else if (!node || !node->handler) {
        resp.status = -ENOENT; resp.data_len = 0;
    } else {
        size_t out_len = 0;
        int rc = node->handler(node->data, argc, argv_ptrs,
                               out_buf, RESP_BUF_SZ, &out_len);
        resp.status   = rc;
        resp.data_len = (rc == 0) ? (uint32_t)out_len : 0;
    }

    send_all(cfd, &resp, sizeof(resp));
    if (resp.data_len > 0 && out_buf)
        send_all(cfd, out_buf, resp.data_len);

    free(out_buf);
    free(arg_buf);
done:
    close(cfd);
}

/* ─── Poll ────────────────────────────────────────────────────────────────── */

int sock_server_poll(fluxipc_ctx_t *ctx)
{
    if (ctx->server_fd < 0) return -EBADF;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->server_fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };

    int ret = select(ctx->server_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) { if (errno == EINTR) return 0; return -errno; }
    if (ret == 0) return 0;

    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    int cfd = accept(ctx->server_fd, (struct sockaddr *)&peer, &peer_len);
    if (cfd < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;

    handle_client(ctx, cfd);
    return 1;
}

/* ─── Client call ─────────────────────────────────────────────────────────── */

int fluxipc_call(const char *sock_path, const char *ipc_path,
                 int argc, char **argv,
                 void *out_buf, size_t out_cap, size_t *out_len)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return -e;
    }

    size_t arg_total = 0;
    for (int i = 0; i < argc; i++) arg_total += strlen(argv[i]) + 1;

    fluxipc_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic         = FLUXIPC_PROTO_MAGIC;
    req.version       = FLUXIPC_PROTO_VERSION;
    req.argc          = argc;
    req.total_arg_len = (uint32_t)arg_total;
    snprintf(req.path, FLUXIPC_PATH_MAX, "%s", ipc_path);

    if (send_all(fd, &req, sizeof(req)) < 0) goto err;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (send_all(fd, argv[i], l) < 0) goto err;
    }

    fluxipc_resp_hdr_t resp;
    if (recv_all(fd, &resp, sizeof(resp)) < 0) goto err;
    if (resp.magic != FLUXIPC_PROTO_MAGIC) { close(fd); return -EPROTO; }

    if (resp.data_len > 0 && out_buf) {
        size_t to_read = resp.data_len < out_cap ? resp.data_len : out_cap;
        if (recv_all(fd, out_buf, to_read) < 0) goto err;
        if (out_len) *out_len = to_read;
        char drain[256];
        size_t rem = resp.data_len - to_read;
        while (rem > 0) {
            size_t n = rem < sizeof(drain) ? rem : sizeof(drain);
            recv_all(fd, drain, n); rem -= n;
        }
    } else if (out_len) {
        *out_len = 0;
    }

    close(fd);
    return resp.status;
err:
    close(fd); return -EIO;
}
