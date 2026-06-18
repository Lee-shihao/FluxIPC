/**
 * fluxipc_mcp.c – MCP (Model Context Protocol) server
 *
 * Transport: Streamable HTTP (MCP spec 2025-03-26+) with SSE backwards
 * compatibility (MCP spec 2024-11-05).  Both transports share a single
 * TCP listener on port 32100 so any compliant MCP host can connect:
 *
 *   Streamable HTTP  GET/POST /mcp      ← Hermes, Claude Desktop ≥ 0.9,
 *                                          Cursor, Windsurf, OpenClaw
 *   HTTP+SSE legacy  GET  /sse          ← older clients (spec 2024-11-05)
 *                    POST /messages     ← older clients
 *
 * JSON-RPC 2.0 methods handled:
 *   initialize                 → serverInfo + capabilities
 *   notifications/initialized  → notification, no response
 *   notifications/cancelled    → notification, no response
 *   ping                       → {}
 *   tools/list                 → all registered FluxIPC endpoints as tools
 *   tools/call                 → invoke handler, return text content
 *
 * Tool naming:  /system/info  →  system_info   (slashes → underscores)
 * Tool args:    {"args": ["a","b"]}  forwarded as argv[1..] to handler
 *               (argv[0] is ipc_path, matching the FluxIPC convention)
 *
 * No external dependencies; hand-rolled HTTP + JSON scanner/builder.
 * Single-threaded, non-blocking; integrates with the existing fluxipc_poll()
 * select loop.
 */

#include "fluxipc_internal.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

/* ── constants ──────────────────────────────────────────────────────────── */

#define MCP_PROTO_VER      "2025-03-26"   /* advertise latest */
#define MCP_PROTO_VER_OLD  "2024-11-05"   /* accept from clients */
#define MCP_SERVER_NAME    "FluxIPC"
#define MCP_SERVER_VER     "1.0.0"
#define MCP_PATH_MCP       "/mcp"          /* Streamable HTTP endpoint */
#define MCP_PATH_SSE       "/sse"          /* legacy SSE endpoint */
#define MCP_PATH_MESSAGES  "/messages"     /* legacy POST endpoint */
#define MCP_READ_BUF       (128 * 1024)
#define MCP_WRITE_BUF      (256 * 1024)
#define MCP_HANDLER_BUF    (64  * 1024)
#define MCP_MAX_ARGS       FLUXIPC_MAX_ARGS
#define MCP_MAX_CONNS      16
#define MCP_MAX_SESSIONS   32
#define MCP_SESSION_ID_LEN 32   /* hex characters */

/* ── DEBUG logging ─────────────────────────────────────────────────────── */
#ifdef MCP_DEBUG
#  define MCP_LOG(fmt, ...) \
    fprintf(stderr, "[mcp] %s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#  define MCP_LOG_RAW(fmt, ...) \
    fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#  define MCP_LOG(fmt, ...)       do {} while(0)
#  define MCP_LOG_RAW(fmt, ...)   do {} while(0)
#endif

/* ── connection types ───────────────────────────────────────────────────── */

typedef enum {
    CONN_HTTP_PENDING = 0,  /* reading HTTP request headers */
    CONN_HTTP_DONE,         /* synchronous POST – close after response */
    CONN_SSE_STREAM,        /* persistent SSE channel (GET /sse or GET /mcp) */
} conn_state_t;

/* Session state persisted across HTTP connections.
 * Streamable HTTP opens a new TCP connection per request, so session
 * state (including whether the handshake completed) lives here. */
typedef struct {
    char     id[MCP_SESSION_ID_LEN + 1];
    char     protocol_version[32];
    int      in_use;
    int      initialized;   /* received notifications/initialized */
    time_t   created_at;
} mcp_session_t;

static mcp_session_t mcp_sessions[MCP_MAX_SESSIONS];

typedef struct {
    int          fd;
    conn_state_t state;
    char        *rbuf;
    size_t       rlen;
    size_t       rcap;
    /* For legacy SSE: POST /messages needs to push response to the SSE fd */
    int          sse_fd;    /* -1 unless this is a /messages POST conn */
    char         session_id[MCP_SESSION_ID_LEN + 1]; /* hex session id */
} mcp_conn_t;

static mcp_conn_t mcp_conns[MCP_MAX_CONNS];

/* ── tiny I/O helpers ───────────────────────────────────────────────────── */

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        if (n == 0) return -EPIPE;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

/* ── minimal JSON scanner ───────────────────────────────────────────────── */

static const char *js_skip(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *js_str(const char *json, const char *key,
                           char *out, size_t out_sz)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    p = js_skip(p);
    if (*p != ':') return NULL;
    p = js_skip(p + 1);
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (*p == '"') ? p + 1 : NULL;
}

static int js_id(const char *json, char *out, size_t out_sz)
{
    const char *p = strstr(json, "\"id\"");
    if (!p) { snprintf(out, out_sz, "null"); return 0; }
    p += 4;
    p = js_skip(p);
    if (*p != ':') { snprintf(out, out_sz, "null"); return 0; }
    p = js_skip(p + 1);
    size_t i = 0;
    if (*p == '"') {
        out[i++] = '"'; p++;
        while (*p && *p != '"' && i + 2 < out_sz) out[i++] = *p++;
        if (*p == '"') out[i++] = '"';
    } else {
        while (*p && (isdigit((unsigned char)*p) || *p == '-') && i + 1 < out_sz)
            out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int js_args(const char *json, char **argv, int max_argc, char **arg_mem)
{
    *arg_mem = NULL;
    const char *p = strstr(json, "\"arguments\"");
    if (!p) return 0;
    p += 11;
    p = js_skip(p);
    if (*p != ':') return 0;
    p = js_skip(p + 1);

    const char *q = strstr(p, "\"args\"");
    if (!q) return 0;
    q += 6;
    q = js_skip(q);
    if (*q != ':') return 0;
    q = js_skip(q + 1);
    if (*q != '[') return 0;
    q++;

    const char *end = strchr(q, ']');
    if (!end) return 0;
    size_t region_len = (size_t)(end - q);
    char *buf = malloc(region_len + 1);
    if (!buf) return 0;
    memcpy(buf, q, region_len);
    buf[region_len] = '\0';
    *arg_mem = buf;

    int argc = 0;
    char *r = buf;
    while (*r && argc < max_argc) {
        r = (char *)js_skip(r);
        if (*r != '"') break;
        argv[argc++] = ++r;
        while (*r && *r != '"') {
            if (*r == '\\') {
                char *dst = r, *src = r + 1;
                switch (*src) {
                    case '"': case '\\': case '/': *dst = *src; break;
                    case 'n': *dst = '\n'; break;
                    case 'r': *dst = '\r'; break;
                    case 't': *dst = '\t'; break;
                    default:  *dst = *src; break;
                }
                memmove(r + 1, src + 1, strlen(src + 1) + 1);
            }
            r++;
        }
        if (*r == '"') *r++ = '\0';
        r = (char *)js_skip(r);
        if (*r == ',') r++;
    }
    return argc;
}

/* ── JSON builder helpers ───────────────────────────────────────────────── */

static int js_escape(char *buf, size_t cap, size_t *used, const char *s, size_t slen)
{
    int wrote = 0;
    for (size_t i = 0; i < slen && *used + 6 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  { buf[(*used)++] = '\\'; buf[(*used)++] = '"';  wrote += 2; }
        else if (c == '\\') { buf[(*used)++] = '\\'; buf[(*used)++] = '\\'; wrote += 2; }
        else if (c == '\n') { buf[(*used)++] = '\\'; buf[(*used)++] = 'n';  wrote += 2; }
        else if (c == '\r') { buf[(*used)++] = '\\'; buf[(*used)++] = 'r';  wrote += 2; }
        else if (c == '\t') { buf[(*used)++] = '\\'; buf[(*used)++] = 't';  wrote += 2; }
        else if (c < 0x20)  { int n = snprintf(buf + *used, cap - *used, "\\u%04x", c);
                               if (n > 0) { *used += (size_t)n; wrote += n; } }
        else                { buf[(*used)++] = (char)c; wrote++; }
    }
    return wrote;
}

#define BUF_APPEND(buf, cap, used, ...) \
    do { int _n = snprintf((buf)+(used), (cap)-(used), __VA_ARGS__); \
         if (_n > 0) (used) += (size_t)_n; } while(0)

/* ── tool name conversion ───────────────────────────────────────────────── */

static void path_to_name(const char *path, char *out, size_t sz)
{
    const char *p = path;
    if (*p == '/') p++;
    size_t i = 0;
    while (*p && i + 1 < sz) {
        out[i++] = (*p == '/') ? '_' : *p;
        p++;
    }
    out[i] = '\0';
}

static int name_to_path(fluxipc_ctx_t *ctx, const char *name,
                        char *out_path, size_t path_sz)
{
    if (!ctx->shm) return 0;
    pthread_rwlock_rdlock(&ctx->shm->lock);
    int found = 0;
    for (int i = 0; i < FLUXIPC_SHM_MAX_ENTRIES && !found; i++) {
        const fluxipc_shm_entry_t *e = &ctx->shm->entries[i];
        if (!(e->flags & FLUXIPC_FLAG_ACTIVE)) continue;
        char candidate[FLUXIPC_PATH_MAX];
        path_to_name(e->path, candidate, sizeof(candidate));
        if (strcmp(candidate, name) == 0) {
            snprintf(out_path, path_sz, "%s", e->path);
            found = 1;
        }
    }
    pthread_rwlock_unlock(&ctx->shm->lock);
    return found;
}

/* ── HTTP response helpers ──────────────────────────────────────────────── */

/*
 * send_http_json – send a complete HTTP/1.1 200 response with JSON body.
 * Used for synchronous POST /mcp responses and legacy POST /messages.
 */
static void send_http_json(int fd, const char *body, size_t body_len,
                           const char *session_id)
{
    char hdr[384];
    int hlen;
    if (session_id && session_id[0]) {
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Mcp-Session-Id: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            body_len, session_id);
    } else {
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n",
            body_len);
    }
    send_all(fd, hdr, (size_t)hlen);
    send_all(fd, body, body_len);
}

/*
 * send_http_accepted – 202 Accepted (no body), used when a POST /mcp contains
 * only notifications (no id), so no JSON-RPC response is warranted.
 */
static void send_http_accepted(int fd, const char *session_id)
{
    char resp[320];
    int len;
    if (session_id && session_id[0]) {
        len = snprintf(resp, sizeof(resp),
            "HTTP/1.1 202 Accepted\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Mcp-Session-Id: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            session_id);
    } else {
        len = snprintf(resp, sizeof(resp),
            "HTTP/1.1 202 Accepted\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n");
    }
    send_all(fd, resp, (size_t)len);
}

/*
 * send_http_sse_headers – open an SSE stream on fd.
 * For Streamable HTTP GET /mcp and legacy GET /sse.
 */
static void send_http_sse_headers(int fd)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    send_all(fd, hdr, strlen(hdr));
}

/*
 * send_http_head_ok – 200 OK with headers only (no body), used for HEAD /mcp
 * and HEAD /sse.  Returns the same Content-Type as a GET would.
 */
static void send_http_head_ok(int fd, const char *content_type)
{
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type);
    send_all(fd, hdr, (size_t)hlen);
}

/*
 * send_http_not_found – 404 for unknown paths.
 */
static void send_http_not_found(int fd)
{
    const char *resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    send_all(fd, resp, strlen(resp));
}

/*
 * send_http_options – CORS preflight response.
 */
static void send_http_options(int fd)
{
    const char *resp =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id\r\n"
        "Connection: close\r\n"
        "\r\n";
    send_all(fd, resp, strlen(resp));
}

/*
 * send_sse_event – write one SSE "data:" frame onto an open SSE stream.
 * payload must be a single-line JSON string (no embedded newlines).
 */
static void send_sse_event(int fd, const char *payload, size_t plen)
{
    /* "data: <json>\n\n" */
    const char prefix[] = "data: ";
    const char suffix[] = "\n\n";
    send_all(fd, prefix, sizeof(prefix) - 1);
    send_all(fd, payload, plen);
    send_all(fd, suffix, sizeof(suffix) - 1);
}

/*
 * For legacy SSE transport: the initial endpoint event tells the client
 * where to POST messages.
 */
static void send_sse_endpoint_event(int fd, uint16_t port)
{
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "event: endpoint\ndata: http://localhost:%u/messages\n\n",
                     port);
    send_all(fd, line, (size_t)n);
}

/* ── MCP JSON-RPC response builders ─────────────────────────────────────── */

/* Write a complete JSON-RPC success response into *buf (caller-allocated).
 * Returns the number of bytes written. */
static size_t build_result(char *buf, size_t cap,
                           const char *id, const char *result_json)
{
    size_t used = 0;
    BUF_APPEND(buf, cap, used,
               "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
               id, result_json);
    return used;
}

static size_t build_error(char *buf, size_t cap,
                          const char *id, int code, const char *msg)
{
    size_t used = 0;
    BUF_APPEND(buf, cap, used,
               "{\"jsonrpc\":\"2.0\",\"id\":%s,"
               "\"error\":{\"code\":%d,\"message\":\"",
               id, code);
    js_escape(buf, cap, &used, msg, strlen(msg));
    BUF_APPEND(buf, cap, used, "\"}}");
    return used;
}

/*
 * Deliver a JSON-RPC response to the right place:
 *   - Streamable HTTP POST /mcp  → send_http_json on the same fd
 *   - Legacy POST /messages      → SSE event on the paired sse_fd
 *   - SSE GET /mcp or GET /sse   → SSE event on fd itself
 */
static void deliver_response(mcp_conn_t *c, const char *payload, size_t plen)
{
    if (c->state == CONN_HTTP_DONE) {
        /* Streamable HTTP synchronous response */
        if (c->sse_fd >= 0) {
            /* Legacy: push to the open SSE channel */
            send_sse_event(c->sse_fd, payload, plen);
        } else {
            send_http_json(c->fd, payload, plen,
                           c->session_id[0] ? c->session_id : NULL);
        }
    } else {
        /* SSE stream: push as event */
        send_sse_event(c->fd, payload, plen);
    }
    MCP_LOG("deliver response len=%zu session=%.32s", plen,
            c->session_id[0] ? c->session_id : "(none)");
}

/* ── session management ─────────────────────────────────────────────────── */

static void gen_session_id(char *out, size_t out_sz)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char rnd[16];
        ssize_t n = read(fd, rnd, sizeof(rnd));
        close(fd);
        if (n == (ssize_t)sizeof(rnd)) {
            for (size_t i = 0; i < sizeof(rnd); i++)
                snprintf(out + i * 2, 3, "%02x", rnd[i]);
            return;
        }
    }
    /* fallback: time + pid + address-based pseudo-unique id */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out, out_sz, "%08x%08x%04x%08x",
             (uint32_t)getpid(),
             (uint32_t)ts.tv_sec,
             (uint16_t)(ts.tv_nsec & 0xFFFF),
             (uint32_t)(uintptr_t)&ts);
}

static mcp_session_t *session_create(const char *protocol_version)
{
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (!mcp_sessions[i].in_use) {
            mcp_session_t *s = &mcp_sessions[i];
            memset(s, 0, sizeof(*s));
            gen_session_id(s->id, sizeof(s->id));
            if (protocol_version && protocol_version[0])
                snprintf(s->protocol_version, sizeof(s->protocol_version),
                         "%s", protocol_version);
            else
                snprintf(s->protocol_version, sizeof(s->protocol_version),
                         "%s", MCP_PROTO_VER);
            s->in_use     = 1;
            s->created_at = time(NULL);
            MCP_LOG("session created id=%.32s proto=%s", s->id, s->protocol_version);
            return s;
        }
    }
    MCP_LOG("session table full (%d sessions)", MCP_MAX_SESSIONS);
    return NULL;
}

static mcp_session_t *session_find(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (mcp_sessions[i].in_use &&
            strncmp(mcp_sessions[i].id, id, MCP_SESSION_ID_LEN) == 0)
            return &mcp_sessions[i];
    }
    return NULL;
}

static void session_mark_initialized(mcp_session_t *s)
{
    if (s && !s->initialized) {
        s->initialized = 1;
        MCP_LOG("session %.32s marked initialized", s->id);
    }
}

/* ── MCP method handlers ────────────────────────────────────────────────── */

static void handle_initialize(mcp_conn_t *c, const char *id,
                               const char *json)
{
    /* Record client's protocol version (accept both old and new) */
    char client_ver[32] = {0};
    js_str(json, "protocolVersion", client_ver, sizeof(client_ver));

    /* Choose protocol version to advertise: match client if they're old */
    const char *use_ver = MCP_PROTO_VER;
    if (strncmp(client_ver, "2024-11-05", 10) == 0)
        use_ver = MCP_PROTO_VER_OLD;

    /* Create or reuse session.
     * If client sent an existing Mcp-Session-Id, reuse that session;
     * otherwise create a new one. */
    mcp_session_t *s = session_find(c->session_id);
    if (!s) {
        s = session_create(client_ver);
        if (!s) {
            char err[128];
            size_t elen = build_error(err, sizeof(err), id, -32603,
                                      "session table full");
            deliver_response(c, err, elen);
            return;
        }
        /* Store new session id on the connection so it goes into the
         * Mcp-Session-Id response header. */
        memcpy(c->session_id, s->id, sizeof(c->session_id));
        MCP_LOG("initialize new session %.32s (client proto=%s → use=%s)",
                s->id, client_ver, use_ver);
    } else {
        MCP_LOG("initialize reuse session %.32s (client proto=%s → use=%s)",
                s->id, client_ver, use_ver);
    }

    char result[512];
    snprintf(result, sizeof(result),
        "{"
            "\"protocolVersion\":\"%s\","
            "\"capabilities\":{\"tools\":{\"listChanged\":false}},"
            "\"serverInfo\":{\"name\":\"" MCP_SERVER_NAME "\","
                            "\"version\":\"" MCP_SERVER_VER "\"}"
        "}",
        use_ver);

    char buf[640];
    size_t len = build_result(buf, sizeof(buf), id, result);
    deliver_response(c, buf, len);
}

static void handle_ping(mcp_conn_t *c, const char *id)
{
    char buf[128];
    size_t len = build_result(buf, sizeof(buf), id, "{}");
    deliver_response(c, buf, len);
}

static void handle_tools_list(mcp_conn_t *c, const char *id,
                               fluxipc_ctx_t *ctx)
{
    char *buf = malloc(MCP_WRITE_BUF);
    if (!buf) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32603, "out of memory");
        deliver_response(c, err, elen);
        return;
    }

    size_t used = 0;
    /* Build result inline; we'll wrap it in build_result after */
    char *result = malloc(MCP_WRITE_BUF);
    if (!result) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32603, "out of memory");
        deliver_response(c, err, elen);
        free(buf);
        return;
    }
    size_t rused = 0;

    BUF_APPEND(result, MCP_WRITE_BUF, rused, "{\"tools\":[");

    if (ctx->shm) {
        pthread_rwlock_rdlock(&ctx->shm->lock);
        int first = 1;
        for (int i = 0; i < FLUXIPC_SHM_MAX_ENTRIES; i++) {
            const fluxipc_shm_entry_t *e = &ctx->shm->entries[i];
            if (!(e->flags & FLUXIPC_FLAG_ACTIVE)) continue;

            char tname[FLUXIPC_PATH_MAX];
            path_to_name(e->path, tname, sizeof(tname));

            if (!first) BUF_APPEND(result, MCP_WRITE_BUF, rused, ",");
            first = 0;

            BUF_APPEND(result, MCP_WRITE_BUF, rused, "{\"name\":\"");
            js_escape(result, MCP_WRITE_BUF, &rused, tname, strlen(tname));
            BUF_APPEND(result, MCP_WRITE_BUF, rused, "\",\"description\":\"");
            if (e->usage[0])
                js_escape(result, MCP_WRITE_BUF, &rused,
                          e->usage, strlen(e->usage));
            BUF_APPEND(result, MCP_WRITE_BUF, rused,
                "\","
                "\"inputSchema\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"args\":{"
                            "\"type\":\"array\","
                            "\"items\":{\"type\":\"string\"},"
                            "\"description\":"
                            "\"Positional arguments for this endpoint\""
                        "}"
                    "},"
                    "\"additionalProperties\":false"
                "}}");
        }
        pthread_rwlock_unlock(&ctx->shm->lock);
    }

    BUF_APPEND(result, MCP_WRITE_BUF, rused, "]}");
    used = build_result(buf, MCP_WRITE_BUF, id, result);
    free(result);

    deliver_response(c, buf, used);
    free(buf);
}

static void handle_tools_call(mcp_conn_t *c, const char *id,
                               const char *json, fluxipc_ctx_t *ctx)
{
    char tname[FLUXIPC_PATH_MAX];
    if (!js_str(json, "name", tname, sizeof(tname))) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32602, "missing name");
        deliver_response(c, err, elen);
        return;
    }

    char ipc_path[FLUXIPC_PATH_MAX];
    if (!name_to_path(ctx, tname, ipc_path, sizeof(ipc_path))) {
        char msg[FLUXIPC_PATH_MAX + 32];
        snprintf(msg, sizeof(msg), "unknown tool: %s", tname);
        char err[FLUXIPC_PATH_MAX + 64];
        size_t elen = build_error(err, sizeof(err), id, -32602, msg);
        deliver_response(c, err, elen);
        return;
    }

    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find(ctx->root, ipc_path);
    pthread_mutex_unlock(&ctx->tree_lock);

    if (!node || !node->handler) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32602, "handler not found");
        deliver_response(c, err, elen);
        return;
    }

    /* argv[0] = ipc_path (FluxIPC convention), user args from argv[1] */
    char *argv[MCP_MAX_ARGS + 1];
    char *arg_mem   = NULL;
    int   user_argc = js_args(json, argv + 1, MCP_MAX_ARGS, &arg_mem);
    argv[0] = ipc_path;
    int argc = user_argc + 1;

    char  *out_buf = malloc(MCP_HANDLER_BUF);
    size_t out_len = 0;
    int    status  = -ENOMEM;

    if (out_buf)
        status = node->handler(node->data, argc, argv,
                               out_buf, MCP_HANDLER_BUF, &out_len);
    free(arg_mem);

    /* Non-zero status with no output → protocol-level error */
    if (status != 0 && out_len == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "handler error %d", status);
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32603, msg);
        deliver_response(c, err, elen);
        free(out_buf);
        return;
    }

    /* Build result: {"content":[{"type":"text","text":"..."}],"isError":<bool>} */
    char   *resp = malloc(MCP_WRITE_BUF);
    size_t  used = 0;
    if (!resp) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32603, "out of memory");
        deliver_response(c, err, elen);
        free(out_buf);
        return;
    }

    char *result = malloc(MCP_WRITE_BUF / 2);
    size_t rused = 0;
    if (!result) {
        char err[128];
        size_t elen = build_error(err, sizeof(err), id, -32603, "out of memory");
        deliver_response(c, err, elen);
        free(out_buf);
        free(resp);
        return;
    }

    BUF_APPEND(result, MCP_WRITE_BUF / 2, rused,
               "{\"content\":[{\"type\":\"text\",\"text\":\"");
    js_escape(result, MCP_WRITE_BUF / 2, &rused,
              out_buf, out_len > 0 ? out_len : 0);
    BUF_APPEND(result, MCP_WRITE_BUF / 2, rused,
               "\"}],\"isError\":%s}", status == 0 ? "false" : "true");

    used = build_result(resp, MCP_WRITE_BUF, id, result);
    free(result);
    free(out_buf);

    deliver_response(c, resp, used);
    free(resp);
}

/* ── JSON-RPC dispatch ──────────────────────────────────────────────────── */

/* Returns 1 if a JSON-RPC response was delivered, 0 if this was a
 * notification (no id) and no response should be sent.  Callers use this
 * to know whether they must send an HTTP 202 Accepted for the POST. */
static int dispatch_jsonrpc(mcp_conn_t *c, char *body, fluxipc_ctx_t *ctx)
{
    char id[64];
    int has_id = js_id(body, id, sizeof(id));
    if (!has_id || id[0] == '\0') snprintf(id, sizeof(id), "null");

    char method[64];
    if (!js_str(body, "method", method, sizeof(method))) {
        MCP_LOG("no method in request id=%s", id);
        if (has_id) {
            char err[128];
            size_t elen = build_error(err, sizeof(err), id, -32600,
                                      "missing method");
            deliver_response(c, err, elen);
            return 1;
        }
        return 0;  /* notification without method – ignore */
    }

    MCP_LOG("dispatch method=%s id=%s session=%.32s",
            method, id, c->session_id[0] ? c->session_id : "(none)");

    /* ── session validation (everything except initialize) ── */
    if (strcmp(method, "initialize") != 0) {
        if (c->session_id[0] == '\0') {
            MCP_LOG("missing session for '%s' → -32001", method);
            if (has_id) {
                char err[128];
                size_t elen = build_error(err, sizeof(err), id, -32001,
                                "Missing Mcp-Session-Id header");
                deliver_response(c, err, elen);
                return 1;
            }
            return 0;
        }
        mcp_session_t *s = session_find(c->session_id);
        if (!s) {
            MCP_LOG("invalid session '%.32s' for '%s' → -32001",
                    c->session_id, method);
            if (has_id) {
                char err[128];
                size_t elen = build_error(err, sizeof(err), id, -32001,
                                "Invalid or expired session");
                deliver_response(c, err, elen);
                return 1;
            }
            return 0;
        }
        MCP_LOG("session %.32s valid (initialized=%d)",
                s->id, s->initialized);
    }

    if (strcmp(method, "initialize") == 0) {
        const char *params = strstr(body, "\"params\"");
        handle_initialize(c, id, params ? params : body);
        return 1;
    } else if (strcmp(method, "notifications/initialized") == 0) {
        mcp_session_t *s = session_find(c->session_id);
        session_mark_initialized(s);
        return 0;
    } else if (strcmp(method, "notifications/cancelled") == 0) {
        MCP_LOG("notification: cancelled (session=%.32s)", c->session_id);
        return 0;
    } else if (strcmp(method, "ping") == 0) {
        handle_ping(c, id);
        return 1;
    } else if (strcmp(method, "tools/list") == 0) {
        handle_tools_list(c, id, ctx);
        return 1;
    } else if (strcmp(method, "tools/call") == 0) {
        const char *params = strstr(body, "\"params\"");
        handle_tools_call(c, id, params ? params : body, ctx);
        return 1;
    } else {
        MCP_LOG("unknown method '%s'", method);
        if (has_id) {
            char err[128];
            size_t elen = build_error(err, sizeof(err), id, -32601,
                                      "method not found");
            deliver_response(c, err, elen);
            return 1;
        }
        return 0;
    }
}

/* ── HTTP request parser ─────────────────────────────────────────────────
 *
 * We only need to parse:
 *   - Request line:  METHOD  PATH  HTTP/1.x
 *   - Content-Length header
 *   - Mcp-Session-Id header (optional)
 *   - End of headers (\r\n\r\n), then body of Content-Length bytes
 *
 * Returns:
 *   1  headers complete, body extracted into *body_out (points inside buf)
 *   0  need more data
 *  -1  parse error
 */

static int http_parse(const char *buf, size_t len,
                      char method_out[16], char path_out[128],
                      const char **body_out, size_t *body_len_out,
                      char session_id_out[33])
{
    /* Find end of headers */
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            hdr_end = buf + i + 4;
            break;
        }
    }
    if (!hdr_end) return 0;  /* incomplete */

    /* Parse request line */
    const char *p = buf;
    size_t mi = 0;
    while (*p && *p != ' ' && mi + 1 < 16) method_out[mi++] = *p++;
    method_out[mi] = '\0';
    if (*p != ' ') return -1;
    p++;
    size_t pi = 0;
    while (*p && *p != ' ' && *p != '\r' && pi + 1 < 128) path_out[pi++] = *p++;
    path_out[pi] = '\0';

    /* Parse headers for Content-Length and Mcp-Session-Id */
    long content_length = 0;
    session_id_out[0] = '\0';

    const char *line = strchr(buf, '\n');
    while (line && line < hdr_end) {
        line++;
        if (strncasecmp(line, "content-length:", 15) == 0) {
            content_length = strtol(line + 15, NULL, 10);
        } else if (strncasecmp(line, "mcp-session-id:", 15) == 0) {
            const char *v = line + 15;
            while (*v == ' ') v++;
            size_t si = 0;
            while (*v && *v != '\r' && *v != '\n' && si + 1 < 33)
                session_id_out[si++] = *v++;
            session_id_out[si] = '\0';
        }
        line = strchr(line, '\n');
    }

    /* Check body is fully received */
    size_t headers_len = (size_t)(hdr_end - buf);
    size_t body_available = len - headers_len;
    if ((size_t)content_length > body_available) return 0; /* incomplete */

    *body_out     = hdr_end;
    *body_len_out = (size_t)content_length;
    return 1;
}

/* ── connection state machine ────────────────────────────────────────────── */

static mcp_conn_t *conn_alloc(int fd)
{
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        if (mcp_conns[i].fd <= 0) {
            mcp_conn_t *c = &mcp_conns[i];
            if (!c->rbuf) {
                c->rbuf = malloc(MCP_READ_BUF);
                if (!c->rbuf) return NULL;
                c->rcap = MCP_READ_BUF;
            }
            c->fd      = fd;
            c->state   = CONN_HTTP_PENDING;
            c->rlen    = 0;
            c->sse_fd  = -1;
            c->session_id[0] = '\0';
            return c;
        }
    }
    return NULL;
}

static void conn_close(mcp_conn_t *c)
{
    MCP_LOG("close fd=%d session=%.32s",
            c->fd, c->session_id[0] ? c->session_id : "(none)");
    if (c->fd > 0) { close(c->fd); c->fd = -1; }
    c->state  = CONN_HTTP_PENDING;
    c->rlen   = 0;
    c->sse_fd = -1;
    c->session_id[0] = '\0';
}

/*
 * Find the SSE stream connection with the matching session_id.
 * Used by legacy POST /messages to route responses.
 */
static mcp_conn_t *find_sse_conn(void)
{
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        if (mcp_conns[i].fd > 0 &&
            mcp_conns[i].state == CONN_SSE_STREAM &&
            mcp_conns[i].sse_fd < 0)   /* not itself a POST helper */
            return &mcp_conns[i];
    }
    return NULL;
}

/* Port stored here so SSE endpoint event can reference it */
static uint16_t g_mcp_port = 32100;

/*
 * Process a fully-received HTTP request on connection c.
 * body/body_len point into c->rbuf.
 */
static void handle_http_request(mcp_conn_t *c,
                                 const char *method, const char *path,
                                 const char *body, size_t body_len,
                                 fluxipc_ctx_t *ctx)
{
    MCP_LOG("%s %s body=%zub session=%.32s",
            method, path, body_len,
            c->session_id[0] ? c->session_id : "(none)");

    /* ── CORS preflight ── */
    if (strcmp(method, "OPTIONS") == 0) {
        send_http_options(c->fd);
        conn_close(c);
        return;
    }

    /* ── HEAD (health / probing) ── */
    if (strcmp(method, "HEAD") == 0) {
        if (strcmp(path, MCP_PATH_MCP) == 0 ||
            strcmp(path, MCP_PATH_SSE) == 0)
        {
            send_http_head_ok(c->fd, "text/event-stream");
        } else {
            send_http_not_found(c->fd);
        }
        conn_close(c);
        return;
    }

    /* ── Streamable HTTP: POST /mcp ── */
    if (strcmp(method, "POST") == 0 &&
        strcmp(path, MCP_PATH_MCP) == 0)
    {
        if (body_len == 0) {
            send_http_accepted(c->fd,
                c->session_id[0] ? c->session_id : NULL);
            conn_close(c);
            return;
        }
        /* body is the JSON-RPC request; copy so dispatch can mutate */
        char *msg = malloc(body_len + 1);
        if (!msg) { conn_close(c); return; }
        memcpy(msg, body, body_len);
        msg[body_len] = '\0';

        MCP_LOG("POST /mcp body: %s",
                body_len < 512 ? msg : "(>512 bytes, truncated)");
        if (body_len >= 512)
            MCP_LOG_RAW("[mcp] body[0:512]=%.512s\n", msg);

        c->state  = CONN_HTTP_DONE;
        c->sse_fd = -1;  /* synchronous: respond on same fd */

        int responded = dispatch_jsonrpc(c, msg, ctx);
        free(msg);
        if (!responded) {
            /* notification – no JSON-RPC response; send HTTP 202 Accepted */
            send_http_accepted(c->fd,
                c->session_id[0] ? c->session_id : NULL);
        }
        conn_close(c);
        return;
    }

    /* ── Streamable HTTP: GET /mcp (open SSE stream) ── */
    if (strcmp(method, "GET") == 0 &&
        strcmp(path, MCP_PATH_MCP) == 0)
    {
        send_http_sse_headers(c->fd);
        c->state = CONN_SSE_STREAM;
        /* Keep connection open; responses will be pushed as SSE events */
        return;
    }

    /* ── Legacy SSE: GET /sse ── */
    if (strcmp(method, "GET") == 0 &&
        strcmp(path, MCP_PATH_SSE) == 0)
    {
        send_http_sse_headers(c->fd);
        send_sse_endpoint_event(c->fd, g_mcp_port);
        c->state = CONN_SSE_STREAM;
        return;
    }

    /* ── Legacy SSE: POST /messages ── */
    if (strcmp(method, "POST") == 0 &&
        strcmp(path, MCP_PATH_MESSAGES) == 0)
    {
        if (body_len == 0) {
            send_http_accepted(c->fd,
                c->session_id[0] ? c->session_id : NULL);
            conn_close(c);
            return;
        }
        char *msg = malloc(body_len + 1);
        if (!msg) { conn_close(c); return; }
        memcpy(msg, body, body_len);
        msg[body_len] = '\0';

        /* Route response to the SSE stream */
        mcp_conn_t *sse = find_sse_conn();
        c->state  = CONN_HTTP_DONE;
        c->sse_fd = sse ? sse->fd : -1;

        dispatch_jsonrpc(c, msg, ctx);
        free(msg);

        /* Return 202 to the POST client */
        send_http_accepted(c->fd,
            c->session_id[0] ? c->session_id : NULL);
        conn_close(c);
        return;
    }

    /* ── Unknown path ── */
    send_http_not_found(c->fd);
    conn_close(c);
}

/*
 * conn_read – called when select() reports c->fd readable.
 * Accumulates data; fires handle_http_request when headers+body complete.
 */
static void conn_read(mcp_conn_t *c, fluxipc_ctx_t *ctx)
{
    /* SSE connections: client shouldn't send anything after the GET;
     * if they do (e.g. close notify) we just drain and let it close. */
    if (c->state == CONN_SSE_STREAM) {
        char drain[256];
        ssize_t n = recv(c->fd, drain, sizeof(drain), 0);
        if (n <= 0) conn_close(c);
        return;
    }

    size_t space = c->rcap - c->rlen - 1;
    if (space == 0) { conn_close(c); return; }

    ssize_t n = recv(c->fd, c->rbuf + c->rlen, space, 0);
    if (n <= 0) { conn_close(c); return; }
    c->rlen += (size_t)n;
    c->rbuf[c->rlen] = '\0';

    char   method[16], path[128], session_id[33];
    const char *body;
    size_t body_len;

    int r = http_parse(c->rbuf, c->rlen,
                       method, path, &body, &body_len, session_id);
    if (r == 0) return;   /* need more data */
    if (r < 0)  { conn_close(c); return; }

    if (session_id[0])
        memcpy(c->session_id, session_id, sizeof(c->session_id));

    handle_http_request(c, method, path, body, body_len, ctx);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int fluxipc_mcp_start(fluxipc_ctx_t *ctx, uint16_t port)
{
    if (!ctx || ctx->mcp_fd >= 0) return -EALREADY;

    /* Try IPv6 dual-stack first, fall back to IPv4 */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int ipv6 = (fd >= 0);
    if (!ipv6) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("fluxipc_mcp: socket"); return -errno; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (ipv6) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = {0};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        addr.sin6_addr   = in6addr_any;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("fluxipc_mcp: bind"); close(fd); return -errno;
        }
    } else {
        struct sockaddr_in addr = {0};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("fluxipc_mcp: bind"); close(fd); return -errno;
        }
    }

    if (listen(fd, 16) < 0) {
        perror("fluxipc_mcp: listen"); close(fd); return -errno;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    g_mcp_port  = port;
    ctx->mcp_fd = fd;

    fprintf(stderr,
        "fluxipc: MCP server listening on :%u\n"
        "  Streamable HTTP  →  http://localhost:%u/mcp\n"
        "  Legacy SSE       →  http://localhost:%u/sse\n",
        port, port, port);
    return 0;
}

int fluxipc_mcp_poll(fluxipc_ctx_t *ctx)
{
    if (!ctx || ctx->mcp_fd < 0) return 0;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->mcp_fd, &rfds);
    int maxfd = ctx->mcp_fd;

    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        if (mcp_conns[i].fd > 0) {
            FD_SET(mcp_conns[i].fd, &rfds);
            if (mcp_conns[i].fd > maxfd) maxfd = mcp_conns[i].fd;
        }
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return 0;

    /* Accept new connections */
    if (FD_ISSET(ctx->mcp_fd, &rfds)) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(ctx->mcp_fd, (struct sockaddr *)&peer, &plen);
        if (cfd >= 0) {
            char peer_str[64] = {0};
            if (peer.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&peer;
                inet_ntop(AF_INET, &sin->sin_addr, peer_str, sizeof(peer_str));
            } else if (peer.ss_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&peer;
                inet_ntop(AF_INET6, &sin6->sin6_addr, peer_str, sizeof(peer_str));
            }
            MCP_LOG("accept fd=%d from %s", cfd, peer_str[0] ? peer_str : "?");
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            setsockopt(cfd, SOL_SOCKET,  SO_KEEPALIVE, &one, sizeof(one));
            int flags = fcntl(cfd, F_GETFL, 0);
            fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
            if (!conn_alloc(cfd)) close(cfd);
        }
    }

    /* Service existing connections */
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        mcp_conn_t *c = &mcp_conns[i];
        if (c->fd > 0 && FD_ISSET(c->fd, &rfds))
            conn_read(c, ctx);
    }
    return 1;
}

void fluxipc_mcp_stop(fluxipc_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < MCP_MAX_CONNS; i++)
        if (mcp_conns[i].fd > 0) conn_close(&mcp_conns[i]);
    for (int i = 0; i < MCP_MAX_CONNS; i++) {
        free(mcp_conns[i].rbuf);
        mcp_conns[i].rbuf = NULL;
    }
    if (ctx->mcp_fd >= 0) {
        close(ctx->mcp_fd);
        ctx->mcp_fd = -1;
    }
}
