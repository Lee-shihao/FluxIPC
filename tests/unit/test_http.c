/**
 * test_http.c – HTTP request parsing, routing and response building.
 *
 * Drives the real http_conn_process() over an in-memory fake connection so the
 * full parser/router/response path is exercised without any network I/O.
 */

#include "ut_common.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static int http_setup(void **state)
{
    (void)state;
    ut_server_up();
    return 0;
}

static int http_teardown(void **state)
{
    (void)state;
    ut_server_down();
    return 0;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int do_http(const char *raw, fluxipc_conn_t *c)
{
    ut_conn_init(c, raw, strlen(raw), FLUXIPC_CONN_HTTP);
    return http_conn_process(g_ctx, c);
}

static int resp_status(fluxipc_conn_t *c)
{
    int s = 0;
    if (c->wbuf) sscanf(c->wbuf, "HTTP/1.1 %d", &s);
    return s;
}

static int resp_has(fluxipc_conn_t *c, const char *needle)
{
    if (!c->wbuf) return 0;
    return memmem(c->wbuf, c->wlen, needle, strlen(needle)) != NULL;
}

/* ── parser ──────────────────────────────────────────────────────────────── */

static void test_need_more(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GET / HTTP/1.1\r\nHost: x\r\n", &c), 0);
    assert_null(c.wbuf);
    ut_conn_free(&c);
}

static void test_bad_request_line(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GARBAGE\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 400);
    assert_true(resp_has(&c, "application/problem+json"));
    ut_conn_free(&c);
}

static void test_unknown_path_404(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /no/such/path HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 404);
    assert_true(resp_has(&c, "\"status\":404"));
    assert_true(resp_has(&c, "\"instance\":\"/no/such/path\""));
    ut_conn_free(&c);
}

/* ── built-ins ───────────────────────────────────────────────────────────── */

static void test_healthz(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "\"status\":\"ok\""));
    ut_conn_free(&c);
}

static void test_openapi(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .summary = "sum", .description = "desc",
                           .schema = "{\"type\":\"object\"}",
                           .methods = FLUXIPC_M_GET | FLUXIPC_M_PUT };
    fluxipc_node_leaf(root, "thing", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /openapi.json HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "\"openapi\":\"3.1.0\""));
    assert_true(resp_has(&c, "\"/thing\""));
    assert_true(resp_has(&c, "\"get\""));
    assert_true(resp_has(&c, "\"put\""));
    ut_conn_free(&c);

    /* non-GET is rejected */
    assert_int_equal(do_http("POST /openapi.json HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 405);
    ut_conn_free(&c);
}

/* ── containers ──────────────────────────────────────────────────────────── */

static void test_container_manifest(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ut_ipc_echo };
    fluxipc_node_leaf(a, "x", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /a HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "\"kind\":\"collection\""));
    ut_conn_free(&c);

    /* OPTIONS → 204 with Allow */
    assert_int_equal(do_http("OPTIONS /a HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 204);
    assert_true(resp_has(&c, "Allow: GET, HEAD, OPTIONS"));
    ut_conn_free(&c);

    /* POST → 405 */
    assert_int_equal(do_http("POST /a HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 405);
    ut_conn_free(&c);
}

/* ── leaves ──────────────────────────────────────────────────────────────── */

static void test_leaf_methods(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .data = (void *)"H",
                           .methods = FLUXIPC_M_GET | FLUXIPC_M_DELETE };
    fluxipc_node_leaf(root, "leaf", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /leaf HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "\"handler\":\"H\""));
    ut_conn_free(&c);

    /* OPTIONS lists the allowed methods plus HEAD/OPTIONS */
    assert_int_equal(do_http("OPTIONS /leaf HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 204);
    assert_true(resp_has(&c, "Allow: GET, DELETE, HEAD, OPTIONS"));
    ut_conn_free(&c);

    /* disallowed method */
    assert_int_equal(do_http("POST /leaf HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 405);
    assert_true(resp_has(&c, "Allow:"));
    ut_conn_free(&c);
}

static void test_head_has_no_body(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .methods = FLUXIPC_M_GET };
    fluxipc_node_leaf(root, "h", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("HEAD /h HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    const char *b = strstr(c.wbuf, "\r\n\r\n");
    assert_non_null(b);
    /* header block is the whole response: no body bytes follow it */
    assert_int_equal(c.wlen, (size_t)(b - c.wbuf) + 4);
    ut_conn_free(&c);
}

static void test_json_body(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .methods = FLUXIPC_M_POST };
    fluxipc_node_leaf(root, "post", &m);

    const char *body = "{\"a\":1}";
    char req[512];
    snprintf(req, sizeof(req),
             "POST /post HTTP/1.1\r\nHost: x\r\n"
             "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(body), body);

    fluxipc_conn_t c;
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 200);
    ut_conn_free(&c);

    /* malformed JSON → 400 */
    const char *bad = "{not json";
    snprintf(req, sizeof(req),
             "POST /post HTTP/1.1\r\nHost: x\r\n"
             "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(bad), bad);
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 400);
    ut_conn_free(&c);
}

static void test_content_too_large(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    const char *req =
        "POST /post HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n";
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 413);
    ut_conn_free(&c);
}

/* ── extra handlers ──────────────────────────────────────────────────────── */

static int http_json_probe(void *data, const fluxipc_http_request_t *req,
                           fluxipc_http_response_t *resp)
{
    (void)data;
    resp->json = cJSON_CreateObject();
    if (resp->json) {
        cJSON_AddBoolToObject(resp->json, "has_json", req->json != NULL);
        cJSON_AddNumberToObject(resp->json, "verb", (double)req->verb);
        cJSON_AddNumberToObject(resp->json, "header_count",
                                (double)req->header_count);
        cJSON_AddNumberToObject(resp->json, "query_count",
                                (double)req->query_count);
    }
    return 0;
}

static int http_negative(void *data, const fluxipc_http_request_t *req,
                         fluxipc_http_response_t *resp)
{
    (void)req; (void)resp;
    return data ? *(int *)data : -1;
}

static int http_negative_with_json(void *data,
                                   const fluxipc_http_request_t *req,
                                   fluxipc_http_response_t *resp)
{
    (void)data; (void)req;
    resp->json = cJSON_CreateObject();
    if (resp->json) cJSON_AddStringToObject(resp->json, "err", "detail");
    return -1;
}

static int http_custom_status(void *data, const fluxipc_http_request_t *req,
                              fluxipc_http_response_t *resp)
{
    (void)req;
    resp->status = data ? *(int *)data : 0;
    resp->json = cJSON_CreateObject();
    return 0;
}

static int g_forced_status;
static int http_forced_status(void *data, const fluxipc_http_request_t *req,
                              fluxipc_http_response_t *resp)
{
    (void)data; (void)req;
    resp->status = g_forced_status;
    resp->json = cJSON_CreateObject();
    return 0;
}

static int http_empty_body(void *data, const fluxipc_http_request_t *req,
                           fluxipc_http_response_t *resp)
{
    (void)data; (void)req;
    resp->json = NULL;                      /* handler emits no JSON body */
    resp->content_type = "text/plain";
    return 0;
}

static int http_empty_default_ct(void *data, const fluxipc_http_request_t *req,
                                 fluxipc_http_response_t *resp)
{
    (void)data; (void)req;
    resp->json = NULL;
    resp->content_type = NULL;              /* default content-type */
    return 0;
}

/* ── method/Allow coverage ───────────────────────────────────────────────── */

static void test_all_method_allow(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    struct { const char *name; uint32_t m; const char *expect; } cases[] = {
        { "post",   FLUXIPC_M_POST,                  "Allow: POST, OPTIONS" },
        { "put",    FLUXIPC_M_PUT,                   "Allow: PUT, OPTIONS" },
        { "patch",  FLUXIPC_M_PATCH,                 "Allow: PATCH, OPTIONS" },
        { "del",    FLUXIPC_M_DELETE,                "Allow: DELETE, OPTIONS" },
        { "get",    FLUXIPC_M_GET,                   "Allow: GET, HEAD, OPTIONS" },
        { "all",    FLUXIPC_M_ALL,                   "Allow: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                               .methods = cases[i].m };
        fluxipc_node_leaf(root, cases[i].name, &m);

        fluxipc_conn_t c;
        char req[128];
        snprintf(req, sizeof(req),
                 "OPTIONS /%s HTTP/1.1\r\nHost: x\r\n\r\n", cases[i].name);
        assert_int_equal(do_http(req, &c), 1);
        assert_int_equal(resp_status(&c), 204);
        assert_true(resp_has(&c, cases[i].expect));
        ut_conn_free(&c);
    }
}

static void test_unknown_method(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok };
    fluxipc_node_leaf(root, "leaf", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("FOO /leaf HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 405);
    ut_conn_free(&c);
}

/* ── OpenAPI ─────────────────────────────────────────────────────────────── */

static void test_openapi_all_verbs(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .methods = FLUXIPC_M_ALL };
    fluxipc_node_leaf(root, "verbs", &m);   /* no summary/description/schema */

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /openapi.json HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    const char *verbs[] = { "get", "post", "put", "patch", "delete" };
    for (size_t i = 0; i < 5; i++)
        assert_true(resp_has(&c, verbs[i]));
    assert_true(resp_has(&c, "\"operationId\":\"verbs\""));
    assert_true(resp_has(&c, "\"summary\":\"verbs\""));  /* fallback to name */
    ut_conn_free(&c);
}

/* ── routing detail ──────────────────────────────────────────────────────── */

static void test_healthz_program_and_head(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"status\":\"ok\""));
    assert_true(resp_has(&c, g_ctx->prog_name));
    ut_conn_free(&c);

    /* HEAD /healthz bypasses the built-in and resolves in the tree → 404 */
    assert_int_equal(do_http("HEAD /healthz HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 404);
    ut_conn_free(&c);
}

static void test_container_recursive_query(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_node_t *b = fluxipc_node_mkdir(a, "b");
    fluxipc_method_t m = { .ipc = ut_ipc_echo };
    fluxipc_node_leaf(a, "direct", &m);
    fluxipc_node_leaf(b, "deep", &m);

    fluxipc_conn_t c;
    /* recursive=0 → 1 direct leaf */
    assert_int_equal(do_http("GET /a?recursive=0 HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"count\":1"));
    ut_conn_free(&c);

    /* recursive=false → same */
    assert_int_equal(do_http("GET /a?recursive=false HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"count\":1"));
    ut_conn_free(&c);

    /* default → 2 (recursive) */
    assert_int_equal(do_http("GET /a HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"count\":2"));
    ut_conn_free(&c);

    /* HEAD on a container → no body */
    assert_int_equal(do_http("HEAD /a HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    const char *b2 = strstr(c.wbuf, "\r\n\r\n");
    assert_int_equal(c.wlen, (size_t)(b2 - c.wbuf) + 4);
    ut_conn_free(&c);
}

static void test_verb_is_parsed_once(void **state)
{
    (void)state;
    fluxipc_method_t m = { .json = http_json_probe, .methods = FLUXIPC_M_ALL };
    fluxipc_node_leaf(fluxipc_tree_root(), "verb", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /verb HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"verb\":1"));
    ut_conn_free(&c);

    assert_int_equal(do_http("PUT /verb HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"verb\":4"));
    ut_conn_free(&c);
}

static void test_query_and_path_decode(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_node_t *a = fluxipc_node_mkdir(root, "a");
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = http_json_probe };
    fluxipc_node_leaf(a, "leaf", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /a/leaf?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "\"query_count\":2"));
    ut_conn_free(&c);

    /* trailing slash is normalised away */
    assert_int_equal(do_http("GET /a/leaf/ HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    ut_conn_free(&c);

    /* %2F decodes back to a path separator */
    assert_int_equal(do_http("GET /a%2Fleaf HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    ut_conn_free(&c);

    /* decoded NUL must not truncate the route */
    assert_int_equal(do_http("GET /a/leaf%00evil HTTP/1.1\r\nHost: x\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 400);
    ut_conn_free(&c);
}

static void test_body_parsing_variants(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = http_json_probe,
                           .methods = FLUXIPC_M_POST };
    fluxipc_node_leaf(root, "post", &m);

    fluxipc_conn_t c;
    /* real JSON → parsed */
    assert_int_equal(do_http("POST /post HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}", &c), 1);
    assert_true(resp_has(&c, "\"has_json\":true"));
    ut_conn_free(&c);

    /* array body is parsed too */
    assert_int_equal(do_http("POST /post HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]", &c), 1);
    assert_true(resp_has(&c, "\"has_json\":true"));
    ut_conn_free(&c);

    /* non-JSON content type / body → left raw */
    assert_int_equal(do_http("POST /post HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello", &c), 1);
    assert_true(resp_has(&c, "\"has_json\":false"));
    ut_conn_free(&c);
}

static void test_body_not_yet_received(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    /* claims 10 bytes of body but supplies none */
    assert_int_equal(do_http("POST /post HTTP/1.1\r\nContent-Length: 10\r\n\r\n", &c), 0);
    assert_null(c.wbuf);
    ut_conn_free(&c);
}

static void test_header_overflow(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = http_json_probe };
    fluxipc_node_leaf(root, "hdr", &m);

    char req[4096];
    int off = snprintf(req, sizeof(req), "GET /hdr HTTP/1.1\r\n");
    for (int i = 0; i < 40; i++)
        off += snprintf(req + off, sizeof(req) - (size_t)off,
                        "X-H%d: v\r\n", i);
    snprintf(req + off, sizeof(req) - (size_t)off, "\r\n");

    fluxipc_conn_t c;
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "\"header_count\":32"));  /* capped */
    ut_conn_free(&c);
}

static void test_header_too_large(void **state)
{
    (void)state;
    size_t n = 20 * 1024;
    char *req = malloc(n + 1);
    assert_non_null(req);
    memset(req, 'A', n);
    req[n] = '\0';                       /* no CRLFCRLF terminator */

    fluxipc_conn_t c;
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 431);
    ut_conn_free(&c);

    /* A complete header section over the limit is rejected too. */
    memcpy(req + n - 4, "\r\n\r\n", 4);
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 431);
    ut_conn_free(&c);

    free(req);
}

static void test_target_too_long(void **state)
{
    (void)state;
    size_t n = 3000;
    char *req = malloc(n + 64);
    assert_non_null(req);
    int off = snprintf(req, n + 64, "GET /");
    memset(req + off, 'a', n);
    const char *suffix = " HTTP/1.1\r\n\r\n";
    size_t slen = strlen(suffix);
    memcpy(req + off + n, suffix, slen);
    req[off + n + slen] = '\0';

    fluxipc_conn_t c;
    assert_int_equal(do_http(req, &c), 1);
    assert_int_equal(resp_status(&c), 414);
    ut_conn_free(&c);
    free(req);
}

static void test_invalid_content_length(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http(
        "POST /x HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
        &c), 1);
    assert_int_equal(resp_status(&c), 400);
    ut_conn_free(&c);
}

static void test_malformed_request_line(void **state)
{
    (void)state;
    fluxipc_conn_t c;
    assert_int_equal(do_http("GET\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 400);
    ut_conn_free(&c);
}

/* ── handler return-code mapping ─────────────────────────────────────────── */

static void test_handler_error_mapping(void **state)
{
    (void)state;
    static int einval = -EINVAL, enoent = -ENOENT, other = -999;
    fluxipc_node_t *root = fluxipc_tree_root();

    fluxipc_method_t m1 = { .ipc = ut_ipc_echo, .json = http_negative,
                            .data = &einval };
    fluxipc_node_leaf(root, "einval", &m1);
    fluxipc_method_t m2 = { .ipc = ut_ipc_echo, .json = http_negative,
                            .data = &enoent };
    fluxipc_node_leaf(root, "enoent", &m2);
    fluxipc_method_t m3 = { .ipc = ut_ipc_echo, .json = http_negative,
                            .data = &other };
    fluxipc_node_leaf(root, "other", &m3);
    fluxipc_method_t m4 = { .ipc = ut_ipc_echo, .json = http_negative_with_json };
    fluxipc_node_leaf(root, "withjson", &m4);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /einval HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 400);
    ut_conn_free(&c);

    assert_int_equal(do_http("GET /enoent HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 404);
    ut_conn_free(&c);

    assert_int_equal(do_http("GET /other HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 500);
    ut_conn_free(&c);

    /* negative rc but a JSON body → 500 with that body */
    assert_int_equal(do_http("GET /withjson HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 500);
    assert_true(resp_has(&c, "\"err\":\"detail\""));
    ut_conn_free(&c);
}

static void test_status_reasons(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = http_forced_status };
    fluxipc_node_leaf(root, "s", &m);

    const int codes[] = { 202, 409, 411, 415, 422, 501, 503, 299 };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        fluxipc_conn_t c;
        g_forced_status = codes[i];
        assert_int_equal(do_http("GET /s HTTP/1.1\r\n\r\n", &c), 1);
        assert_int_equal(resp_status(&c), codes[i]);
        ut_conn_free(&c);
    }
}

static void test_empty_body_responses(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m1 = { .ipc = ut_ipc_echo, .json = http_empty_body };
    fluxipc_node_leaf(root, "empty", &m1);
    fluxipc_method_t m2 = { .ipc = ut_ipc_echo, .json = http_empty_default_ct };
    fluxipc_node_leaf(root, "empty2", &m2);

    fluxipc_conn_t c;
    assert_int_equal(do_http("GET /empty HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    assert_true(resp_has(&c, "Content-Type: text/plain"));
    assert_true(resp_has(&c, "Content-Length: 0"));
    ut_conn_free(&c);

    assert_int_equal(do_http("GET /empty2 HTTP/1.1\r\n\r\n", &c), 1);
    assert_true(resp_has(&c, "application/json"));
    ut_conn_free(&c);
}

static void test_header_trailing_whitespace(void **state)
{
    (void)state;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = ut_http_ok,
                           .methods = FLUXIPC_M_POST };
    fluxipc_node_leaf(root, "tp", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("POST /tp HTTP/1.1\r\nX-Test: v  \r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 200);
    ut_conn_free(&c);
}

static void test_http_start_stop(void **state)
{
    (void)state;
    /* fixture started the server with HTTP disabled (port 0). */
    assert_int_equal(g_ctx->http_fd, -1);

    assert_int_equal(fluxipc_http_start(g_ctx, 0), 0);   /* ephemeral port */
    assert_true(g_ctx->http_fd >= 0);
    assert_int_equal(fluxipc_http_start(g_ctx, 0), -EALREADY);

    fluxipc_http_stop(g_ctx);
    assert_int_equal(g_ctx->http_fd, -1);
    fluxipc_http_stop(g_ctx);                            /* idempotent */

    assert_int_equal(fluxipc_http_start(NULL, 0), -EALREADY);
}

static void test_custom_status(void **state)
{
    (void)state;
    static int created = 201;
    fluxipc_node_t *root = fluxipc_tree_root();
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .json = http_custom_status,
                           .data = &created, .methods = FLUXIPC_M_POST };
    fluxipc_node_leaf(root, "create", &m);

    fluxipc_conn_t c;
    assert_int_equal(do_http("POST /create HTTP/1.1\r\n\r\n", &c), 1);
    assert_int_equal(resp_status(&c), 201);
    ut_conn_free(&c);
}

int ut_http(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_need_more, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_bad_request_line, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_unknown_path_404, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_healthz, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_openapi, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_container_manifest, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_leaf_methods, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_head_has_no_body, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_json_body, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_content_too_large, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_all_method_allow, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_unknown_method, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_openapi_all_verbs, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_healthz_program_and_head, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_container_recursive_query, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_verb_is_parsed_once, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_query_and_path_decode, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_body_parsing_variants, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_body_not_yet_received, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_header_overflow, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_header_too_large, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_target_too_long, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_invalid_content_length, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_malformed_request_line, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_handler_error_mapping, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_custom_status, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_status_reasons, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_empty_body_responses, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_header_trailing_whitespace, http_setup, http_teardown),
        cmocka_unit_test_setup_teardown(test_http_start_stop, http_setup, http_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
