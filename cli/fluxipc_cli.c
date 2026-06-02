/**
 * fluxipc_cli.c – universal client / interactive shell
 *
 * Invocation modes:
 *   1. Symlink: /run/<prog>-fluxipc/<path> [args...]  (argv[0] encodes server+path)
 *   2. Direct:  fluxipc-cli --server <prog> <path> [args...]
 *   3. List:    fluxipc-cli --list <prog>
 *   4. REPL:    fluxipc-cli --interactive [<prog>]
 *
 * Runs fine as ordinary user (no sudo needed) because:
 *   - socket has mode 0666
 *   - shm has mode 0666 (needs RW for process-shared rwlock futex)
 *   - /run/<prog>-fluxipc/ has mode 0755
 */

#include "fluxipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static void print_help(const char *argv0)
{
    printf(
        "Usage:\n"
        "  %s --interactive,-i [<server>]           Enter REPL (Tab = one-level complete)\n"
        "  %s --list,-l <server>                    List registered IPC paths\n"
        "  %s --server,-s <server> <path> [args]    Call one endpoint\n"
        "  /run/<server>-fluxipc<path> [args]    Symlink invocation (zero-config)\n"
        "\nOptions:\n"
        "  -h, --help    Show this message\n"
        "\nExamples:\n"
        "  %s -s myapp /devices/stub/xxx/name\n"
        "  %s -i myapp\n"
        "  /run/myapp-fluxipc/devices/stub/xxx/name\n",
        argv0, argv0, argv0, argv0, argv0);
}

static void print_list_entry(const char *path, uint32_t id,
                              const char *usage, void *userdata)
{
    (void)userdata;
    printf("%-42s  %-6u  %s\n", path, id, usage[0] ? usage : "-");
}

static void cmd_list(const char *prog_name)
{
    printf("\n%-42s  %-6s  %s\n", "PATH", "ID", "USAGE");
    printf("%-42s  %-6s  %s\n",
           "------------------------------------------",
           "------", "-----------------------------");
    int rc = fluxipc_list_endpoints(prog_name, print_list_entry, NULL);
    if (rc < 0)
        fprintf(stderr, "fluxipc: cannot open shm for '%s' "
                "(is the server running?)\n", prog_name);
    else
        printf("\n");
}

int main(int argc, char **argv)
{
    /* ── Mode 1: symlink invocation ──────────────────────────────────────── */
    {
        int rc = fluxipc_client_dispatch(argc, argv);
        if (rc >= 0) return rc;
    }

    if (argc < 2) { print_help(argv[0]); return 1; }

    /* ── Mode 4: --interactive ───────────────────────────────────────────── */
    if (strcmp(argv[1], "--interactive") == 0 || strcmp(argv[1], "-i") == 0)
        return fluxipc_interactive_init(argc > 2 ? argv[2] : NULL);

    /* ── Mode 3: --list ──────────────────────────────────────────────────── */
    if (strcmp(argv[1], "--list") == 0 || strcmp(argv[1], "-l") == 0) {
        if (argc < 3) { fprintf(stderr, "fluxipc: --list requires <server>\n"); return 1; }
        cmd_list(argv[2]);
        return 0;
    }

    /* ── --help ──────────────────────────────────────────────────────────── */
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]); return 0;
    }

    /* ── Mode 2: --server <prog> <path> [args] ───────────────────────────── */
    if (strcmp(argv[1], "--server") == 0 || strcmp(argv[1], "-s") == 0) {
        if (argc < 4) {
            fprintf(stderr, "fluxipc: --server requires <prog> <path>\n");
            return 1;
        }
        const char *prog = argv[2];
        const char *path = argv[3];

        char sock_path[FLUXIPC_PATH_MAX];
        snprintf(sock_path, sizeof(sock_path),
                 "/run/user/%d/%s-fluxipc/%s.sock", getuid(), prog, prog);

        char out[64 * 1024];
        size_t out_len = 0;
        int rc = fluxipc_call(sock_path, path,
                              argc - 3, argv + 3,
                              out, sizeof(out), &out_len);
        if (rc < 0) {
            fprintf(stderr, "fluxipc error %d: %s\n", rc, strerror(-rc));
            return 1;
        }
        if (out_len > 0) fwrite(out, 1, out_len, stdout);
        return 0;
    }

    /* ── Bare path ──────────────────────────────────────────────────────── */
    if (argv[1][0] == '/') {
        char prog[FLUXIPC_NAME_MAX], sock[FLUXIPC_PATH_MAX], shm_n[64];
        /* try to resolve from argv[0] first, fall back to prompt */
        if (fluxipc_client_resolve(argv[0], prog, sizeof(prog),
                                    sock, sizeof(sock), shm_n, sizeof(shm_n)) < 0) {
            fprintf(stderr, "fluxipc: use --server <prog> <path> for direct calls\n");
            return 1;
        }
        char out[64 * 1024];
        size_t out_len = 0;
        int rc = fluxipc_call(sock, argv[1],
                              argc - 1, argv + 1,
                              out, sizeof(out), &out_len);
        if (rc < 0) { fprintf(stderr, "fluxipc error %d: %s\n", rc, strerror(-rc)); return 1; }
        if (out_len > 0) fwrite(out, 1, out_len, stdout);
        return 0;
    }

    print_help(argv[0]);
    return 1;
}
