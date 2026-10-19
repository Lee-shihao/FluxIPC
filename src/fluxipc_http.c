/**
 * fluxipc_http.c – standard HTTP/1.1 interface for the FluxIPC namespace tree
 *
 *   GET    /<leaf>   → invoke handler (read)
 *   POST   /<leaf>   → invoke handler (write / action)
 *   PUT/PATCH/DELETE /<leaf>
 *   OPTIONS /<leaf>  → 204 + Allow (discover allowed methods)
 *   GET    /<container>      → JSON manifest of all leaves below it
 *   GET    /                 → manifest of the whole tree
 *   GET    /openapi.json     → OpenAPI 3.1 document for every leaf
 *   GET    /healthz          → liveness probe
 *
 * Uses the shared non-blocking connection layer (fluxipc_conn.c).  This file
 * only parses HTTP requests and builds HTTP responses into a byte buffer.
 */

#include "fluxipc_internal.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* ── tunables ───────────────────────────────────────────────────────────── */
#define HTTP_HDR_MAX     (16 * 1024)
#define HTTP_BODY_MAX    (256 * 1024)
#define HTTP_MAX_HEADERS 32
#define HTTP_MAX_QUERY   32

#ifdef HTTP_DEBUG
#  define HTTP_LOG(fmt, ...) \
    fprintf(stderr, "[http] %s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#  define HTTP_LOG(fmt, ...) do {} while (0)
#endif

/* ── growable byte buffer ───────────────────────────────────────────────── */

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} http_buf_t;

static int buf_reserve(http_buf_t *b, size_t extra)
{
    if (extra == 0) return 0;
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 512;
    while (ncap < b->len + extra) {
        if (ncap > ((size_t)-1) / 2) { ncap = b->len + extra; break; }
        ncap *= 2;
    }
    char *np = realloc(b->p, ncap);
    if (!np) return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

static int buf_append(http_buf_t *b, const void *data, size_t n)
{
    if (buf_reserve(b, n) < 0) return -1;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

static int buf_printf(http_buf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (buf_reserve(b, (size_t)n + 1) < 0) { va_end(ap2); return -1; }
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}

/* ── method helpers ─────────────────────────────────────────────────────── */

static uint32_t mask_from_request_method(const char *m)
{
    if (strcmp(m, "HEAD")    == 0) return FLUXIPC_M_GET;   /* served as GET */
    if (strcmp(m, "OPTIONS") == 0) return 0;
    return (uint32_t)fluxipc_method_to_mask(m);
}

static const char *status_reason(int code)
{
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

/* ── response construction (into a byte buffer) ─────────────────────────── */

static void build_response(http_buf_t *b, int status, const char *content_type,
                           const char *body, size_t body_len, int head_only,
                           const char *extra)
{
    buf_printf(b,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        status, status_reason(status),
        content_type ? content_type : "application/json; charset=utf-8",
        body_len, extra ? extra : "");
    if (!head_only && body && body_len > 0)
        buf_append(b, body, body_len);
}

static void send_problem(http_buf_t *b, int status, const char *title,
                         const char *detail, const char *instance,
                         int head_only, const char *extra)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "type", "about:blank");
    cJSON_AddStringToObject(o, "title", title ? title : status_reason(status));
    cJSON_AddNumberToObject(o, "status", status);
    if (detail)   cJSON_AddStringToObject(o, "detail", detail);
    if (instance) cJSON_AddStringToObject(o, "instance", instance);

    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        build_response(b, status, "application/problem+json; charset=utf-8",
                       s, strlen(s), head_only, extra);
        free(s);
    }
    cJSON_Delete(o);
}

static void send_json(http_buf_t *b, int status, cJSON *body, int head_only,
                      const char *extra)
{
    char *s = body ? cJSON_PrintUnformatted(body) : NULL;
    build_response(b, status, "application/json; charset=utf-8",
                   s ? s : "", s ? strlen(s) : 0, head_only, extra);
    free(s);
}

static int respond_problem(fluxipc_conn_t *c, int status, const char *title,
                           const char *detail, const char *instance)
{
    http_buf_t o = { 0 };
    send_problem(&o, status, title, detail, instance, 0, NULL);
    int r = conn_respond_own(c, o.p, o.len);
    return r < 0 ? -1 : 1;
}

/* ── Allow header construction ──────────────────────────────────────────── */

static void build_allow(uint32_t methods, int leaf, char *out, size_t cap)
{
    static const struct { uint32_t bit; const char *name; } tab[] = {
        { FLUXIPC_M_GET,    "GET"    },
        { FLUXIPC_M_POST,   "POST"   },
        { FLUXIPC_M_PUT,    "PUT"    },
        { FLUXIPC_M_PATCH,  "PATCH"  },
        { FLUXIPC_M_DELETE, "DELETE" },
    };
    size_t u = 0;
    out[0] = '\0';
    if (!leaf) {
        snprintf(out, cap, "GET, HEAD, OPTIONS");
        return;
    }
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!(methods & tab[i].bit)) continue;
        int n = snprintf(out + u, cap - u, "%s%s", u ? ", " : "", tab[i].name);
        if (n > 0) u += (size_t)n;
    }
    if (methods & FLUXIPC_M_GET) {
        int n = snprintf(out + u, cap - u, "%sHEAD", u ? ", " : "");
        if (n > 0) u += (size_t)n;
    }
    snprintf(out + u, cap - u, "%sOPTIONS", u ? ", " : "");
}

/* ── OpenAPI 3.1 generation ─────────────────────────────────────────────── */

typedef struct {
    cJSON *paths;
} openapi_ctx_t;

static void add_operation(cJSON *item, const char *verb,
                          const fluxipc_node_t *leaf)
{
    cJSON *op = cJSON_AddObjectToObject(item, verb);
    if (!op) return;
    cJSON_AddStringToObject(op, "operationId",
                            leaf->full_path[0] == '/' ? leaf->full_path + 1
                                                      : leaf->full_path);
    cJSON_AddStringToObject(op, "summary",
                            leaf->method.summary ? leaf->method.summary
                                                 : leaf->name);
    if (leaf->method.description)
        cJSON_AddStringToObject(op, "description", leaf->method.description);

    cJSON *rb = cJSON_AddObjectToObject(op, "requestBody");
    if (rb) {
        cJSON_AddBoolToObject(rb, "required", 0);
        cJSON *content = cJSON_AddObjectToObject(rb, "content");
        if (content) {
            cJSON *app = cJSON_AddObjectToObject(content, "application/json");
            if (app) {
                cJSON *schema = leaf->method.schema
                                  ? cJSON_Parse(leaf->method.schema) : NULL;
                cJSON_AddItemToObject(app, "schema",
                                      schema ? schema : cJSON_CreateObject());
            }
        }
    }
    cJSON *responses = cJSON_AddObjectToObject(op, "responses");
    if (responses) {
        cJSON *ok = cJSON_AddObjectToObject(responses, "200");
        if (ok) cJSON_AddStringToObject(ok, "description", "OK");
    }
}

static int openapi_leaf_cb(const fluxipc_node_t *leaf, void *user)
{
    openapi_ctx_t *oc = user;
    cJSON *item = cJSON_AddObjectToObject(oc->paths, leaf->full_path);
    if (!item) return 0;
    for (int i = 0; i < 5; i++) {
        uint32_t bit = 1u << i;
        if (!(leaf->method.methods & bit)) continue;
        const char *nm = fluxipc_mask_to_method(bit);
        if (!nm) continue;
        char lower[8];
        size_t k = 0;
        for (; nm[k] && k < sizeof(lower) - 1; k++)
            lower[k] = (char)tolower((unsigned char)nm[k]);
        lower[k] = '\0';
        add_operation(item, lower, leaf);
    }
    return 0;
}

static cJSON *build_openapi(void)
{
    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;
    cJSON_AddStringToObject(doc, "openapi", "3.1.0");
    cJSON *info = cJSON_AddObjectToObject(doc, "info");
    if (info) {
        cJSON_AddStringToObject(info, "title", "FluxIPC");
        cJSON_AddStringToObject(info, "version", "1.0.0");
    }
    cJSON *paths = cJSON_AddObjectToObject(doc, "paths");
    if (!paths) { cJSON_Delete(doc); return NULL; }

    if (g_ctx && g_ctx->root) {
        openapi_ctx_t oc = { paths };
        fluxipc_node_leaves(g_ctx->root, openapi_leaf_cb, &oc);
    }
    return doc;
}

/* ── request routing ────────────────────────────────────────────────────── */

static void route_request(fluxipc_ctx_t *ctx, http_buf_t *out,
                          const char *method, const char *target,
                          fluxipc_kv_t *headers, size_t header_count,
                          const char *content_type,
                          const char *body, size_t body_len)
{
    int head_only = (strcmp(method, "HEAD") == 0);
    const char *effective_method = head_only ? "GET" : method;

    const char *qmark = strchr(target, '?');
    size_t path_len = qmark ? (size_t)(qmark - target) : strlen(target);
    const char *qraw = qmark ? qmark + 1 : NULL;
    size_t qlen = qraw ? strlen(qraw) : 0;

    char raw_path[2048];
    if (path_len >= sizeof(raw_path)) path_len = sizeof(raw_path) - 1;
    memcpy(raw_path, target, path_len);
    raw_path[path_len] = '\0';

    char path[2048];
    fluxipc_url_decode(raw_path, path_len, path, sizeof(path));

    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = '\0';

    /* ── special built-in endpoints ── */
    if (strcmp(path, "/healthz") == 0 && !head_only) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "status", "ok");
            cJSON_AddStringToObject(o, "program",
                                    ctx->prog_name[0] ? ctx->prog_name : "fluxipc");
        }
        send_json(out, 200, o, head_only, NULL);
        cJSON_Delete(o);
        return;
    }
    if (strcmp(path, "/openapi.json") == 0) {
        if (strcmp(effective_method, "GET") != 0) {
            send_problem(out, 405, "Method Not Allowed", NULL, path, head_only,
                         "Allow: GET, HEAD, OPTIONS\r\n");
            return;
        }
        cJSON *doc = build_openapi();
        send_json(out, 200, doc, head_only, NULL);
        cJSON_Delete(doc);
        return;
    }

    /* ── resolve in tree ── */
    pthread_mutex_lock(&ctx->tree_lock);
    fluxipc_node_t *node = tree_find_path(ctx->root, path);
    pthread_mutex_unlock(&ctx->tree_lock);

    if (!node) {
        send_problem(out, 404, "Not Found", "no endpoint at this path",
                     path, head_only, NULL);
        return;
    }

    uint32_t want = mask_from_request_method(method);

    if (!node->is_leaf) {
        /* container: GET/HEAD → manifest; OPTIONS → 204; else 405 */
        if (strcmp(method, "OPTIONS") == 0) {
            build_response(out, 204, "application/json", NULL, 0, 1,
                           "Allow: GET, HEAD, OPTIONS\r\n");
            return;
        }
        if (strcmp(effective_method, "GET") != 0) {
            build_response(out, 405, "application/json", NULL, 0, head_only,
                           "Allow: GET, HEAD, OPTIONS\r\n");
            return;
        }
        int recursive = 1;
        char qarena[2048];
        fluxipc_kv_t qkv[HTTP_MAX_QUERY];
        int qn = fluxipc_parse_query(qraw, qlen, qkv, HTTP_MAX_QUERY,
                             qarena, sizeof(qarena));
        for (int i = 0; i < qn; i++)
            if (strcmp(qkv[i].key, "recursive") == 0 &&
                (strcmp(qkv[i].value, "0") == 0 ||
                 strcasecmp(qkv[i].value, "false") == 0))
                recursive = 0;
        cJSON *m = fluxipc_build_manifest(node, recursive, 1);
        send_json(out, 200, m, head_only, NULL);
        cJSON_Delete(m);
        return;
    }

    /* leaf */
    char allow[128];
    build_allow(node->method.methods, 1, allow, sizeof(allow));

    if (strcmp(method, "OPTIONS") == 0) {
        char extra[160];
        snprintf(extra, sizeof(extra), "Allow: %s\r\n", allow);
        build_response(out, 204, "application/json", NULL, 0, 1, extra);
        return;
    }
    if (want == 0 || !(node->method.methods & want)) {
        char extra[160];
        snprintf(extra, sizeof(extra), "Allow: %s\r\n", allow);
        send_problem(out, 405, "Method Not Allowed",
                     "method not allowed for this endpoint", path, head_only,
                     extra);
        return;
    }

    /* parse body as JSON when present */
    const cJSON *json = NULL;
    cJSON *parsed = NULL;
    if (body && body_len > 0) {
        int looks_json = (content_type &&
                          strstr(content_type, "json") != NULL) ||
                         body[0] == '{' || body[0] == '[';
        if (looks_json) {
            parsed = cJSON_ParseWithLength(body, body_len);
            if (!parsed) {
                send_problem(out, 400, "Bad Request",
                             "request body is not valid JSON", path, head_only,
                             NULL);
                return;
            }
            json = parsed;
        }
    }

    char qarena[2048];
    fluxipc_kv_t qkv[HTTP_MAX_QUERY];
    int qn = fluxipc_parse_query(qraw, qlen, qkv, HTTP_MAX_QUERY,
                         qarena, sizeof(qarena));

    fluxipc_json_request_t rq = {
        .method       = effective_method,
        .verb         = want,
        .path         = path,
        .node         = node,
        .query        = qkv,
        .query_count  = (size_t)qn,
        .headers      = headers,
        .header_count = header_count,
        .content_type = content_type,
        .body         = body,
        .body_len     = body_len,
        .json         = json,
        .data         = node->method.data,
    };
    fluxipc_json_response_t rs = { 0 };

    int rc = fluxipc_dispatch_http(ctx, &rq, &rs);

    if (rc < 0 && rs.json == NULL) {
        int status = (rc == -ENOENT) ? 404
                   : (rc == -ENOSYS) ? 501
                   : (rc == -EINVAL) ? 400
                   : 500;
        send_problem(out, status, status_reason(status), NULL, path, head_only,
                     NULL);
    } else {
        int status = rs.status ? rs.status : (rc < 0 ? 500 : 200);
        if (rs.json) {
            send_json(out, status, rs.json, head_only, NULL);
        } else {
            build_response(out, status,
                           rs.content_type ? rs.content_type
                                           : "application/json; charset=utf-8",
                           NULL, 0, head_only, NULL);
        }
    }

    cJSON_Delete(rs.json);
    cJSON_Delete(parsed);
}

/* ── request parsing ────────────────────────────────────────────────────── */

int http_conn_process(fluxipc_ctx_t *ctx, fluxipc_conn_t *c)
{
    char *he = memmem(c->rbuf, c->rlen, "\r\n\r\n", 4);
    if (!he) {
        if (c->rlen > HTTP_HDR_MAX)
            return respond_problem(c, 431, "Request Header Fields Too Large",
                                   "header section exceeds limit", NULL);
        return 0;
    }
    size_t hbytes = (size_t)(he - c->rbuf);
    if (hbytes > HTTP_HDR_MAX)
        return respond_problem(c, 431, "Request Header Fields Too Large",
                               "header section exceeds limit", NULL);

    size_t content_length = fluxipc_find_content_length(c->rbuf, hbytes);
    if (content_length > HTTP_BODY_MAX)
        return respond_problem(c, 413, "Content Too Large",
                               "request body exceeds limit", NULL);

    c->hdr_end  = hbytes;
    c->body_len = content_length;
    size_t body_off = hbytes + 4;
    if (c->rlen < body_off + content_length)
        return 0;   /* wait for the rest of the body */

    /* Full request present — safe to parse (this mutates the buffer). */
    char *le = memmem(c->rbuf, hbytes + 2, "\r\n", 2);
    if (!le)
        return respond_problem(c, 400, "Bad Request",
                               "malformed request line", NULL);
    *le = '\0';

    char method[16] = {0};
    char target[2048] = {0};
    if (sscanf(c->rbuf, "%15s %2047s", method, target) != 2)
        return respond_problem(c, 400, "Bad Request",
                               "malformed request line", NULL);

    fluxipc_kv_t headers[HTTP_MAX_HEADERS];
    size_t header_count = 0;
    const char *content_type = NULL;

    char *line = le + 2;
    while (line < he && header_count < HTTP_MAX_HEADERS) {
        char *eol = memmem(line, (size_t)(he - line) + 2, "\r\n", 2);
        if (!eol || eol > he) eol = he;
        *eol = '\0';
        if (*line == '\0') break;

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            char *vend = val + strlen(val);
            while (vend > val && (vend[-1] == '\r' || vend[-1] == ' '))
                *--vend = '\0';

            headers[header_count].key = line;
            headers[header_count].value = val;
            header_count++;

            if (strcasecmp(line, "Content-Type") == 0)
                content_type = val;
        }
        line = eol + 2;
    }

    const char *body = content_length ? c->rbuf + body_off : NULL;

    HTTP_LOG("%s %s body=%zu", method, target, content_length);

    http_buf_t out = { 0 };
    route_request(ctx, &out, method, target, headers, header_count,
                  content_type, body, content_length);
    int r = conn_respond_own(c, out.p, out.len);
    return r < 0 ? -1 : 1;
}

/* ── lifecycle (listener only) ──────────────────────────────────────────── */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -errno : 0;
}

int fluxipc_http_start(fluxipc_ctx_t *ctx, uint16_t port)
{
    if (!ctx || ctx->http_fd >= 0) return -EALREADY;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("fluxipc_http: socket"); return -errno; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("fluxipc_http: bind");
        close(fd);
        return -errno;
    }
    if (listen(fd, 32) < 0) {
        perror("fluxipc_http: listen");
        close(fd);
        return -errno;
    }
    set_nonblocking(fd);

    ctx->http_fd = fd;
    fprintf(stderr, "fluxipc: HTTP server listening on :%u\n"
                    "  GET  http://localhost:%u/          (tree manifest)\n"
                    "  GET  http://localhost:%u/openapi.json\n",
            port, port, port);
    return 0;
}

void fluxipc_http_stop(fluxipc_ctx_t *ctx)
{
    if (ctx && ctx->http_fd >= 0) {
        close(ctx->http_fd);
        ctx->http_fd = -1;
    }
}
