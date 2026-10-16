/**
 * test_interactive.c – white-box tests for the REPL module.
 *
 * The interactive module is dominated by static helpers (path trie, range
 * parser, completion).  We include the translation unit directly so those
 * helpers can be exercised without going through readline.
 */

#include "ut_common.h"

/* pull in the module under test (excluded from the link list in the Makefile) */
#include "../../src/fluxipc_interactive.c"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>

/* ── fixture ─────────────────────────────────────────────────────────────── */

static int inter_setup(void **state)
{
    (void)state;
    tree_init();
    tree_clear();
    return 0;
}

static int inter_teardown(void **state)
{
    (void)state;
    if (g_root) tree_clear();
    return 0;
}

/* ── is_number ───────────────────────────────────────────────────────────── */

static void test_is_number(void **state)
{
    (void)state;
    assert_true(is_number("0"));
    assert_true(is_number("123"));
    assert_true(is_number("-5"));
    assert_true(is_number("+5"));
    assert_true(is_number("1.5"));
    assert_true(is_number("-.5"));
    assert_true(is_number("007"));

    assert_false(is_number(""));
    assert_false(is_number("+"));
    assert_false(is_number("-"));
    assert_false(is_number("."));
    assert_false(is_number("1.2.3"));
    assert_false(is_number("1e3"));
    assert_false(is_number("1a"));
    assert_false(is_number("a1"));
    assert_false(is_number(NULL));
}

/* ── parse_arg / range expansion ─────────────────────────────────────────── */

static void test_parse_arg_literal(void **state)
{
    (void)state;
    arg_spec_t s;

    parse_arg("hello", &s);
    assert_false(s.is_range);
    assert_string_equal(s.literal, "hello");

    parse_arg("a:b", &s);          /* non-numeric → literal */
    assert_false(s.is_range);
    assert_string_equal(s.literal, "a:b");

    parse_arg("1:2:3:4", &s);      /* too many colons → literal */
    assert_false(s.is_range);

    parse_arg("\\0:100", &s);      /* backslash escape */
    assert_false(s.is_range);
    assert_true(s.escaped);
    assert_string_equal(s.literal, "0:100");

    parse_arg("'0:100:5'", &s);    /* quoted literal */
    assert_false(s.is_range);
    assert_string_equal(s.literal, "0:100:5");
}

static void test_parse_arg_ranges(void **state)
{
    (void)state;
    arg_spec_t s;

    parse_arg("0:4", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 5);
    assert_true(s.vals[0] == 0.0 && s.vals[4] == 4.0);
    free_arg_spec(&s);

    parse_arg("0:10:5", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 3);   /* 0,5,10 */
    assert_true(s.vals[2] == 10.0);
    free_arg_spec(&s);

    /* descending with default (negative) step */
    parse_arg("4:0", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 5);
    assert_true(s.vals[0] == 4.0 && s.vals[4] == 0.0);
    free_arg_spec(&s);

    /* explicit negative step */
    parse_arg("10:0:-2", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 6);
    assert_true(s.vals[0] == 10.0 && s.vals[5] == 0.0);
    free_arg_spec(&s);

    /* fractional step */
    parse_arg("0:1:0.25", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 5);
    free_arg_spec(&s);

    /* single value */
    parse_arg("3:3", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, 1);
    free_arg_spec(&s);
}

static void test_parse_arg_errors(void **state)
{
    (void)state;
    arg_spec_t s;

    /* zero step → error, treated as literal */
    ut_capture_begin();
    parse_arg("0:10:0", &s);
    const char *out = ut_capture_end();
    assert_false(s.is_range);
    assert_non_null(strstr(out, "step is zero"));
    free_arg_spec(&s);

    /* direction mismatch → error, literal */
    ut_capture_begin();
    parse_arg("10:0:2", &s);
    out = ut_capture_end();
    assert_false(s.is_range);
    assert_non_null(strstr(out, "direction mismatch"));
    free_arg_spec(&s);
}

static void test_fmt_val(void **state)
{
    (void)state;
    char buf[32];

    fmt_val(0.0, buf, sizeof(buf));
    assert_string_equal(buf, "0");
    fmt_val(42.0, buf, sizeof(buf));
    assert_string_equal(buf, "42");
    fmt_val(-7.0, buf, sizeof(buf));
    assert_string_equal(buf, "-7");
    fmt_val(1.5, buf, sizeof(buf));
    assert_string_equal(buf, "1.5");
    fmt_val(0.25, buf, sizeof(buf));
    assert_string_equal(buf, "0.25");
}

/* ── path tree ───────────────────────────────────────────────────────────── */

static void test_path_tree(void **state)
{
    (void)state;

    path_node_t *c = tree_insert_path("/a/b/c");
    assert_non_null(c);
    assert_string_equal(c->full_path, "/a/b/c");
    assert_string_equal(c->name, "c");

    assert_ptr_equal(node_lookup("/a/b/c"), c);
    assert_ptr_equal(node_lookup("/"), g_root);
    assert_null(node_lookup("/a/x"));
    assert_null(node_lookup("relative"));

    path_node_t *a = node_lookup("/a");
    assert_non_null(a);
    assert_int_equal(a->child_count, 1);

    /* idempotent child creation */
    assert_ptr_equal(node_get_child(a, "b"), node_lookup("/a/b"));

    /* relative lookup */
    g_cwd_node = a;
    assert_ptr_equal(node_lookup_rel("b"), node_lookup("/a/b"));
    assert_ptr_equal(node_lookup_rel(""), a);
    assert_ptr_equal(node_lookup_rel("."), a);
    assert_ptr_equal(node_lookup_rel(".."), g_root);
    assert_ptr_equal(node_lookup_rel("b/../b"), node_lookup("/a/b"));
    assert_ptr_equal(node_lookup_rel("/a/b/c"), c);

    g_cwd_node = g_root;
}

static void test_rel_path(void **state)
{
    (void)state;
    path_node_t *c = tree_insert_path("/a/b/c");
    assert_non_null(c);

    g_cwd_node = g_root;
    assert_string_equal(rel_path(c), "a/b/c");

    g_cwd_node = node_lookup("/a");
    assert_string_equal(rel_path(c), "b/c");

    /* node not under cwd falls back to absolute form */
    path_node_t *other = tree_insert_path("/z");
    assert_non_null(other);
    g_cwd_node = g_root;
    assert_string_equal(rel_path(other), "z");
    g_cwd_node = node_lookup("/a");
    assert_string_equal(rel_path(other), "/z");

    g_cwd_node = g_root;
}

static void test_collect_leaves(void **state)
{
    (void)state;
    path_node_t *a = tree_insert_path("/a");
    path_node_t *b = tree_insert_path("/a/b");
    assert_non_null(a);
    assert_non_null(b);

    tree_add_endpoint("/a/leaf1", 1, "u1", "/s1");
    tree_add_endpoint("/a/b/leaf2", 2, "u2", "/s2");

    path_node_t *list[16];
    int count = 0;
    collect_leaves(g_root, list, &count, 16);
    assert_int_equal(count, 2);
}

/* ── commands ────────────────────────────────────────────────────────────── */

static void test_cmd_cd(void **state)
{
    (void)state;
    path_node_t *a = tree_insert_path("/a");
    path_node_t *b = tree_insert_path("/a/b");
    assert_non_null(a);
    assert_non_null(b);
    tree_add_endpoint("/a/b/leaf", 5, "u", "/sock");

    g_cwd_node = g_root;

    cmd_cd("/a/b");
    assert_ptr_equal(g_cwd_node, b);

    cmd_cd("..");
    assert_ptr_equal(g_cwd_node, a);

    ut_capture_begin();
    cmd_cd("/a/b/leaf");          /* leaf → error */
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "is a leaf endpoint"));

    ut_capture_begin();
    cmd_cd("/does/not/exist");
    out = ut_capture_end();
    assert_non_null(strstr(out, "No such namespace"));

    cmd_cd("/");
    assert_ptr_equal(g_cwd_node, g_root);

    cmd_cd(NULL);
    assert_ptr_equal(g_cwd_node, g_root);
}

static void test_update_prompt(void **state)
{
    (void)state;
    path_node_t *a = tree_insert_path("/a");
    assert_non_null(a);

    g_cwd_node = g_root;
    update_prompt();
    assert_string_equal(g_prompt, "fluxipc> ");

    g_cwd_node = a;
    update_prompt();
    assert_string_equal(g_prompt, "fluxipc@/a> ");
    g_cwd_node = g_root;
}

static void test_cmd_help_and_ls(void **state)
{
    (void)state;
    tree_add_endpoint("/a/leaf", 3, "leaf usage", "/sock");
    g_cwd_node = g_root;

    ut_capture_begin();
    cmd_help(NULL);
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "Built-in commands"));

    ut_capture_begin();
    cmd_help("/a/leaf");
    out = ut_capture_end();
    assert_non_null(strstr(out, "leaf usage"));

    ut_capture_begin();
    cmd_ls();
    out = ut_capture_end();
    assert_non_null(strstr(out, "a/leaf"));

    /* empty namespace */
    g_cwd_node = tree_insert_path("/a");
    assert_non_null(g_cwd_node);
    ut_capture_begin();
    cmd_ls();
    out = ut_capture_end();
    assert_non_null(strstr(out, "leaf"));   /* relative to cwd /a */
    g_cwd_node = g_root;
}

/* ── completion helpers ──────────────────────────────────────────────────── */

static void test_build_candidates_relative(void **state)
{
    (void)state;
    tree_insert_path("/alpha/one");
    tree_insert_path("/alpha/two");
    tree_insert_path("/beta");

    g_cwd_node = node_lookup("/alpha");
    build_candidates("t");
    assert_int_equal(g_cand_count, 1);
    assert_string_equal(g_candidates[0], "two");
    if (g_candidates) {
        for (int i = 0; i < g_cand_count; i++) free(g_candidates[i]);
        free(g_candidates);
        g_candidates = NULL;
    }
    g_cand_count = 0;
    g_cwd_node = g_root;
}

static void test_build_candidates_absolute(void **state)
{
    (void)state;
    tree_insert_path("/alpha/one");
    tree_insert_path("/alpha/two");
    g_cwd_node = g_root;

    build_candidates("/alpha/");
    assert_int_equal(g_cand_count, 2);
    if (g_candidates) {
        for (int i = 0; i < g_cand_count; i++) free(g_candidates[i]);
        free(g_candidates);
        g_candidates = NULL;
    }
    g_cand_count = 0;

    build_candidates("/al");
    assert_int_equal(g_cand_count, 1);
    if (g_candidates) {
        for (int i = 0; i < g_cand_count; i++) free(g_candidates[i]);
        free(g_candidates);
        g_candidates = NULL;
    }
    g_cand_count = 0;
}

static void test_token_helpers(void **state)
{
    (void)state;
    char line[] = "watch 2 /a/b";
    rl_line_buffer = line;
    assert_true(first_word_is("watch"));
    assert_false(first_word_is("cd"));
    assert_false(first_word_is("watchx"));
    assert_int_equal(token_index_at(0), 0);
    assert_int_equal(token_index_at(6), 1);
    assert_int_equal(token_index_at(8), 2);
    rl_line_buffer = NULL;
}

/* ── server-backed fixture (dispatch / sweep / reload) ───────────────────── */

static int inter_srv_setup(void **state)
{
    (void)state;
    ut_server_up();
    ut_server_pump_start();
    tree_init();
    tree_clear();
    return 0;
}

static int inter_srv_teardown(void **state)
{
    (void)state;
    ut_server_pump_stop();
    ut_server_down();
    if (g_root) tree_clear();
    return 0;
}

/* ── more range / number tests ───────────────────────────────────────────── */

static void test_parse_arg_cap(void **state)
{
    (void)state;
    arg_spec_t s;
    parse_arg("0:100000", &s);
    assert_true(s.is_range);
    assert_int_equal(s.nvals, RANGE_MAX_VALS);   /* capped */
    free_arg_spec(&s);
}

static void test_sweep_call_count(void **state)
{
    (void)state;
    arg_spec_t specs[3];
    memset(specs, 0, sizeof(specs));
    specs[0].is_range = 1; specs[0].nvals = 3;
    specs[2].is_range = 1; specs[2].nvals = 4;
    assert_int_equal(sweep_call_count(specs, 3), 12);
    assert_int_equal(sweep_call_count(specs, 0), 1);
}

static void test_fmt_val_large(void **state)
{
    (void)state;
    char buf[40];
    fmt_val(1e15, buf, sizeof(buf));       /* boundary of the integer fast path */
    assert_true(buf[0] != '\0');
    fmt_val(-2.5, buf, sizeof(buf));
    assert_string_equal(buf, "-2.5");
}

/* ── path tree edges ─────────────────────────────────────────────────────── */

static void test_tree_insert_invalid(void **state)
{
    (void)state;
    assert_null(tree_insert_path("relative"));
    assert_null(tree_insert_path(NULL));
    assert_ptr_equal(tree_insert_path("/"), g_root);
}

static void test_lookup_rel_dotdot_at_root(void **state)
{
    (void)state;
    g_cwd_node = g_root;
    assert_ptr_equal(node_lookup_rel(".."), g_root);
    assert_ptr_equal(node_lookup_rel("."), g_root);
    assert_ptr_equal(node_lookup_rel(NULL), g_root);
}

static void test_collect_leaves_max(void **state)
{
    (void)state;
    for (int i = 0; i < 8; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/l%d", i);
        tree_add_endpoint(p, (uint32_t)i, "u", "/s");
    }
    path_node_t *list[8];
    int count = 0;
    collect_leaves(g_root, list, &count, 3);
    assert_int_equal(count, 3);          /* respects the max cap */
}

/* ── completion generators ───────────────────────────────────────────────── */

static void test_builtin_generator(void **state)
{
    (void)state;
    char *m = builtin_generator("l", 0);
    assert_non_null(m);
    assert_string_equal(m, "ls");
    free(m);
    assert_null(builtin_generator("l", 1));   /* only one l-builtin */

    m = builtin_generator("", 0);
    assert_non_null(m);                          /* any builtin */
    free(m);
}

static void test_path_generator(void **state)
{
    (void)state;
    tree_insert_path("/alpha/one");
    tree_insert_path("/beta");
    g_cwd_node = g_root;

    char *m = path_generator("al", 0);
    assert_non_null(m);
    assert_string_equal(m, "alpha");
    free(m);
    assert_null(path_generator("al", 1));
}

static void test_completer_root_and_builtin(void **state)
{
    (void)state;
    tree_insert_path("/alpha/one");
    tree_insert_path("/alpha/two");
    tree_insert_path("/leaf");
    g_cwd_node = g_root;

    /* absolute path completion */
    char line[] = "/al";
    rl_line_buffer = line;
    char **matches = fluxipc_completer("/al", 0, 3);
    assert_non_null(matches);
    assert_non_null(matches[0]);
    assert_string_equal(matches[0], "/alpha");
    assert_int_equal(rl_completion_append_character, '/');  /* container */
    for (int i = 0; matches[i]; i++) free(matches[i]);
    free(matches);

    /* builtin completion for the first token */
    char line2[] = "l";
    rl_line_buffer = line2;
    matches = fluxipc_completer("l", 0, 1);
    assert_non_null(matches);
    assert_non_null(matches[0]);
    assert_string_equal(matches[0], "ls");
    assert_int_equal(rl_completion_append_character, ' ');
    for (int i = 0; matches[i]; i++) free(matches[i]);
    free(matches);

    rl_line_buffer = NULL;
}

static void test_fs_generator(void **state)
{
    (void)state;
    char *r = fs_path_generator("!/etc/passw", 0);
    assert_non_null(r);
    assert_int_equal(r[0], '!');
    free(r);
}

/* ── cmd_watch argument handling (without entering the loop) ─────────────── */

static void test_cmd_watch_usage(void **state)
{
    (void)state;
    char *t1[] = { (char *)"watch" };
    ut_capture_begin();
    cmd_watch(1, t1);
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "Usage: watch"));

    char *t2[] = { (char *)"watch", (char *)"5" };
    ut_capture_begin();
    cmd_watch(2, t2);
    out = ut_capture_end();
    assert_non_null(strstr(out, "Usage: watch"));

    char *t3[] = { (char *)"watch", (char *)"1", (char *)"/nope" };
    ut_capture_begin();
    cmd_watch(3, t3);
    out = ut_capture_end();
    assert_non_null(strstr(out, "Unknown path"));
}

/* ── dispatch (builtins + real calls) ────────────────────────────────────── */

static void test_dispatch_builtins(void **state)
{
    (void)state;
    tree_add_endpoint("/a/leaf", 1, "u", "/sock");
    g_cwd_node = g_root;

    ut_capture_begin();
    dispatch("ls");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "a/leaf"));

    ut_capture_begin();
    dispatch("cd /a");
    out = ut_capture_end();
    assert_ptr_equal(g_cwd_node, node_lookup("/a"));

    ut_capture_begin();
    dispatch("cd /");
    ut_capture_end();
    assert_ptr_equal(g_cwd_node, g_root);

    ut_capture_begin();
    dispatch("definitely/unknown");
    out = ut_capture_end();
    assert_non_null(strstr(out, "Unknown path"));

    /* container dispatch lists its leaves */
    tree_add_endpoint("/c/x", 2, "u", "/sock");
    ut_capture_begin();
    dispatch("/c");
    out = ut_capture_end();
    assert_non_null(strstr(out, "x"));

    ut_capture_begin();
    dispatch("");
    ut_capture_end();                  /* empty line is a no-op */
}

static void test_dispatch_shell(void **state)
{
    (void)state;
    ut_capture_begin();
    dispatch("!");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "Usage: !"));

    ut_capture_begin();
    dispatch("!true");
    ut_capture_end();                  /* runs and succeeds */
}

static void test_dispatch_call_and_sweep(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"OK" };
    fluxipc_node_leaf(fluxipc_tree_root(), "e", &m);
    load_all_endpoints();
    g_cwd_node = g_root;

    ut_capture_begin();
    dispatch("e 1 2");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "OK"));

    ut_capture_begin();
    dispatch("e 1:3");
    out = ut_capture_end();
    assert_non_null(strstr(out, "Sweep"));
    assert_non_null(strstr(out, "total call"));

    /* escaped argument is passed literally */
    ut_capture_begin();
    dispatch("e \\0:100");
    out = ut_capture_end();
    assert_non_null(strstr(out, "OK"));
}

static void test_load_all_endpoints_local(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"X" };
    fluxipc_node_leaf(fluxipc_tree_root(), "local", &m);
    load_all_endpoints();
    assert_non_null(node_lookup("/local"));
    /* server routes are skipped (never query our own server) but local leaves load */
    assert_ptr_equal(g_cwd_node, g_root);
    assert_string_equal(g_prompt, "fluxipc> ");
}

/* ── completion corner cases ─────────────────────────────────────────────── */

static void free_matches(char **m)
{
    if (!m) return;
    for (int i = 0; m[i]; i++) free(m[i]);
    free(m);
}

static void test_build_candidates_nested(void **state)
{
    (void)state;
    tree_insert_path("/a/b/x");
    tree_insert_path("/a/b/y");
    tree_insert_path("/top");
    g_cwd_node = g_root;

    /* absolute nested */
    build_candidates("/a/b/");
    assert_int_equal(g_cand_count, 2);
    free_matches(g_candidates);
    g_candidates = NULL; g_cand_count = 0;

    /* relative nested prefix */
    build_candidates("a/b/");
    assert_int_equal(g_cand_count, 2);
    if (g_candidates) assert_string_equal(g_candidates[0], "a/b/x");
    free_matches(g_candidates);
    g_candidates = NULL; g_cand_count = 0;

    /* "./" prefix */
    build_candidates("./");
    assert_true(g_cand_dot_slash == 1);
    assert_int_equal(g_cand_count, 2);   /* a and top */
    free_matches(g_candidates);
    g_candidates = NULL; g_cand_count = 0;

    /* unknown parent → empty candidate set */
    build_candidates("nope/");
    assert_int_equal(g_cand_count, 0);
    free_matches(g_candidates);
    g_candidates = NULL; g_cand_count = 0;
}

static void test_completer_fs_prefix(void **state)
{
    (void)state;
    char line[] = "!/etc/passw";
    rl_line_buffer = line;
    char **m = fluxipc_completer("!/etc/passw", 0, 11);
    assert_non_null(m);
    assert_non_null(m[0]);
    assert_int_equal(m[0][0], '!');
    free_matches(m);
    rl_line_buffer = NULL;
}

static void test_completer_second_token(void **state)
{
    (void)state;
    tree_insert_path("/alpha");
    g_cwd_node = g_root;

    char line[] = "cd /al";
    rl_line_buffer = line;
    char **m = fluxipc_completer("/al", 3, 6);
    assert_non_null(m);
    assert_non_null(m[0]);
    assert_string_equal(m[0], "/alpha");
    free_matches(m);
    rl_line_buffer = NULL;
}

static void test_completer_relative_and_leaf(void **state)
{
    (void)state;
    tree_insert_path("/alpha/one");
    g_cwd_node = g_root;

    /* relative first token → do_path fallback */
    char line[] = "al";
    rl_line_buffer = line;
    char **m = fluxipc_completer("al", 0, 2);
    assert_non_null(m);
    assert_string_equal(m[0], "alpha");
    free_matches(m);

    /* ./ prefix with a container child */
    char line2[] = "./a";
    rl_line_buffer = line2;
    m = fluxipc_completer("./a", 0, 3);
    assert_non_null(m);
    assert_string_equal(m[0], "./alpha");
    assert_int_equal(rl_completion_append_character, '/');
    free_matches(m);

    /* exact leaf path prints usage and appends a space */
    tree_add_endpoint("/alpha/leaf", 1, "leaf usage here", "/s");
    char line3[] = "/alpha/leaf";
    rl_line_buffer = line3;
    m = fluxipc_completer("/alpha/leaf", 0, 11);
    assert_non_null(m);
    assert_int_equal(rl_completion_append_character, ' ');
    free_matches(m);
    rl_line_buffer = NULL;
}

static void test_completer_usage_on_second_tab(void **state)
{
    (void)state;
    tree_add_endpoint("/leaf", 1, "the usage", "/s");
    g_cwd_node = g_root;

    /* second token, empty text after a completed leaf → prints usage */
    char line[] = "/leaf ";
    rl_line_buffer = line;
    ut_capture_begin();
    char **m = fluxipc_completer("", 6, 6);
    const char *out = ut_capture_end();
    assert_null(m);
    assert_non_null(strstr(out, "the usage"));
    rl_line_buffer = NULL;
}

/* ── ls/help edges, load_list_cb, rel_path ───────────────────────────────── */

static void test_cmd_ls_empty_and_help_unknown(void **state)
{
    (void)state;
    path_node_t *empty = tree_insert_path("/empty");
    assert_non_null(empty);
    g_cwd_node = empty;

    ut_capture_begin();
    cmd_ls();
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "no entries under"));

    ut_capture_begin();
    cmd_help("/does/not/exist");
    out = ut_capture_end();
    assert_non_null(strstr(out, "No entry"));

    /* container path also reports "No entry" */
    ut_capture_begin();
    cmd_help("/empty");
    out = ut_capture_end();
    assert_non_null(strstr(out, "No entry"));
    g_cwd_node = g_root;
}

static void test_rel_path_self(void **state)
{
    (void)state;
    path_node_t *a = tree_insert_path("/a/b");
    assert_non_null(a);
    g_cwd_node = a;
    assert_string_equal(rel_path(a), "");   /* node == cwd */
    g_cwd_node = g_root;
}

static void test_load_list_cb_dedup(void **state)
{
    (void)state;
    load_cb_ctx_t lc;
    lc.sock_path = "/sockX";

    load_list_cb("/a/leaf", 5, "first", &lc);
    path_node_t *n = node_lookup("/a/leaf");
    assert_non_null(n);
    assert_int_equal(n->id, 5);

    /* second discovery is ignored (dedup across servers) */
    load_list_cb("/a/leaf", 9, "second", &lc);
    assert_int_equal(n->id, 5);
    assert_string_equal(n->usage, "first");
}

/* ── watch loop (signalled) ──────────────────────────────────────────────── */

static void *inter_send_sigint(void *arg)
{
    (void)arg;
    usleep(200 * 1000);
    kill(getpid(), SIGINT);
    return NULL;
}

static void test_watch_loop(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = ut_ipc_echo, .data = (void *)"W" };
    fluxipc_node_leaf(fluxipc_tree_root(), "e", &m);
    load_all_endpoints();

    char *t[] = { (char *)"watch", (char *)"1", (char *)"/e", (char *)"arg" };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, inter_send_sigint, NULL), 0);

    ut_capture_begin();
    cmd_watch(3, t);
    const char *out = ut_capture_end();
    pthread_join(th, NULL);

    assert_non_null(strstr(out, "Watching"));
}

/* ── dispatch argument handling ──────────────────────────────────────────── */

static int inter_arg_ipc(void *d, int argc, char **argv,
                         char *out, size_t cap, size_t *len)
{
    (void)d;
    const char *v = (argc > 1) ? argv[1] : "(none)";
    int n = snprintf(out, cap, "%s", v);
    if (len) *len = (size_t)n;
    return 0;
}

static void test_dispatch_arg_escapes(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = inter_arg_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "arg", &m);
    load_all_endpoints();

    ut_capture_begin();
    dispatch("arg 'quoted'");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "quoted"));

    ut_capture_begin();
    dispatch("arg \\escaped");
    out = ut_capture_end();
    assert_non_null(strstr(out, "escaped"));

    /* cartesian product over the call limit is refused */
    ut_capture_begin();
    dispatch("arg 0:4095 0:100");
    out = ut_capture_end();
    assert_non_null(strstr(out, "too many"));
}

static void test_tree_clear_null(void **state)
{
    (void)state;
    tree_destroy_subtree(g_root);   /* frees the root node itself */
    g_root = NULL;
    g_cwd_node = NULL;
    tree_clear();               /* g_root == NULL → early return */
    tree_init();
    assert_non_null(g_root);
    g_cwd_node = g_root;
}

static void test_fs_generator_bare_bang(void **state)
{
    (void)state;
    char *r = fs_path_generator("!", 0);
    assert_non_null(r);
    assert_int_equal(r[0], '!');
    free(r);
}

static void test_parse_arg_long_quoted(void **state)
{
    (void)state;
    char big[400];
    big[0] = '\'';
    memset(big + 1, 'a', 300);
    big[301] = '\'';
    big[302] = '\0';
    arg_spec_t s;
    parse_arg(big, &s);
    assert_false(s.is_range);
    assert_int_equal(strlen(s.literal), sizeof(s.literal) - 1);
}

static void test_dispatch_help_reload(void **state)
{
    (void)state;
    ut_capture_begin();
    dispatch("help");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "Built-in commands"));

    ut_capture_begin();
    dispatch("reload");
    out = ut_capture_end();
    assert_non_null(strstr(out, "Registry reloaded"));
}

static void test_dispatch_container_empty(void **state)
{
    (void)state;
    tree_insert_path("/empty");
    ut_capture_begin();
    dispatch("/empty");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "no leaf entries"));
}

static void test_dispatch_literal_and_range(void **state)
{
    (void)state;
    fluxipc_method_t m = { .ipc = inter_arg_ipc };
    fluxipc_node_leaf(fluxipc_tree_root(), "arg", &m);
    load_all_endpoints();

    ut_capture_begin();
    dispatch("arg lit 1:2");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "literal"));
    assert_non_null(strstr(out, "Sweep"));
}

static void test_dispatch_shell_false(void **state)
{
    (void)state;
    ut_capture_begin();
    dispatch("!false");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "exit: 1"));
}

static void test_dispatch_shell_args(void **state)
{
    (void)state;
    ut_capture_begin();
    dispatch("!echo !hello");
    const char *out = ut_capture_end();
    assert_non_null(strstr(out, "hello"));
}

static void test_load_all_endpoints_scan(void **state)
{
    (void)state;
    const char *scan = getenv("FLUXIPC_SCAN_DIR");
    assert_non_null(scan);

    /* one non-server file and one *-fluxipc dir that has no live socket */
    char plain[256], sub[256];
    snprintf(plain, sizeof(plain), "%s/plainfile", scan);
    snprintf(sub, sizeof(sub), "%s/foo%d-fluxipc", scan, (int)getpid());

    FILE *f = fopen(plain, "w");
    if (f) fclose(f);
    mkdir(sub, 0755);

    /* exercises the readdir loop, suffix filter, stat and failed-query path */
    load_all_endpoints();
    assert_ptr_equal(g_cwd_node, g_root);

    unlink(plain);
    rmdir(sub);
}

int ut_interactive(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_is_number, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_parse_arg_literal, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_parse_arg_ranges, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_parse_arg_errors, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_fmt_val, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_path_tree, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_rel_path, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_collect_leaves, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_cmd_cd, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_update_prompt, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_cmd_help_and_ls, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_build_candidates_relative, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_build_candidates_absolute, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_token_helpers, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_parse_arg_cap, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_sweep_call_count, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_fmt_val_large, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_tree_insert_invalid, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_lookup_rel_dotdot_at_root, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_collect_leaves_max, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_builtin_generator, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_path_generator, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_completer_root_and_builtin, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_fs_generator, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_cmd_watch_usage, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_builtins, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_shell, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_call_and_sweep,
                                        inter_srv_setup, inter_srv_teardown),
        cmocka_unit_test_setup_teardown(test_load_all_endpoints_local,
                                        inter_srv_setup, inter_srv_teardown),
        cmocka_unit_test_setup_teardown(test_build_candidates_nested, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_completer_fs_prefix, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_completer_second_token, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_completer_relative_and_leaf, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_completer_usage_on_second_tab, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_cmd_ls_empty_and_help_unknown, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_rel_path_self, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_load_list_cb_dedup, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_watch_loop, inter_srv_setup, inter_srv_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_arg_escapes, inter_srv_setup, inter_srv_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_shell_args, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_tree_clear_null, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_fs_generator_bare_bang, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_parse_arg_long_quoted, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_help_reload, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_container_empty, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_literal_and_range,
                                        inter_srv_setup, inter_srv_teardown),
        cmocka_unit_test_setup_teardown(test_dispatch_shell_false, inter_setup, inter_teardown),
        cmocka_unit_test_setup_teardown(test_load_all_endpoints_scan, inter_setup, inter_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
