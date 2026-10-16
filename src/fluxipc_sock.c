/**
 * fluxipc_sock.c – Unix domain socket transport
 *
 * Listener setup plus the per-connection request processor used by the unified
 * event loop (fluxipc_conn.c), and the client-side call helpers.
 *
 * Wire protocol (see fluxipc_internal.h):
 *   request  = fluxipc_req_hdr_t + path[path_len] + argv[arg_len]
 *   response = fluxipc_resp_hdr_t + payload[data_len]
 */

#include "fluxipc_internal.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#define IPC_OUT_CAP (64 * 1024)
#define IPC_ARG_MAX (1024 * 1024)

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -errno : 0;
}

/* ─── Server listener ─────────────────────────────────────────────────────── */

int sock_server_create(fluxipc_ctx_t *ctx)
{
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
    memcpy(addr.sun_path, ctx->sock_path,
           strnlen(ctx->sock_path, sizeof(addr.sun_path) - 1) + 1);

    mode_t old_umask = umask(0);
    int rc = bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);

    if (rc < 0) {
        perror("fluxipc: bind");
        close(ctx->server_fd); ctx->server_fd = -1;
        return -errno;
    }
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

/* ─── Per-connection request processor ────────────────────────────────────── */

static int send_unix_response(fluxipc_conn_t *c, int32_t status,
                              const void *data, uint32_t len)
{
    fluxipc_resp_hdr_t h = {
        .magic    = FLUXIPC_PROTO_MAGIC,
        .status   = status,
        .data_len = len,
    };
    size_t total = sizeof(h) + len;
    char *buf = malloc(total);
    if (!buf) return -1;
    memcpy(buf, &h, sizeof(h));
    if (len) memcpy(buf + sizeof(h), data, len);
    int r = conn_respond(c, buf, total);
    free(buf);
    return r;
}

int sock_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c)
{
    if (c->rlen < sizeof(fluxipc_req_hdr_t)) return 0;

    fluxipc_req_hdr_t h;
    memcpy(&h, c->rbuf, sizeof(h));
    if (h.magic != FLUXIPC_PROTO_MAGIC)        return -1;
    if (h.path_len >= FLUXIPC_PATH_MAX)        return -1;
    if (h.arg_len > IPC_ARG_MAX)               return -1;

    size_t total = sizeof(h) + h.path_len + h.arg_len;
    c->need = total;
    if (c->rlen < total) return 0;

    /* ── FLUXIPC_OP_LIST: whole leaf manifest (JSON) ── */
    if (h.op == FLUXIPC_OP_LIST) {
        cJSON *m = fluxipc_build_manifest(ctx->root, 1, 0);
        if (!m) return -1;
        char *body = cJSON_PrintUnformatted(m);
        cJSON_Delete(m);
        if (!body) return -1;
        int r = send_unix_response(c, 0, body, (uint32_t)strlen(body));
        free(body);
        return r < 0 ? -1 : 1;
    }
    if (h.op != FLUXIPC_OP_CALL) return -1;

    /* ── FLUXIPC_OP_CALL: invoke the leaf's IPC handler ── */
    char path[FLUXIPC_PATH_MAX];
    memcpy(path, c->rbuf + sizeof(h), h.path_len);
    path[h.path_len] = '\0';

    char *argv[FLUXIPC_MAX_ARGS];
    int   argc = 0;
    const char *p   = c->rbuf + sizeof(h) + h.path_len;
    const char *end = p + h.arg_len;
    while (p < end && argc < FLUXIPC_MAX_ARGS) {
        argv[argc++] = (char *)p;
        p += strlen(p) + 1;
    }

    char *out = malloc(IPC_OUT_CAP);
    if (!out) return -1;
    size_t out_len = 0;
    int rc = fluxipc_dispatch_ipc(ctx, path, argc, argv,
                                  out, IPC_OUT_CAP, &out_len);
    if (rc < 0) out_len = 0;

    int r = send_unix_response(c, rc, out, (uint32_t)out_len);
    free(out);
    return r < 0 ? -1 : 1;
}

/* ─── Client helpers ──────────────────────────────────────────────────────── */

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

static int client_send_request(int fd, uint16_t op, const char *path,
                               int argc, char **argv)
{
    size_t plen = path ? strlen(path) : 0;
    size_t arg_total = 0;
    for (int i = 0; i < argc; i++) arg_total += strlen(argv[i]) + 1;

    fluxipc_req_hdr_t req = {
        .magic    = FLUXIPC_PROTO_MAGIC,
        .op       = op,
        .path_len = (uint16_t)plen,
        .arg_len  = (uint32_t)arg_total,
    };
    if (send_all(fd, &req, sizeof(req)) < 0) return -EIO;
    if (plen > 0 && send_all(fd, path, plen) < 0) return -EIO;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (send_all(fd, argv[i], l) < 0) return -EIO;
    }
    return 0;
}

static int client_connect(const char *sock_path)
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
    return fd;
}

int fluxipc_call(const char *sock_path, const char *ipc_path,
                 int argc, char **argv,
                 void *out_buf, size_t out_cap, size_t *out_len)
{
    int fd = client_connect(sock_path);
    if (fd < 0) return fd;

    if (client_send_request(fd, FLUXIPC_OP_CALL, ipc_path, argc, argv) < 0) {
        close(fd); return -EIO;
    }

    fluxipc_resp_hdr_t resp;
    if (recv_all(fd, &resp, sizeof(resp)) < 0) { close(fd); return -EIO; }
    if (resp.magic != FLUXIPC_PROTO_MAGIC) { close(fd); return -EPROTO; }

    if (resp.data_len > 0 && out_buf) {
        size_t to_read = resp.data_len < out_cap ? resp.data_len : out_cap;
        if (recv_all(fd, out_buf, to_read) < 0) { close(fd); return -EIO; }
        if (out_len) *out_len = to_read;
        char drain[256];
        size_t rem = resp.data_len - to_read;
        while (rem > 0) {
            size_t n = rem < sizeof(drain) ? rem : sizeof(drain);
            recv_all(fd, drain, n);
            rem -= n;
        }
    } else if (out_len) {
        *out_len = 0;
    }

    close(fd);
    return resp.status;
}

int sock_call_alloc(const char *sock_path, uint16_t op, const char *ipc_path,
                    int argc, char **argv, char **out, size_t *out_len)
{
    if (out)     *out = NULL;
    if (out_len) *out_len = 0;

    int fd = client_connect(sock_path);
    if (fd < 0) return fd;

    if (client_send_request(fd, op, ipc_path, argc, argv) < 0) {
        close(fd); return -EIO;
    }

    fluxipc_resp_hdr_t resp;
    if (recv_all(fd, &resp, sizeof(resp)) < 0) { close(fd); return -EIO; }
    if (resp.magic != FLUXIPC_PROTO_MAGIC) { close(fd); return -EPROTO; }

    char *buf = NULL;
    if (resp.data_len > 0) {
        buf = malloc((size_t)resp.data_len + 1);
        if (!buf) { close(fd); return -ENOMEM; }
        if (recv_all(fd, buf, resp.data_len) < 0) {
            free(buf); close(fd); return -EIO;
        }
        buf[resp.data_len] = '\0';
    }
    close(fd);

    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = buf ? resp.data_len : 0;
    return resp.status;
}
