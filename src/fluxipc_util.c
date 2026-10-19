/**
 * fluxipc_util.c – pure, side-effect-free helpers shared by the transports.
 *
 * Kept separate from fluxipc_http.c so they can be unit-tested directly.
 */

#include "fluxipc_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** Decode %XX sequences into dst; always NUL-terminates. Returns length. */
size_t fluxipc_url_decode(const char *src, size_t slen, char *dst, size_t cap)
{
    size_t di = 0;
    if (!dst || cap == 0) return 0;
    for (size_t i = 0; i < slen && di + 1 < cap; i++) {
        if (src[i] == '%' && i + 2 < slen) {
            int h = hexval((unsigned char)src[i + 1]);
            int l = hexval((unsigned char)src[i + 2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        dst[di++] = src[i];
    }
    dst[di] = '\0';
    return di;
}

/**
 * Parse a URL query string into @p kv pairs.  Decoded keys/values are written
 * into @p arena.  Returns the number of pairs parsed.
 */
int fluxipc_parse_query(const char *raw, size_t rawlen,
                        fluxipc_kv_t *kv, int max_kv,
                        char *arena, size_t arena_cap)
{
    if (!raw || rawlen == 0) return 0;

    int    count = 0;
    size_t aused = 0;
    const char *p = raw, *end = raw + rawlen;

    while (p < end && count < max_kv) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *seg_end = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(seg_end - p));

        const char *kbeg = p;
        size_t klen = eq ? (size_t)(eq - p) : (size_t)(seg_end - p);
        const char *vbeg = eq ? eq + 1 : seg_end;
        size_t vlen = eq ? (size_t)(seg_end - (eq + 1)) : 0;

        if (aused + klen + vlen + 2 > arena_cap) break;
        char *kdst = arena + aused;
        size_t kd = fluxipc_url_decode(kbeg, klen, kdst, arena_cap - aused);
        aused += kd + 1;
        char *vdst = arena + aused;
        size_t vd = fluxipc_url_decode(vbeg, vlen, vdst, arena_cap - aused);
        aused += vd + 1;

        kv[count].key = kdst;
        kv[count].value = vdst;
        count++;

        p = amp ? amp + 1 : end;
    }
    return count;
}

/**
 * Locate Content-Length inside a request header block without mutating it.
 * @p hbytes is the offset of the terminating CRLFCRLF.
 */
size_t fluxipc_find_content_length(const char *buf, size_t hbytes)
{
    const char *end = buf + hbytes;
    const char *line = memmem(buf, hbytes, "\r\n", 2);
    if (!line) return 0;
    line += 2;

    while (line < end) {
        const char *eol = memmem(line, (size_t)(end - line) + 2, "\r\n", 2);
        if (!eol || eol > end) eol = end;
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t klen = (size_t)(colon - line);
            if (klen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                const char *v = colon + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t val = 0;
                for (; v < eol && *v >= '0' && *v <= '9'; v++)
                    val = val * 10 + (size_t)(*v - '0');
                return val;
            }
        }
        line = eol + 2;
    }
    return 0;
}


/* ─── Structured request helpers ──────────────────────────────────────────── */

static const cJSON *json_field(const fluxipc_json_request_t *req,
                               const char *key)
{
    if (!req || !key || !req->json) return NULL;
    return cJSON_GetObjectItemCaseSensitive(req->json, key);
}

static const char *query_field(const fluxipc_json_request_t *req,
                               const char *key)
{
    if (!req || !key) return NULL;
    for (size_t i = 0; i < req->query_count; i++)
        if (req->query[i].key && strcmp(req->query[i].key, key) == 0)
            return req->query[i].value;
    return NULL;
}

static int bool_from_string(const char *s, int def)
{
    if (!s) return def;
    if (strcasecmp(s, "true")  == 0 || strcasecmp(s, "yes") == 0 ||
        strcasecmp(s, "on")    == 0 || strcmp(s, "1") == 0)
        return 1;
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
        strcasecmp(s, "off")   == 0 || strcmp(s, "0") == 0)
        return 0;
    return def;
}

const char *fluxipc_json_str(const fluxipc_json_request_t *req,
                             const char *key, const char *def)
{
    const cJSON *v = json_field(req, key);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;

    const char *q = query_field(req, key);
    return q ? q : def;
}

long fluxipc_json_int(const fluxipc_json_request_t *req,
                      const char *key, long def)
{
    const cJSON *v = json_field(req, key);
    if (cJSON_IsNumber(v)) return (long)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring)
        return strtol(v->valuestring, NULL, 10);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;

    const char *q = query_field(req, key);
    return q ? strtol(q, NULL, 10) : def;
}

double fluxipc_json_double(const fluxipc_json_request_t *req,
                           const char *key, double def)
{
    const cJSON *v = json_field(req, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring)
        return strtod(v->valuestring, NULL);

    const char *q = query_field(req, key);
    return q ? strtod(q, NULL) : def;
}

int fluxipc_json_bool(const fluxipc_json_request_t *req,
                      const char *key, int def)
{
    const cJSON *v = json_field(req, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring)
        return bool_from_string(v->valuestring, def);

    const char *q = query_field(req, key);
    return q ? bool_from_string(q, def) : def;
}

int fluxipc_json_has(const fluxipc_json_request_t *req, const char *key)
{
    return json_field(req, key) != NULL || query_field(req, key) != NULL;
}

/* ─── Structured error response ───────────────────────────────────────────── */

static int status_from_code(int code)
{
    if (code > 0) return code;

    switch (-code) {
        case EINVAL:  return 400;
        case EACCES:
        case EPERM:   return 403;
        case ENOENT:  return 404;
        case EEXIST:
        case EBUSY:
        case EAGAIN:  return 409;
        case ERANGE:
        case E2BIG:   return 422;
        case ENOSYS:  return 501;
        default:      return 500;
    }
}

int fluxipc_json_error(fluxipc_json_response_t *resp, int code,
                       const char *msg)
{
    if (!resp) return -EINVAL;

    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;

    cJSON_AddStringToObject(o, "error", msg ? msg : "error");
    cJSON_AddNumberToObject(o, "code", code);

    cJSON_Delete(resp->json);
    resp->json   = o;
    resp->status = status_from_code(code);
    return 0;
}
