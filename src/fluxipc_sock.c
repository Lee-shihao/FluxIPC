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
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

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
    return conn_respond_own(c, buf, total);
}

static int send_json_error(fluxipc_conn_t *c, int rc,
                           const char *path, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;
    cJSON_AddStringToObject(o, "error", msg);
    cJSON_AddNumberToObject(o, "code", rc);
    if (path) cJSON_AddStringToObject(o, "path", path);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return -1;

    int r = send_unix_response(c, rc, s, (uint32_t)strlen(s));
    free(s);
    return r < 0 ? -1 : 1;
}

/* FLUXIPC_OP_CALL_JSON: method\0 followed by a JSON body, key=value pairs,
 * or positional arguments bound to the leaf's JSON Schema properties. */
static int sock_conn_json(fluxipc_ctx_t *ctx, fluxipc_conn_t *c,
                          const char *path, int argc, char **argv)
{
    const char *method = "GET";
    int first = 0;

    if (argc > 0 && fluxipc_method_to_mask(argv[0]) != 0) {
        method = argv[0];
        first  = 1;
    }

    const char *body = NULL;
    size_t      body_len = 0;
    cJSON      *parsed = NULL;
    fluxipc_kv_t query[FLUXIPC_MAX_ARGS];
    size_t       query_count = 0;

    if (argc - first == 1 &&
        (argv[first][0] == '{' || argv[first][0] == '[')) {
        body     = argv[first];
        body_len = strlen(body);
        parsed   = cJSON_ParseWithLength(body, body_len);
        if (!parsed)
            return send_json_error(c, -EINVAL, path, "invalid JSON body");
    } else if (argc > first) {
        int named = 0, positional = 0;
        for (int i = first; i < argc; i++) {
            if (strchr(argv[i], '=')) named++;
            else                      positional++;
        }

        if (named && positional)
            return send_json_error(c, -EINVAL, path,
                                   "cannot mix key=value and positional arguments");

        if (named) {
            for (int i = first; i < argc; i++) {
                char *eq = strchr(argv[i], '=');
                if (!eq || eq == argv[i])
                    return send_json_error(c, -EINVAL, path,
                                           "expected key=value");
                *eq = '\0';
                query[query_count].key   = argv[i];
                query[query_count].value = eq + 1;
                query_count++;
            }
        } else {
            int brc = fluxipc_json_bind_positional(ctx, path, argc - first,
                                                   argv + first, &parsed);
            if (brc < 0) {
                const char *msg = brc == -E2BIG
                    ? "too many positional arguments for the endpoint schema"
                    : "positional arguments require a JSON schema with properties; "
                      "use key=value or a JSON body";
                return send_json_error(c, brc, path, msg);
            }
        }
    }

    char  *out = NULL;
    size_t out_len = 0;
    int rc = fluxipc_dispatch_json(ctx, path, method,
                                   query, query_count,
                                   body, body_len, parsed,
                                   &out, &out_len);
    cJSON_Delete(parsed);

    int r = send_unix_response(c, rc, out, (uint32_t)out_len);
    free(out);
    return r < 0 ? -1 : 1;
}

int sock_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c)
{
    if (c->rlen < sizeof(fluxipc_req_hdr_t)) return 0;
    if (c->need && c->rlen < c->need) return 0;

    fluxipc_req_hdr_t h;
    memcpy(&h, c->rbuf, sizeof(h));
    if (h.magic != FLUXIPC_PROTO_MAGIC)        return -1;
    if (h.path_len >= FLUXIPC_PATH_MAX)        return -1;
    if (h.arg_len > IPC_ARG_MAX)               return -1;

    size_t total = sizeof(h) + h.path_len + h.arg_len;
    if (total > FLUXIPC_RBUF_MAX) return -1;
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
    if (h.op != FLUXIPC_OP_CALL && h.op != FLUXIPC_OP_CALL_JSON)
        return -1;

    char path[FLUXIPC_PATH_MAX];
    memcpy(path, c->rbuf + sizeof(h), h.path_len);
    path[h.path_len] = '\0';

    char *argv[FLUXIPC_MAX_ARGS];
    int   argc = 0;
    const char *p   = c->rbuf + sizeof(h) + h.path_len;
    const char *end = p + h.arg_len;
    while (p < end && argc < FLUXIPC_MAX_ARGS) {
        const char *nul = memchr(p, '\0', (size_t)(end - p));
        if (!nul) return -1;
        argv[argc++] = (char *)p;
        p = nul + 1;
    }

    if (h.op == FLUXIPC_OP_CALL_JSON)
        return sock_conn_json(ctx, c, path, argc, argv);

    /* ── FLUXIPC_OP_CALL: invoke the leaf's IPC handler ── */
    if (!ctx->ipc_out) {
        ctx->ipc_out = malloc(IPC_OUT_CAP);
        if (!ctx->ipc_out) return -1;
    }
    size_t out_len = 0;
    int rc = fluxipc_dispatch_ipc(ctx, path, argc, argv,
                                  ctx->ipc_out, IPC_OUT_CAP, &out_len);
    if (rc < 0) out_len = 0;

    int r = send_unix_response(c, rc, ctx->ipc_out, (uint32_t)out_len);
    return r < 0 ? -1 : 1;
}

/* ─── Client helpers ──────────────────────────────────────────────────────── */

static int send_iov_all(int fd, struct iovec *iov, int iovcnt)
{
    while (iovcnt > 0) {
        struct msghdr msg = {
            .msg_iov    = iov,
            .msg_iovlen = (size_t)iovcnt,
        };
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        if (n == 0) return -EPIPE;

        while (iovcnt > 0 && (size_t)n >= iov->iov_len) {
            n -= (ssize_t)iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0) {
            iov->iov_base = (char *)iov->iov_base + n;
            iov->iov_len -= (size_t)n;
        }
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
    if (argc < 0 || argc > FLUXIPC_MAX_ARGS) return -EINVAL;

    size_t plen = path ? strlen(path) : 0;
    size_t arg_total = 0;
    for (int i = 0; i < argc; i++) arg_total += strlen(argv[i]) + 1;

    fluxipc_req_hdr_t req = {
        .magic    = FLUXIPC_PROTO_MAGIC,
        .op       = op,
        .path_len = (uint16_t)plen,
        .arg_len  = (uint32_t)arg_total,
    };

    struct iovec iov[FLUXIPC_MAX_ARGS + 2];
    int iovcnt = 0;
    iov[iovcnt].iov_base = &req;
    iov[iovcnt].iov_len  = sizeof(req);
    iovcnt++;
    if (plen > 0) {
        iov[iovcnt].iov_base = (void *)path;
        iov[iovcnt].iov_len  = plen;
        iovcnt++;
    }
    for (int i = 0; i < argc; i++) {
        iov[iovcnt].iov_base = argv[i];
        iov[iovcnt].iov_len  = strlen(argv[i]) + 1;
        iovcnt++;
    }

    return send_iov_all(fd, iov, iovcnt);
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

int fluxipc_call_json(const char *sock_path, const char *ipc_path,
                      const char *method,
                      int paramc, char **params,
                      char **out, size_t *out_len)
{
    if (out)     *out = NULL;
    if (out_len) *out_len = 0;

    if (!out || paramc < 0 || paramc + 1 > FLUXIPC_MAX_ARGS)
        return -EINVAL;

    char method_buf[8];
    size_t mi = 0;
    if (method && method[0]) {
        for (; method[mi] && mi < sizeof(method_buf) - 1; mi++)
            method_buf[mi] = (char)toupper((unsigned char)method[mi]);
        method_buf[mi] = '\0';
        if (fluxipc_method_to_mask(method_buf) == 0) return -EINVAL;
    } else {
        snprintf(method_buf, sizeof(method_buf), "GET");
    }

    char *wire[FLUXIPC_MAX_ARGS];
    int n = 0;
    wire[n++] = method_buf;
    for (int i = 0; i < paramc; i++) {
        if (!params || !params[i]) return -EINVAL;
        wire[n++] = params[i];
    }

    return sock_call_alloc(sock_path, FLUXIPC_OP_CALL_JSON, ipc_path,
                           n, wire, out, out_len);
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
