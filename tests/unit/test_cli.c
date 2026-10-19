/**
 * test_cli.c – white-box tests for the command-line client.
 *
 * Grammar: fluxipc-cli <server> [-l|--list] [-c|--call <path> [args]] [-i|--interactive]
 *
 * The CLI owns a main(); rename it and include the source so its helpers can
 * be driven directly with captured output.
 */

#include "ut_common.h"

#define main fluxipc_cli_main
#include "../../cli/fluxipc_cli.c"
#undef main

#include <string.h>

/* ── server-backed fixture (for --call / --list success) ─────────────────── */

static int cli_setup(void **state)
{
    (void)state;
    ut_server_up();
    ut_server_pump_start();
    return 0;
}

static int cli_teardown(void **state)
{
    (void)state;
    ut_server_pump_stop();
    ut_server_down();
    return 0;
}

/* ── help / list errors ──────────────────────────────────────────────────── */

static void test_print_help(void **state)
{
    (void)state;
    ut_capture_begin();
    print_help("fluxipc-cli");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "--list"));
    assert_non_null(strstr(out, "--call"));
    assert_non_null(strstr(out, "--interactive"));
    assert_non_null(strstr(out, "<server>"));
}

static void test_cmd_list_missing_server(void **state)
{
    (void)state;
    ut_capture_begin();
    int rc = cmd_list("definitely_not_a_server_xyz");
    const char *out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "cannot reach server"));
}

/* ── argument handling ───────────────────────────────────────────────────── */

static void test_main_arg_errors(void **state)
{
    (void)state;

    /* no arguments at all */
    char *noargs[] = { (char *)"fluxipc-cli", NULL };
    ut_capture_begin();
    int rc = fluxipc_cli_main(1, noargs);
    ut_capture_end();
    assert_int_equal(rc, 1);

    /* global help */
    char *help[] = { (char *)"fluxipc-cli", (char *)"--help", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(2, help);
    ut_capture_end();
    assert_int_equal(rc, 0);

    char *help_short[] = { (char *)"fluxipc-cli", (char *)"-h", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(2, help_short);
    ut_capture_end();
    assert_int_equal(rc, 0);

    /* first positional argument must be the server, not a flag */
    char *flag_first[] = { (char *)"fluxipc-cli", (char *)"--list", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(2, flag_first);
    const char *out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "expected <server>"));

    /* bare <server> with no action → list; unreachable → failure */
    char *server_only[] = { (char *)"fluxipc-cli",
                            (char *)"no_such_server_zzz", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(2, server_only);
    out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "cannot reach server"));

    /* --list with trailing junk is rejected */
    char *list_extra[] = { (char *)"fluxipc-cli", (char *)"srv",
                           (char *)"--list", (char *)"junk", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(4, list_extra);
    ut_capture_end();
    assert_int_equal(rc, 1);

    /* --interactive with trailing junk is rejected (avoids entering REPL) */
    char *inter_extra[] = { (char *)"fluxipc-cli", (char *)"srv",
                            (char *)"--interactive", (char *)"junk", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(4, inter_extra);
    ut_capture_end();
    assert_int_equal(rc, 1);

    /* --call without a path is rejected */
    char *call_nopath[] = { (char *)"fluxipc-cli", (char *)"srv",
                            (char *)"--call", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(3, call_nopath);
    out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "requires <path>"));

    /* --json without a path is rejected */
    char *json_nopath[] = { (char *)"fluxipc-cli", (char *)"srv",
                            (char *)"--json", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(3, json_nopath);
    out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "requires <path>"));

    /* --json -X without a method value is rejected */
    char *json_nomethod[] = { (char *)"fluxipc-cli", (char *)"srv",
                              (char *)"--json", (char *)"-X", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(4, json_nomethod);
    out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "requires a value"));

    /* unknown command */
    char *unknown[] = { (char *)"fluxipc-cli", (char *)"srv",
                        (char *)"--nope", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(3, unknown);
    out = ut_capture_end();
    assert_int_equal(rc, 1);
    assert_non_null(strstr(out, "unknown command"));
}

/* ── success paths (real server + pump) ──────────────────────────────────── */

static void test_call_success(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"PONG" };
    fluxipc_node_leaf(fluxipc_tree_root(), "ping", &m);

    char *av[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                   (char *)"-c", (char *)"/ping", NULL };
    ut_capture_begin();
    int rc = fluxipc_cli_main(4, av);
    const char *out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "PONG"));

    /* long form with extra call arguments */
    char *av2[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                    (char *)"--call", (char *)"/ping",
                    (char *)"extra", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(5, av2);
    out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "PONG"));
}

static void test_call_json_success(void **state)
{
    (void)state;
    fluxipc_method_t m = {
        .json   = ut_http_ok,
        .data   = (void *)"JSON",
        .schema = "{\"type\":\"object\",\"properties\":{"
                  "\"a\":{\"type\":\"integer\"}}}",
    };
    fluxipc_node_leaf(fluxipc_tree_root(), "json", &m);

    char *av[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                   (char *)"--json", (char *)"/json", NULL };
    ut_capture_begin();
    int rc = fluxipc_cli_main(4, av);
    const char *out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "handler"));
    assert_non_null(strstr(out, "JSON"));

    /* a JSON body defaults to POST and is forwarded to the handler */
    char *av2[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                    (char *)"-j", (char *)"/json",
                    (char *)"{\"a\":1}", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(5, av2);
    out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "JSON"));

    /* positional arguments bind to schema properties in order */
    char *av3[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                    (char *)"--json", (char *)"/json",
                    (char *)"7", NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(5, av3);
    out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "JSON"));
}

static void test_list_success(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .summary = "ping me" };
    fluxipc_node_leaf(fluxipc_tree_root(), "ping", &m);

    /* explicit --list */
    char *av[] = { (char *)"fluxipc-cli", (char *)ut_server_name(),
                   (char *)"--list", NULL };
    ut_capture_begin();
    int rc = fluxipc_cli_main(3, av);
    const char *out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "/ping"));

    /* bare <server> defaults to listing */
    char *av2[] = { (char *)"fluxipc-cli", (char *)ut_server_name(), NULL };
    ut_capture_begin();
    rc = fluxipc_cli_main(2, av2);
    out = ut_capture_end();
    assert_int_equal(rc, 0);
    assert_non_null(strstr(out, "/ping"));
}

int ut_cli(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_print_help),
        cmocka_unit_test(test_cmd_list_missing_server),
        cmocka_unit_test(test_main_arg_errors),
        cmocka_unit_test_setup_teardown(test_call_success,
                                        cli_setup, cli_teardown),
        cmocka_unit_test_setup_teardown(test_call_json_success,
                                        cli_setup, cli_teardown),
        cmocka_unit_test_setup_teardown(test_list_success,
                                        cli_setup, cli_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
