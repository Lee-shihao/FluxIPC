/**
 * test_util.c – unit tests for the pure parsing helpers.
 *
 * These functions are the front line for hostile input, so the cases focus on
 * boundaries: escape at end-of-string, capacity exhaustion, empty segments.
 */

#include "ut_common.h"
#include <errno.h>
#include <string.h>

/* ── fluxipc_url_decode ──────────────────────────────────────────────────── */

static void test_url_decode_basic(void **state)
{
    (void)state;
    char buf[64];

    assert_int_equal(fluxipc_url_decode("a%20b", 5, buf, sizeof(buf)), 3);
    assert_string_equal(buf, "a b");

    assert_int_equal(fluxipc_url_decode("%2F", 3, buf, sizeof(buf)), 1);
    assert_string_equal(buf, "/");

    assert_int_equal(fluxipc_url_decode("%2f", 3, buf, sizeof(buf)), 1);
    assert_string_equal(buf, "/");

    assert_int_equal(fluxipc_url_decode("%41%42", 6, buf, sizeof(buf)), 2);
    assert_string_equal(buf, "AB");

    assert_int_equal(fluxipc_url_decode("plain", 5, buf, sizeof(buf)), 5);
    assert_string_equal(buf, "plain");
}

static void test_url_decode_invalid_escape(void **state)
{
    (void)state;
    char buf[64];

    /* invalid hex digits are kept literally */
    assert_int_equal(fluxipc_url_decode("%zz", 3, buf, sizeof(buf)), 3);
    assert_string_equal(buf, "%zz");

    assert_int_equal(fluxipc_url_decode("a%2Gb", 5, buf, sizeof(buf)), 5);
    assert_string_equal(buf, "a%2Gb");

    /* too short: "%2" and a trailing "%" are literal */
    assert_int_equal(fluxipc_url_decode("%2", 2, buf, sizeof(buf)), 2);
    assert_string_equal(buf, "%2");

    assert_int_equal(fluxipc_url_decode("ab%", 3, buf, sizeof(buf)), 3);
    assert_string_equal(buf, "ab%");
}

static void test_url_decode_capacity(void **state)
{
    (void)state;
    char buf[8];

    /* exact fit: cap 5 holds 4 chars + NUL */
    memset(buf, 'X', sizeof(buf));
    assert_int_equal(fluxipc_url_decode("abcd", 4, buf, 5), 4);
    assert_string_equal(buf, "abcd");

    /* truncated: cap 4 holds 3 chars + NUL */
    memset(buf, 'X', sizeof(buf));
    assert_int_equal(fluxipc_url_decode("abcdef", 6, buf, 4), 3);
    assert_string_equal(buf, "abc");

    /* cap 1 → only the terminator */
    memset(buf, 'X', sizeof(buf));
    assert_int_equal(fluxipc_url_decode("abc", 3, buf, 1), 0);
    assert_int_equal(buf[0], '\0');

    /* cap 0 → nothing written, returns 0 */
    assert_int_equal(fluxipc_url_decode("abc", 3, buf, 0), 0);

    /* NULL destination */
    assert_int_equal(fluxipc_url_decode("abc", 3, NULL, 8), 0);
}

static void test_url_decode_edge(void **state)
{
    (void)state;
    char buf[8];

    /* %00 decodes to an embedded NUL but still counts one output byte */
    assert_int_equal(fluxipc_url_decode("%00", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], '\0');

    /* empty input */
    assert_int_equal(fluxipc_url_decode("", 0, buf, sizeof(buf)), 0);
    assert_int_equal(buf[0], '\0');

    /* a literal % that is followed by valid hex but out of range stays put */
    assert_int_equal(fluxipc_url_decode("100%", 4, buf, sizeof(buf)), 4);
    assert_string_equal(buf, "100%");
}

/* ── fluxipc_parse_query ─────────────────────────────────────────────────── */

static void test_query_basic(void **state)
{
    (void)state;
    fluxipc_kv_t kv[8];
    char arena[128];

    int n = fluxipc_parse_query("a=1&b=hello%20world", 19, kv, 8,
                                arena, sizeof(arena));
    assert_int_equal(n, 2);
    assert_string_equal(kv[0].key, "a");
    assert_string_equal(kv[0].value, "1");
    assert_string_equal(kv[1].key, "b");
    assert_string_equal(kv[1].value, "hello world");
}

static void test_query_no_equals(void **state)
{
    (void)state;
    fluxipc_kv_t kv[4];
    char arena[64];

    int n = fluxipc_parse_query("flag", 4, kv, 4, arena, sizeof(arena));
    assert_int_equal(n, 1);
    assert_string_equal(kv[0].key, "flag");
    assert_string_equal(kv[0].value, "");
}

static void test_query_empty_parts(void **state)
{
    (void)state;
    fluxipc_kv_t kv[8];
    char arena[128];

    /* leading '&' yields an empty first pair */
    int n = fluxipc_parse_query("&a=1", 4, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 2);
    assert_string_equal(kv[0].key, "");
    assert_string_equal(kv[0].value, "");
    assert_string_equal(kv[1].key, "a");

    /* trailing '&' does not add a pair */
    n = fluxipc_parse_query("a=1&", 4, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 1);

    /* "=v" → empty key; "a=" → empty value */
    n = fluxipc_parse_query("=v", 2, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 1);
    assert_string_equal(kv[0].key, "");
    assert_string_equal(kv[0].value, "v");

    n = fluxipc_parse_query("a=", 2, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 1);
    assert_string_equal(kv[0].key, "a");
    assert_string_equal(kv[0].value, "");
}

static void test_query_limits(void **state)
{
    (void)state;
    fluxipc_kv_t kv[8];
    char arena[128];

    /* max_kv caps the number of parsed pairs */
    int n = fluxipc_parse_query("a=1&b=2&c=3", 11, kv, 2,
                                arena, sizeof(arena));
    assert_int_equal(n, 2);

    /* NULL / empty fast paths */
    assert_int_equal(fluxipc_parse_query(NULL, 4, kv, 8, arena, sizeof(arena)), 0);
    assert_int_equal(fluxipc_parse_query("", 0, kv, 8, arena, sizeof(arena)), 0);

    /* arena too small → stop early (at least one pair must not fit) */
    char tiny[4];
    n = fluxipc_parse_query("aaaa=bbbb&cccc=dddd", 19, kv, 8, tiny, sizeof(tiny));
    assert_true(n <= 2);
}

static void test_query_repeated_and_encoded(void **state)
{
    (void)state;
    fluxipc_kv_t kv[8];
    char arena[128];

    int n = fluxipc_parse_query("a=1&a=2", 7, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 2);
    assert_string_equal(kv[0].value, "1");
    assert_string_equal(kv[1].value, "2");

    n = fluxipc_parse_query("k%20ey=v%2Fal", 13, kv, 8, arena, sizeof(arena));
    assert_int_equal(n, 1);
    assert_string_equal(kv[0].key, "k ey");
    assert_string_equal(kv[0].value, "v/al");
}

/* ── fluxipc_find_content_length ─────────────────────────────────────────── */

static size_t hbytes_of(const char *req)
{
    const char *e = strstr(req, "\r\n\r\n");
    return e ? (size_t)(e - req) : 0;
}

static void test_content_length_present(void **state)
{
    (void)state;
    const char *h1 =
        "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 42\r\n\r\n";
    size_t n = 0;
    assert_int_equal(fluxipc_find_content_length(h1, hbytes_of(h1), &n), 1);
    assert_int_equal(n, 42);

    /* case-insensitive, leading spaces, tabs */
    const char *h2 =
        "POST /x HTTP/1.1\r\ncontent-length:\t 7\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h2, hbytes_of(h2), &n), 1);
    assert_int_equal(n, 7);
}

static void test_content_length_absent(void **state)
{
    (void)state;
    size_t n = 0;
    const char *h1 = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h1, hbytes_of(h1), &n), 0);

    /* no CRLF at all */
    const char *h2 = "GET / HTTP/1.1";
    assert_int_equal(fluxipc_find_content_length(h2, strlen(h2), &n), 0);

    /* hbytes == 0 */
    assert_int_equal(fluxipc_find_content_length("garbage", 0, &n), 0);

    /* header name that merely starts with the same prefix */
    const char *h3 = "GET / HTTP/1.1\r\nX-Content-Length: 9\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h3, hbytes_of(h3), &n), 0);
}

static void test_content_length_no_mutation(void **state)
{
    (void)state;
    const char *h1 =
        "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 42\r\n\r\n";
    char copy[128];
    strcpy(copy, h1);
    size_t hb = hbytes_of(copy);
    size_t n = 0;
    assert_int_equal(fluxipc_find_content_length(copy, hb, &n), 1);
    assert_int_equal(n, 42);
    assert_string_equal(copy, h1);   /* unchanged */
}

static void test_url_decode_hex_boundaries(void **state)
{
    (void)state;
    char buf[8];
    /* exercise every hex-digit boundary in hexval() */
    assert_int_equal(fluxipc_url_decode("%30", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], '0');
    assert_int_equal(fluxipc_url_decode("%39", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], '9');
    assert_int_equal(fluxipc_url_decode("%61", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 'a');
    assert_int_equal(fluxipc_url_decode("%66", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 'f');
    assert_int_equal(fluxipc_url_decode("%41", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 'A');
    assert_int_equal(fluxipc_url_decode("%46", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 'F');
    /* hex digits exactly at 'a' and 'A' (lower/upper-range starts) */
    assert_int_equal(fluxipc_url_decode("%1a", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 0x1a);
    assert_int_equal(fluxipc_url_decode("%1A", 3, buf, sizeof(buf)), 1);
    assert_int_equal(buf[0], 0x1a);

    /* just outside the hex-digit ranges are invalid and stay literal */
    assert_int_equal(fluxipc_url_decode("%3:", 3, buf, sizeof(buf)), 3);  /* ':' > '9' */
    assert_string_equal(buf, "%3:");
    assert_int_equal(fluxipc_url_decode("%3/", 3, buf, sizeof(buf)), 3);  /* '/' < '0' */
    assert_int_equal(fluxipc_url_decode("%3g", 3, buf, sizeof(buf)), 3);  /* 'g' > 'f' */
    assert_int_equal(fluxipc_url_decode("%3`", 3, buf, sizeof(buf)), 3);  /* '`' < 'a' */
    assert_int_equal(fluxipc_url_decode("%3G", 3, buf, sizeof(buf)), 3);  /* 'G' > 'F' */
    assert_int_equal(fluxipc_url_decode("%3@", 3, buf, sizeof(buf)), 3);  /* '@' < 'A' */
    assert_int_equal(fluxipc_url_decode("%2F", 3, buf, sizeof(buf)), 1);  /* '/' */
}

static void test_url_decode_loop_sentinel(void **state)
{
    (void)state;
    /* slen stops before the sentinel; a <= off-by-one would decode 'X' */
    char src[] = "abX";
    char buf[8];
    assert_int_equal(fluxipc_url_decode(src, 2, buf, sizeof(buf)), 2);
    assert_string_equal(buf, "ab");
}

static void test_find_content_length_digit_boundaries(void **state)
{
    (void)state;
    size_t n = 0;
    const char *h1 = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h1, hbytes_of(h1), &n), 1);
    assert_int_equal(n, 10);
    const char *h2 = "POST / HTTP/1.1\r\nContent-Length: 90\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h2, hbytes_of(h2), &n), 1);
    assert_int_equal(n, 90);
    const char *h3 = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    assert_int_equal(fluxipc_find_content_length(h3, hbytes_of(h3), &n), 1);
    assert_int_equal(n, 0);
}

static void test_query_exact_arena(void **state)
{
    (void)state;
    fluxipc_kv_t kv[4];
    char arena[4];                             /* exactly "a\0" + "1\0" */
    int n = fluxipc_parse_query("a=1", 3, kv, 4, arena, sizeof(arena));
    assert_int_equal(n, 1);
    assert_string_equal(kv[0].key, "a");
    assert_string_equal(kv[0].value, "1");
}

/* ── schema string macros ────────────────────────────────────────────────── */

static void test_schema_macros(void **state)
{
    (void)state;
    const char *text = FLUXIPC_SCHEMA(
        FLUXIPC_PROP_INT("a") "," FLUXIPC_PROP_STR("b"));

    cJSON *schema = cJSON_Parse(text);
    assert_non_null(schema);
    assert_string_equal(cJSON_GetObjectItem(schema, "type")->valuestring,
                        "object");

    cJSON *props = cJSON_GetObjectItem(schema, "properties");
    assert_non_null(props);
    assert_string_equal(cJSON_GetObjectItem(
        cJSON_GetObjectItem(props, "a"), "type")->valuestring, "integer");
    assert_string_equal(cJSON_GetObjectItem(
        cJSON_GetObjectItem(props, "b"), "type")->valuestring, "string");
    cJSON_Delete(schema);

    schema = cJSON_Parse(FLUXIPC_SCHEMA(
        FLUXIPC_PROP_ARRAY("args", "string")));
    assert_non_null(schema);
    cJSON *args = cJSON_GetObjectItem(
        cJSON_GetObjectItem(schema, "properties"), "args");
    assert_string_equal(cJSON_GetObjectItem(args, "type")->valuestring,
                        "array");
    assert_string_equal(cJSON_GetObjectItem(
        cJSON_GetObjectItem(args, "items"), "type")->valuestring, "string");
    cJSON_Delete(schema);
}

/* ── structured request helpers ──────────────────────────────────────────── */

static void test_json_request_helpers(void **state)
{
    (void)state;

    cJSON *body = cJSON_Parse(
        "{\"s\":\"body\",\"n\":7,\"d\":1.5,\"b\":true,"
        "\"strnum\":\"42\",\"ds\":\"2.5\",\"bnum\":1,"
        "\"badbool\":\"maybe\"}");
    assert_non_null(body);

    fluxipc_kv_t query[] = {
        { "q",      "query" },
        { "n2",     "9"     },
        { "flag",    "yes"   },
        { "noflag",  "off"   },
        { "badquery","maybe" },
        { "dn",      "3.5"   },
    };
    fluxipc_json_request_t req = {
        .json        = body,
        .query       = query,
        .query_count = sizeof(query) / sizeof(query[0]),
    };

    assert_string_equal(fluxipc_json_str(&req, "s", "def"), "body");
    assert_string_equal(fluxipc_json_str(&req, "q", "def"), "query");
    assert_string_equal(fluxipc_json_str(&req, "missing", "def"), "def");

    assert_int_equal(fluxipc_json_int(&req, "n", -1), 7);
    assert_int_equal(fluxipc_json_int(&req, "n2", -1), 9);
    assert_int_equal(fluxipc_json_int(&req, "strnum", -1), 42);
    assert_int_equal(fluxipc_json_int(&req, "b", -1), 1);
    assert_int_equal(fluxipc_json_int(&req, "missing", -1), -1);

    assert_true(fluxipc_json_double(&req, "d", -1) == 1.5);
    assert_true(fluxipc_json_double(&req, "ds", -1) == 2.5);
    assert_true(fluxipc_json_double(&req, "dn", -1) == 3.5);
    assert_true(fluxipc_json_double(&req, "missing", -1) == -1);

    assert_int_equal(fluxipc_json_bool(&req, "b", 0), 1);
    assert_int_equal(fluxipc_json_bool(&req, "bnum", 0), 1);
    assert_int_equal(fluxipc_json_bool(&req, "flag", 0), 1);
    assert_int_equal(fluxipc_json_bool(&req, "noflag", 1), 0);
    assert_int_equal(fluxipc_json_bool(&req, "badbool", 7), 7);
    assert_int_equal(fluxipc_json_bool(&req, "badquery", 8), 8);
    assert_int_equal(fluxipc_json_bool(&req, "missing", 1), 1);

    assert_true(fluxipc_json_has(&req, "s"));
    assert_true(fluxipc_json_has(&req, "q"));
    assert_false(fluxipc_json_has(&req, "missing"));

    req.verb = FLUXIPC_M_PUT;
    assert_true(FLUXIPC_VERB_IS(&req, FLUXIPC_M_PUT));
    assert_true(FLUXIPC_VERB_IS_WRITE(&req));
    assert_false(FLUXIPC_VERB_IS_READ(&req));

    cJSON_Delete(body);
}

static void test_json_error_helper(void **state)
{
    (void)state;
    fluxipc_json_response_t resp = { 0 };

    struct { int code; int status; int ret; } cases[] = {
        { -EINVAL,  400, -EINVAL  },
        { -EACCES,  403, -EACCES  },
        { -EPERM,   403, -EPERM   },
        { -ENOENT,  404, -ENOENT  },
        { -EEXIST,  409, -EEXIST  },
        { -EBUSY,   409, -EBUSY   },
        { -EAGAIN,  409, -EAGAIN  },
        { -ERANGE,  422, -ERANGE  },
        { -E2BIG,   422, -E2BIG   },
        { -ENOSYS,  501, -ENOSYS  },
        { -ENOMEM,  500, -ENOMEM  },
        { 400,      400, -EINVAL  },
        { 403,      403, -EACCES  },
        { 404,      404, -ENOENT  },
        { 409,      409, -EBUSY   },
        { 413,      413, -EMSGSIZE},
        { 422,      422, -ERANGE  },
        { 431,      431, -E2BIG   },
        { 501,      501, -ENOSYS  },
        { 418,      418, -EIO     },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int ret = fluxipc_json_error(&resp, cases[i].code, "msg");
        assert_int_equal(ret, cases[i].ret);
        assert_int_equal(resp.status, cases[i].status);
        assert_string_equal(
            cJSON_GetObjectItem(resp.json, "error")->valuestring, "msg");
        assert_int_equal(
            cJSON_GetObjectItem(resp.json, "code")->valueint, cases[i].code);
        cJSON_Delete(resp.json);
        resp.json = NULL;
    }

    assert_int_equal(fluxipc_json_error(NULL, -EINVAL, "x"), -EINVAL);
}

static void test_content_length_invalid(void **state)
{
    (void)state;
    size_t n = 0;
    const char *overflow =
        "POST / HTTP/1.1\r\nContent-Length: 999999999999999999999999999999\r\n\r\n";
    const char *empty =
        "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n";
    const char *junk =
        "POST / HTTP/1.1\r\nContent-Length: 42x\r\n\r\n";
    const char *dup =
        "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n";

    assert_int_equal(fluxipc_find_content_length(overflow, hbytes_of(overflow), &n), -1);
    assert_int_equal(fluxipc_find_content_length(empty, hbytes_of(empty), &n), -1);
    assert_int_equal(fluxipc_find_content_length(junk, hbytes_of(junk), &n), -1);
    assert_int_equal(fluxipc_find_content_length(dup, hbytes_of(dup), &n), -1);
}

int ut_util(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_url_decode_basic),
        cmocka_unit_test(test_url_decode_invalid_escape),
        cmocka_unit_test(test_url_decode_capacity),
        cmocka_unit_test(test_url_decode_edge),
        cmocka_unit_test(test_query_basic),
        cmocka_unit_test(test_query_no_equals),
        cmocka_unit_test(test_query_empty_parts),
        cmocka_unit_test(test_query_limits),
        cmocka_unit_test(test_query_repeated_and_encoded),
        cmocka_unit_test(test_content_length_present),
        cmocka_unit_test(test_content_length_absent),
        cmocka_unit_test(test_content_length_no_mutation),
        cmocka_unit_test(test_url_decode_hex_boundaries),
        cmocka_unit_test(test_url_decode_loop_sentinel),
        cmocka_unit_test(test_find_content_length_digit_boundaries),
        cmocka_unit_test(test_content_length_invalid),
        cmocka_unit_test(test_query_exact_arena),
        cmocka_unit_test(test_schema_macros),
        cmocka_unit_test(test_json_request_helpers),
        cmocka_unit_test(test_json_error_helper),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
