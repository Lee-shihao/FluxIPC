/**
 * test_protocol.c – pins the IPC wire-format layout so client and server
 * cannot drift apart silently.
 */

#include "ut_common.h"
#include <stddef.h>
#include <string.h>

static void test_header_sizes(void **state)
{
    (void)state;
    assert_int_equal(sizeof(fluxipc_req_hdr_t),  12);
    assert_int_equal(sizeof(fluxipc_resp_hdr_t), 12);
}

static void test_header_layout(void **state)
{
    (void)state;
    assert_int_equal(offsetof(fluxipc_req_hdr_t, magic),    0);
    assert_int_equal(offsetof(fluxipc_req_hdr_t, op),       4);
    assert_int_equal(offsetof(fluxipc_req_hdr_t, path_len), 6);
    assert_int_equal(offsetof(fluxipc_req_hdr_t, arg_len),  8);

    assert_int_equal(offsetof(fluxipc_resp_hdr_t, magic),    0);
    assert_int_equal(offsetof(fluxipc_resp_hdr_t, status),   4);
    assert_int_equal(offsetof(fluxipc_resp_hdr_t, data_len), 8);
}

static void test_request_roundtrip(void **state)
{
    (void)state;
    fluxipc_req_hdr_t in = {
        .magic = FLUXIPC_PROTO_MAGIC, .op = FLUXIPC_OP_CALL,
        .path_len = 5, .arg_len = 9,
    };
    unsigned char wire[sizeof(in)];
    memcpy(wire, &in, sizeof(in));
    fluxipc_req_hdr_t out;
    memcpy(&out, wire, sizeof(out));
    assert_int_equal(out.magic,    FLUXIPC_PROTO_MAGIC);
    assert_int_equal(out.op,       FLUXIPC_OP_CALL);
    assert_int_equal(out.path_len, 5);
    assert_int_equal(out.arg_len,  9);
}

static void test_magic_constant(void **state)
{
    (void)state;
    /* "FLX2" little-endian — a change here is a protocol break. */
    assert_int_equal(FLUXIPC_PROTO_MAGIC, 0x464C5832UL);
    assert_int_equal(FLUXIPC_OP_CALL, 1);
    assert_int_equal(FLUXIPC_OP_LIST, 2);
}

int ut_protocol(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_header_sizes),
        cmocka_unit_test(test_header_layout),
        cmocka_unit_test(test_request_roundtrip),
        cmocka_unit_test(test_magic_constant),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
