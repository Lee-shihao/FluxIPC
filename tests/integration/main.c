/**
 * integration/main.c – end-to-end tests against a real example_server.
 *
 * Exercises the public IPC surface: dispatch, argument passing, listing,
 * concurrency, large payloads and graceful shutdown.  The example server runs
 * its event loop in a host-created thread, so these tests also cover the
 * external-thread scheduling API.
 *
 * The server binary can be overridden with FLUXIPC_EXAMPLE_SERVER (used by
 * `make it-asan`).  FLUXIPC_TEST_VERBOSE=1 keeps the server's stderr visible
 * so AddressSanitizer/LeakSanitizer reports are not discarded.
 */

#include "../unit/test.h"
#include <fluxipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/wait.h>

int g_checks = 0;
int g_fails  = 0;

static pid_t     g_child = -1;
static char      g_sock[FLUXIPC_PATH_MAX];

/* ── process / timing helpers ───────────────────────────────────────────── */

static const char *server_path(void)
{
    const char *p = getenv("FLUXIPC_EXAMPLE_SERVER");
    return (p && *p) ? p : "./example_server";
}

static pid_t start_server(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            if (!getenv("FLUXIPC_TEST_VERBOSE")) dup2(dn, STDERR_FILENO);
            close(dn);
        }
        execl(server_path(), "example_server", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop_server(void)
{
    if (g_child > 0) {
        kill(g_child, SIGTERM);
        waitpid(g_child, NULL, 0);
        g_child = -1;
    }
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

static int wait_ready(void)
{
    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };

    for (int i = 0; i < 300; i++) {
        if (fluxipc_call(g_sock, "/system/info", 1, av,
                         out, sizeof(out), &len) == 0)
            return 0;
        sleep_ms(10);
    }
    return -1;
}

static int shutdown_server(void)
{
    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/shutdown" };
    int rc = fluxipc_call(g_sock, "/system/shutdown", 1, av,
                          out, sizeof(out), &len);

    if (g_child <= 0) return rc < 0 ? rc : 0;

    int status = 0;
    for (int i = 0; i < 300; i++) {          /* up to 3 s */
        pid_t w = waitpid(g_child, &status, WNOHANG);
        if (w == g_child) {
            g_child = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return -1;
            return rc < 0 ? rc : 0;
        }
        if (w < 0 && errno == ECHILD) { g_child = -1; return rc < 0 ? rc : 0; }
        sleep_ms(10);
    }

    stop_server();
    return -1;
}

/* ── IPC helpers ────────────────────────────────────────────────────────── */

static int ipc(const char *path, int argc, char **argv,
               void *out, size_t cap, size_t *len)
{
    return fluxipc_call(g_sock, path, argc, argv, out, cap, len);
}

/* ── checks ─────────────────────────────────────────────────────────────── */

static void test_system_info(void)
{
    char out[512];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };
    CHECK_EQ_INT(ipc("/system/info", 1, av, out, sizeof(out), &len), 0);
    CHECK(len > 0);
    CHECK(memmem(out, len, "example_server", 14) != NULL);
}

static void test_arithmetic(void)
{
    char out[64];
    size_t len = 0;

    char *add[] = { (char *)"/demo/arithmetic/add", (char *)"7", (char *)"3" };
    CHECK_EQ_INT(ipc("/demo/arithmetic/add", 3, add,
                     out, sizeof(out), &len), 0);
    CHECK_EQ_INT(len, 2);
    CHECK_EQ_INT(out[0], '1');
    CHECK_EQ_INT(out[1], '0');

    char *mul[] = { (char *)"/demo/arithmetic/mul", (char *)"6", (char *)"7" };
    CHECK_EQ_INT(ipc("/demo/arithmetic/mul", 3, mul,
                     out, sizeof(out), &len), 0);
    CHECK_EQ_INT(len, 2);
    CHECK_EQ_INT(out[0], '4');
    CHECK_EQ_INT(out[1], '2');
}

static void test_echo_and_errors(void)
{
    char out[256];
    size_t len = 0;

    char *av[] = { (char *)"/demo/echo", (char *)"hello", (char *)"world" };
    CHECK_EQ_INT(ipc("/demo/echo", 3, av, out, sizeof(out), &len), 0);
    CHECK(memmem(out, len, "hello world", 11) != NULL);

    /* Unknown path → -ENOENT, no output. */
    len = 123;
    CHECK_EQ_INT(ipc("/no/such/endpoint", 0, NULL,
                     out, sizeof(out), &len), -ENOENT);
    CHECK_EQ_INT(len, 0);

    /* Container is not callable. */
    CHECK_EQ_INT(ipc("/demo", 0, NULL, out, sizeof(out), &len), -ENOENT);

    /* Out-of-range config is reported as an error string by the handler. */
    char *bad[] = { (char *)"/config/log_level", (char *)"99" };
    CHECK_EQ_INT(ipc("/config/log_level", 2, bad,
                     out, sizeof(out), &len), 0);
    CHECK(memmem(out, len, "error=", 6) != NULL);
}

static void test_large_payload(void)
{
    const size_t n = 32 * 1024;
    char *big = malloc(n + 1);
    CHECK(big != NULL);
    if (!big) return;
    memset(big, 'A', n);
    big[n] = '\0';

    char *av[] = { (char *)"/demo/echo", big };
    char out[64 * 1024];
    size_t len = 0;
    CHECK_EQ_INT(ipc("/demo/echo", 2, av, out, sizeof(out), &len), 0);
    CHECK_EQ_INT(len, n);
    CHECK_EQ_INT(out[0], 'A');
    CHECK_EQ_INT(out[n - 1], 'A');
    free(big);
}

/* ── listing ────────────────────────────────────────────────────────────── */

static int g_list_count;
static int g_list_hit;

static void list_cb(const char *path, uint32_t id, const char *usage,
                    void *userdata)
{
    (void)id; (void)usage; (void)userdata;
    if (!path) return;
    g_list_count++;
    if (strcmp(path, "/system/info") == 0) g_list_hit = 1;
}

static void test_list_endpoints(void)
{
    g_list_count = 0;
    g_list_hit = 0;
    CHECK_EQ_INT(fluxipc_list_endpoints("example_server", list_cb, NULL), 0);
    CHECK(g_list_count >= 10);
    CHECK_EQ_INT(g_list_hit, 1);
}

/* ── concurrency ────────────────────────────────────────────────────────── */

typedef struct {
    int iters;
    int errors;
} worker_arg_t;

static void *worker_fn(void *arg)
{
    worker_arg_t *w = arg;
    char out[512];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };

    for (int i = 0; i < w->iters; i++) {
        if (fluxipc_call(g_sock, "/system/info", 1, av,
                         out, sizeof(out), &len) != 0)
            w->errors++;
    }
    return NULL;
}

static void test_concurrent_clients(void)
{
    enum { THREADS = 8, ITERS = 100 };
    pthread_t th[THREADS];
    worker_arg_t args[THREADS];

    for (int i = 0; i < THREADS; i++) {
        args[i].iters = ITERS;
        args[i].errors = 0;
        CHECK_EQ_INT(pthread_create(&th[i], NULL, worker_fn, &args[i]), 0);
    }

    int errors = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        errors += args[i].errors;
    }
    CHECK_EQ_INT(errors, 0);
}

static void test_repeated_calls(void)
{
    char out[512];
    size_t len = 0;
    char *av[] = { (char *)"/system/version" };
    int errors = 0;

    for (int i = 0; i < 500; i++) {
        if (ipc("/system/version", 1, av, out, sizeof(out), &len) != 0)
            errors++;
    }
    CHECK_EQ_INT(errors, 0);
    CHECK(memmem(out, len, "version=", 8) != NULL);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    snprintf(g_sock, sizeof(g_sock),
             "/run/user/%d/example_server-fluxipc/example_server.sock",
             (int)getuid());

    /* Make sure a stale server from a previous run cannot confuse us. */
    stop_server();

    g_child = start_server();
    if (g_child <= 0) {
        fprintf(stderr, "integration: cannot start %s\n", server_path());
        return 1;
    }
    atexit(stop_server);

    if (wait_ready() != 0) {
        fprintf(stderr, "integration: server did not become ready\n");
        stop_server();
        return 1;
    }

    test_system_info();
    test_arithmetic();
    test_echo_and_errors();
    test_large_payload();
    test_list_endpoints();
    test_concurrent_clients();
    test_repeated_calls();

    int rc = shutdown_server();
    CHECK_EQ_INT(rc, 0);
    stop_server();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
