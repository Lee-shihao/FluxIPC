/**
 * main.c – unit-test runner.
 */

#include "test.h"

int g_checks = 0;
int g_fails  = 0;

static void run_suite(const char *name, void (*fn)(void))
{
    int before = g_fails;
    printf("[%s]\n", name);
    fn();
    printf("  %s\n", (g_fails == before) ? "ok" : "FAILED");
}

int main(void)
{
    run_suite("tree",     ut_tree);
    run_suite("util",     ut_util);
    run_suite("protocol", ut_protocol);
    run_suite("conn",     ut_conn);

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
