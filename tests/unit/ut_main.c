/**
 * ut_main.c – unit-test entry point (cmocka based).
 *
 * Each suite is a function returning the number of failed tests; the process
 * exits non-zero if any suite failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int ut_protocol(void);
int ut_tree(void);
int ut_core(void);
int ut_sock(void);
int ut_conn(void);
int ut_interactive(void);
int ut_cli(void);

int main(void)
{
    int failed = 0;

    /* Keep endpoint auto-discovery hermetic: point it at an empty directory
     * instead of /run/user/<uid>, so tests never query other (possibly
     * parallel, possibly stale) servers. */
    const char *scan = "/tmp/fluxipc-ut-scan";
    mkdir(scan, 0755);
    setenv("FLUXIPC_SCAN_DIR", scan, 1);


    failed += ut_protocol();
    failed += ut_tree();
    failed += ut_core();
    failed += ut_sock();
    failed += ut_conn();
    failed += ut_interactive();
    failed += ut_cli();

    if (failed) {
        fprintf(stderr, "\n== %d test(s) FAILED ==\n", failed);
        return 1;
    }
    printf("\n== all unit tests passed ==\n");
    return 0;
}
