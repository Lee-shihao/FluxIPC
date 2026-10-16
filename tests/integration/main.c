/**
 * integration/main.c – end-to-end tests against a real example_server.
 *
 * Starts the server, then exercises the IPC and HTTP transports (including the
 * "connect immediately after startup" regression that used to fail with -EIO).
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
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int g_checks = 0;
int g_fails  = 0;

static const int HTTP_PORT = 32100;
static pid_t     g_child   = -1;

/* ── helpers ────────────────────────────────────────────────────────────── */

static pid_t start_server(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
        execl("./example_server", "example_server", (char *)NULL);
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

static int ipc(const char *sock, const char *path,
               int argc, char **argv, void *out, size_t cap, size_t *len)
{
    return fluxipc_call(sock, path, argc, argv, out, cap, len);
}

/** Very small HTTP/1.1 client: sends GET and reads until close. */
static int http_get(int port, const char *path, char *resp, size_t cap,
                    const char *host)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, host);
    if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) < 0) { close(fd); return -1; }

    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) break;
        ssize_t n = recv(fd, resp + used, cap - used - 1, 0);
        if (n > 0) { used += (size_t)n; continue; }
        break;
    }
    resp[used] = '\0';
    close(fd);
    return (int)used;
}

static void list_cb(const char *path, uint32_t id, const char *usage, void *ud)
{
    (void)path; (void)id; (void)usage;
    (*(int *)ud)++;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    char sock[FLUXIPC_PATH_MAX];
    snprintf(sock, sizeof(sock),
             "/run/user/%d/example_server-fluxipc/example_server.sock",
             (int)getuid());

    g_child = start_server();
    CHECK(g_child > 0);
    if (g_child <= 0) { printf("\n%d checks, %d failure(s)\n", g_checks, g_fails); return 1; }

    char out[8192];
    char *argv0 = (char *)"/system/info";

    /* ── readiness + regression: first connect must not be -EIO ── */
    printf("[ipc]\n");
    int rc = -1;
    for (int i = 0; i < 300; i++) {
        rc = ipc(sock, "/system/info", 1, &argv0, out, sizeof(out), NULL);
        if (rc != -ENOENT) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK_EQ_INT(rc, 0);                       /* -EIO here == pool bug */
    CHECK(strstr(out, "program=example_server") != NULL);

    /* ── IPC with arguments ── */
    {
        char *av[3] = { (char *)"/demo/arithmetic/add", (char *)"7", (char *)"3" };
        size_t len = 0;
        rc = ipc(sock, "/demo/arithmetic/add", 3, av, out, sizeof(out), &len);
        CHECK_EQ_INT(rc, 0);
        CHECK(strstr(out, "add: 7 + 3 = 10") != NULL);
        CHECK_EQ_INT(len, strlen("add: 7 + 3 = 10"));
    }

    /* ── IPC error propagation (out of range -> negative status) ── */
    {
        char *av[2] = { (char *)"/devices/io/led/value", (char *)"999" };
        rc = ipc(sock, "/devices/io/led/value", 2, av, out, sizeof(out), NULL);
        CHECK(rc < 0);
    }

    /* ── compile-time endpoint (FLUXIPC_STATIC) ── */
    {
        char *av1[1] = { (char *)"/system/compiled" };
        rc = ipc(sock, "/system/compiled", 1, av1, out, sizeof(out), NULL);
        CHECK_EQ_INT(rc, 0);
        CHECK(strstr(out, "compiled endpoint: FLUXIPC_STATIC") != NULL);
    }

    /* ── endpoint listing (OP_LIST) ── */
    {
        int n = 0;
        rc = fluxipc_list_endpoints("example_server", list_cb, &n);
        CHECK_EQ_INT(rc, 0);
        CHECK(n >= 20);
    }

    /* ── HTTP ── */
    printf("[http]\n");
    char resp[16384];

    rc = http_get(HTTP_PORT, "/system/info", resp, sizeof(resp), "127.0.0.1");
    CHECK(rc > 0);
    CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CHECK(strstr(resp, "\"program\":\"example_server\"") != NULL);

    rc = http_get(HTTP_PORT, "/", resp, sizeof(resp), "127.0.0.1");
    CHECK(strstr(resp, "\"kind\":\"collection\"") != NULL);

    rc = http_get(HTTP_PORT, "/does/not/exist", resp, sizeof(resp), "127.0.0.1");
    CHECK(strstr(resp, "HTTP/1.1 404") != NULL);
    CHECK(strstr(resp, "application/problem+json") != NULL);

    /* FLUXIPC_STATIC registered IPC only, so HTTP reports 501 */
    rc = http_get(HTTP_PORT, "/system/compiled", resp, sizeof(resp), "127.0.0.1");
    CHECK(strstr(resp, "HTTP/1.1 501") != NULL);

    stop_server();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
