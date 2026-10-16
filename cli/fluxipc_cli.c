/**
 * fluxipc_cli.c – universal client / interactive shell
 *
 * Invocation grammar:
 *
 *   fluxipc-cli <server> [command] [args...]
 *
 *   fluxipc-cli <server>                        list registered IPC paths
 *   fluxipc-cli <server> -l|--list              list registered IPC paths
 *   fluxipc-cli <server> -c|--call <path> [args...]   call one endpoint
 *   fluxipc-cli <server> -i|--interactive       enter the interactive shell
 *   fluxipc-cli -h|--help                       show help
 *
 * The first positional argument is always the server (program) name; the
 * optional second argument selects the action.  With no action the endpoint
 * list is shown, which keeps a bare `<server>` script-friendly.
 *
 * Runs fine as ordinary user (no sudo needed) because:
 *   - socket has mode 0666
 *   - /run/user/<uid>/<prog>-fluxipc/ has mode 0755
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
        "  %s <server>                                  list registered IPC paths\n"
        "  %s <server> -l, --list                       list registered IPC paths\n"
        "  %s <server> -c, --call <path> [args...]      call one endpoint\n"
        "  %s <server> -i, --interactive                enter the REPL (Tab completes)\n"
        "  %s -h, --help                                show this message\n"
        "\nOptions:\n"
        "  -h, --help    Show this message\n"
        "\nExamples:\n"
        "  %s myapp\n"
        "  %s myapp --list\n"
        "  %s myapp --call /devices/stub/xxx/name\n"
        "  %s myapp -c /demo/arithmetic/add 7 3\n"
        "  %s myapp -i\n",
        argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0);
}

static void print_list_entry(const char *path, uint32_t id,
                              const char *usage, void *userdata)
{
    (void)userdata;
    printf("%-42s  %-6u  %s\n", path, id, usage[0] ? usage : "-");
}

/** @return 0 on success, 1 on failure (server unreachable). */
static int cmd_list(const char *prog_name)
{
    printf("\n%-42s  %-6s  %s\n", "PATH", "ID", "USAGE");
    printf("%-42s  %-6s  %s\n",
           "------------------------------------------",
           "------", "-----------------------------");
    int rc = fluxipc_list_endpoints(prog_name, print_list_entry, NULL);
    if (rc < 0) {
        fprintf(stderr, "fluxipc: cannot reach server '%s' "
                "(is it running?)\n", prog_name);
        return 1;
    }
    printf("\n");
    return 0;
}

/** Call one endpoint over the server's Unix socket.
 *  @p argc / @p argv follow the IPC contract: argv[0] is the endpoint path,
 *  argv[1..] are the call arguments.  @return process exit status. */
static int cmd_call(const char *prog_name, const char *path,
                    int argc, char **argv)
{
    char sock_path[FLUXIPC_PATH_MAX];
    snprintf(sock_path, sizeof(sock_path),
             "/run/user/%d/%s-fluxipc/%s.sock", getuid(), prog_name, prog_name);

    char out[64 * 1024];
    size_t out_len = 0;
    int rc = fluxipc_call(sock_path, path, argc, argv,
                          out, sizeof(out), &out_len);
    if (rc < 0) {
        fprintf(stderr, "fluxipc error %d: %s\n", rc, strerror(-rc));
        return 1;
    }
    if (out_len > 0) {
        fwrite(out, 1, out_len, stdout);
        putchar('\n');
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { print_help(argv[0]); return 1; }

    /* Global help, before the server name. */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    /* The first positional argument is always the server name. */
    if (argv[1][0] == '-') {
        fprintf(stderr, "fluxipc: expected <server> as the first argument\n\n");
        print_help(argv[0]);
        return 1;
    }

    const char *prog = argv[1];

    /* No action given → list endpoints (safe, script-friendly default). */
    if (argc == 2)
        return cmd_list(prog);

    const char *cmd = argv[2];

    if (strcmp(cmd, "-l") == 0 || strcmp(cmd, "--list") == 0) {
        if (argc > 3) {
            fprintf(stderr, "fluxipc: --list takes no further arguments\n");
            return 1;
        }
        return cmd_list(prog);
    }

    if (strcmp(cmd, "-i") == 0 || strcmp(cmd, "--interactive") == 0) {
        if (argc > 3) {
            fprintf(stderr,
                    "fluxipc: --interactive takes no further arguments\n");
            return 1;
        }
        return fluxipc_interactive_init(prog);
    }

    if (strcmp(cmd, "-c") == 0 || strcmp(cmd, "--call") == 0) {
        if (argc < 4) {
            fprintf(stderr, "fluxipc: --call requires <path>\n");
            return 1;
        }
        /* The IPC contract is argv[0] == endpoint path, so hand over the
         * remaining argv starting at the path itself. */
        return cmd_call(prog, argv[3], argc - 3, argv + 3);
    }

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    fprintf(stderr, "fluxipc: unknown command '%s'\n\n", cmd);
    print_help(argv[0]);
    return 1;
}
