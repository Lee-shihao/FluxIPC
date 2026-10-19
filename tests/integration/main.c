/**
 * integration/main.c – end-to-end tests against a real example_server.
 *
 * Covers the public IPC/JSON/HTTP surface plus protocol robustness,
 * concurrency, large payloads and a coarse server-RSS growth check.
 *
 * The server binary can be overridden with FLUXIPC_EXAMPLE_SERVER (used by
 * `make it-asan`).  FLUXIPC_TEST_VERBOSE=1 keeps the server's stderr visible
 * so AddressSanitizer/LeakSanitizer reports are not discarded.
 */

#include "../unit/test.h"
#include <fluxipc.h>
#include <fluxipc_internal.h>

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
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int g_checks = 0;
int g_fails  = 0;

static const int HTTP_PORT = 32100;
static pid_t     g_child   = -1;

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
        /* Hooks/usage are noisy on stdout; keep stderr only when explicitly
         * requested (ASan/LeakSanitizer reports). */
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

/** Ask the server to shut down through the public IPC surface, then reap it. */
static int shutdown_server(const char *sock)
{
    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/shutdown" };
    int rc = fluxipc_call(sock, "/system/shutdown", 1, av,
                          out, sizeof(out), &len);

    if (g_child <= 0) return rc < 0 ? rc : 0;

    int status = 0;
    for (int i = 0; i < 200; i++) {          /* up to 2 s */
        pid_t w = waitpid(g_child, &status, WNOHANG);
        if (w == g_child) {
            g_child = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return -1;
            return rc < 0 ? rc : 0;
        }
        if (w < 0 && errno == ECHILD) { g_child = -1; return rc < 0 ? rc : 0; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    stop_server();
    return -1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static long rss_kb(pid_t pid)
{
    char path[64];
    char line[256];
    long kb = -1;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

/* ── IPC helpers ────────────────────────────────────────────────────────── */

static int ipc(const char *sock, const char *path,
               int argc, char **argv, void *out, size_t cap, size_t *len)
{
    return fluxipc_call(sock, path, argc, argv, out, cap, len);
}

static cJSON *json_call(const char *sock, const char *path, const char *method,
                        int n, char **params, int *rc_out)
{
    char *out = NULL;
    size_t len = 0;
    int rc = fluxipc_call_json(sock, path, method, n, params, &out, &len);
    if (rc_out) *rc_out = rc;

    cJSON *j = out ? cJSON_ParseWithLength(out, len) : NULL;
    free(out);
    return j;
}

static int json_int(const cJSON *j, const char *key)
{
    const cJSON *v = j ? cJSON_GetObjectItemCaseSensitive(j, key) : NULL;
    return cJSON_IsNumber(v) ? v->valueint : -999999;
}

static const char *json_str(const cJSON *j, const char *key)
{
    const cJSON *v = j ? cJSON_GetObjectItemCaseSensitive(j, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ── HTTP helpers ───────────────────────────────────────────────────────── */

static int send_all_fd(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all_fd(int fd, char *buf, size_t cap)
{
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n > 0) { used += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    buf[used] = '\0';
    return (int)used;
}

static int tcp_connect(int port, const char *host)
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
    return fd;
}

static int http_request(int port, const char *method, const char *path,
                        const char *host, const char *body,
                        char *resp, size_t cap)
{
    int fd = tcp_connect(port, host);
    if (fd < 0) return -1;

    char hdr[1024];
    size_t blen = body ? strlen(body) : 0;
    int hl;
    if (body) {
        hl = snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
            method, path, host, blen);
    } else {
        hl = snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, host);
    }
    if (hl < 0 || (size_t)hl >= sizeof(hdr)) { close(fd); return -1; }

    if (send_all_fd(fd, hdr, (size_t)hl) < 0 ||
        (body && send_all_fd(fd, body, blen) < 0)) {
        close(fd);
        return -1;
    }

    int n = read_all_fd(fd, resp, cap);
    close(fd);
    return n;
}

static int http_get(int port, const char *path, char *resp, size_t cap,
                    const char *host)
{
    return http_request(port, "GET", path, host, NULL, resp, cap);
}

static int http_raw(int port, const char *raw, char *resp, size_t cap)
{
    int fd = tcp_connect(port, "127.0.0.1");
    if (fd < 0) return -1;
    if (send_all_fd(fd, raw, strlen(raw)) < 0) { close(fd); return -1; }
    int n = read_all_fd(fd, resp, cap);
    close(fd);
    return n;
}

static int http_status(const char *resp)
{
    int code = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &code) == 1) return code;
    return -1;
}

static char *http_body(char *resp)
{
    char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
}

static cJSON *http_json(char *resp)
{
    return cJSON_Parse(http_body(resp));
}

/* ── endpoint-list callback ─────────────────────────────────────────────── */

static void list_cb(const char *path, uint32_t id, const char *usage, void *ud)
{
    (void)path; (void)id; (void)usage;
    (*(int *)ud)++;
}

/* ── raw IPC protocol helpers ───────────────────────────────────────────── */

static int unix_connect(const char *sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_bad_header(const char *sock, uint32_t magic, uint16_t op,
                            uint16_t path_len, uint32_t arg_len,
                            const char *tail, size_t tail_len)
{
    int fd = unix_connect(sock);
    if (fd < 0) return;

    fluxipc_req_hdr_t h = {
        .magic = magic, .op = op, .path_len = path_len, .arg_len = arg_len,
    };
    (void)send_all_fd(fd, &h, sizeof(h));
    if (tail && tail_len) (void)send_all_fd(fd, tail, tail_len);
    close(fd);
}

static int server_alive(const char *sock)
{
    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };
    return ipc(sock, "/system/info", 1, av, out, sizeof(out), &len);
}

/* ── concurrency worker ─────────────────────────────────────────────────── */

typedef struct {
    const char *sock;
    int         iters;
    int         id;
    int         errors;
} worker_arg_t;

static void *mix_worker(void *arg)
{
    worker_arg_t *a = arg;

    for (int i = 0; i < a->iters; i++) {
        char out[4096];
        size_t len = 0;
        char *av[] = { (char *)"/system/info" };
        if (ipc(a->sock, "/system/info", 1, av,
                out, sizeof(out), &len) != 0)
            a->errors++;

        char x[32], y[32];
        char *params[2] = { x, y };
        snprintf(x, sizeof(x), "%d", i);
        snprintf(y, sizeof(y), "%d", a->id);

        char *jout = NULL;
        size_t jlen = 0;
        int rc = fluxipc_call_json(a->sock, "/demo/arithmetic/add", "GET",
                                   2, params, &jout, &jlen);
        if (rc != 0 || !jout) {
            a->errors++;
        } else {
            cJSON *j = cJSON_ParseWithLength(jout, jlen);
            if (!j || !cJSON_IsNumber(
                    cJSON_GetObjectItemCaseSensitive(j, "result")))
                a->errors++;
            cJSON_Delete(j);
        }
        free(jout);

        char resp[8192];
        int n = http_get(HTTP_PORT, "/system/info", resp, sizeof(resp),
                         "127.0.0.1");
        if (n <= 0 || http_status(resp) != 200) a->errors++;
    }
    return NULL;
}

/* ── large JSON payload helper ──────────────────────────────────────────── */

static char *make_echo_body(size_t payload_len)
{
    size_t cap = payload_len + 64;
    char *body = malloc(cap);
    if (!body) return NULL;

    int n = snprintf(body, cap, "{\"args\":[\"");
    if (n < 0) { free(body); return NULL; }
    memset(body + n, 'x', payload_len);
    memcpy(body + n + payload_len, "\"]}", 3);
    body[n + payload_len + 3] = '\0';
    return body;
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
    if (g_child <= 0) {
        printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
        return 1;
    }
    atexit(stop_server);

    char out[8192];
    char *argv0 = (char *)"/system/info";

    /* ── readiness + regression: first connect must not be -EIO ── */
    printf("[ipc]\n");
    int rc = -1;
    for (int i = 0; i < 1000; i++) {
        rc = ipc(sock, "/system/info", 1, &argv0, out, sizeof(out), NULL);
        if (rc == 0) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK_EQ_INT(rc, 0);                       /* -EIO here == pool bug */
    CHECK(strstr(out, "program=example_server") != NULL);

    /* ── IPC with arguments ── */
    {
        char *av[3] = { (char *)"/demo/arithmetic/add",
                        (char *)"7", (char *)"3" };
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
        char *av[1] = { (char *)"/system/compiled" };
        rc = ipc(sock, "/system/compiled", 1, av, out, sizeof(out), NULL);
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

    /* ── local JSON representation ── */
    printf("[json-ipc]\n");
    {
        int jrc = 0;
        cJSON *j = json_call(sock, "/system/info", NULL, 0, NULL, &jrc);
        CHECK_EQ_INT(jrc, 0);
        CHECK(j != NULL);
        CHECK_EQ_STR(json_str(j, "program"), "example_server");
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"7", (char *)"3" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/demo/arithmetic/add", "GET",
                             2, p, &jrc);
        CHECK_EQ_INT(jrc, 0);
        CHECK_EQ_INT(json_int(j, "result"), 10);
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"{\"a\":8,\"b\":9}" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/demo/arithmetic/add", "POST",
                             1, p, &jrc);
        CHECK_EQ_INT(jrc, 0);
        CHECK_EQ_INT(json_int(j, "result"), 17);
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"a=4", (char *)"b=5" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/demo/arithmetic/add", "GET",
                             2, p, &jrc);
        CHECK_EQ_INT(jrc, 0);
        CHECK_EQ_INT(json_int(j, "result"), 9);
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"{\"level\":9}" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/config/log_level", "PUT",
                             1, p, &jrc);
        CHECK_EQ_INT(jrc, -ERANGE);
        CHECK_EQ_INT(json_int(j, "code"), -ERANGE);
        cJSON_Delete(j);
    }
    {
        int jrc = 0;
        cJSON *j = json_call(sock, "/system/compiled", NULL, 0, NULL, &jrc);
        CHECK_EQ_INT(jrc, -ENOSYS);
        CHECK_EQ_INT(json_int(j, "code"), -ENOSYS);
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"{bad" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/demo/arithmetic/add", "POST",
                             1, p, &jrc);
        CHECK_EQ_INT(jrc, -EINVAL);
        CHECK_EQ_INT(json_int(j, "code"), -EINVAL);
        cJSON_Delete(j);
    }
    {
        char *p[] = { (char *)"1" };
        int jrc = 0;
        cJSON *j = json_call(sock, "/system/info", "GET", 1, p, &jrc);
        CHECK_EQ_INT(jrc, -EINVAL);
        CHECK_EQ_INT(json_int(j, "code"), -EINVAL);
        cJSON_Delete(j);
    }

    /* ── large JSON body over IPC ── */
    {
        const size_t payload_len = 32 * 1024;
        char *body = make_echo_body(payload_len);
        char *p[] = { body };
        char *jout = NULL;
        size_t jlen = 0;
        CHECK(body != NULL);
        if (body) {
            int jrc = fluxipc_call_json(sock, "/demo/echo", "POST",
                                        1, p, &jout, &jlen);
            CHECK_EQ_INT(jrc, 0);
            CHECK(jout != NULL);
            CHECK(jlen > payload_len);
            CHECK(jout && strstr(jout, "xxxx") != NULL);
            free(jout);
            free(body);
        }
    }

    /* ── HTTP ── */
    printf("[http]\n");
    char resp[65536];

    rc = http_get(HTTP_PORT, "/system/info", resp, sizeof(resp), "127.0.0.1");
    CHECK(rc > 0);
    CHECK_EQ_INT(http_status(resp), 200);
    CHECK(strstr(http_body(resp), "\"program\":\"example_server\"") != NULL);

    rc = http_get(HTTP_PORT, "/", resp, sizeof(resp), "127.0.0.1");
    CHECK_EQ_INT(http_status(resp), 200);
    CHECK(strstr(http_body(resp), "\"kind\":\"collection\"") != NULL);

    rc = http_get(HTTP_PORT, "/module", resp, sizeof(resp), "127.0.0.1");
    CHECK_EQ_INT(http_status(resp), 200);
    CHECK(strstr(http_body(resp), "\"schema\"") != NULL);
    CHECK(strstr(http_body(resp), "\"rpm\"") != NULL);

    rc = http_get(HTTP_PORT, "/does/not/exist", resp, sizeof(resp), "127.0.0.1");
    CHECK_EQ_INT(http_status(resp), 404);
    CHECK(strstr(resp, "application/problem+json") != NULL);

    /* FLUXIPC_STATIC registered IPC only, so HTTP reports 501 */
    rc = http_get(HTTP_PORT, "/system/compiled", resp, sizeof(resp), "127.0.0.1");
    CHECK_EQ_INT(http_status(resp), 501);

    /* write + read through the structured handler */
    rc = http_request(HTTP_PORT, "PUT", "/config/log_level", "127.0.0.1",
                      "{\"level\":3}", resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 200);
    {
        cJSON *j = http_json(resp);
        CHECK_EQ_INT(json_int(j, "log_level"), 3);
        cJSON_Delete(j);
    }
    rc = http_get(HTTP_PORT, "/config/log_level", resp, sizeof(resp), "127.0.0.1");
    {
        cJSON *j = http_json(resp);
        CHECK_EQ_INT(json_int(j, "log_level"), 3);
        cJSON_Delete(j);
    }

    /* semantic errno -> status mapping */
    rc = http_request(HTTP_PORT, "PUT", "/config/log_level", "127.0.0.1",
                      "{\"level\":9}", resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 422);
    {
        cJSON *j = http_json(resp);
        CHECK_EQ_INT(json_int(j, "code"), -ERANGE);
        cJSON_Delete(j);
    }

    /* restore default-ish state */
    (void)http_request(HTTP_PORT, "PUT", "/config/log_level", "127.0.0.1",
                       "{\"level\":2}", resp, sizeof(resp));

    rc = http_request(HTTP_PORT, "OPTIONS", "/system/info", "127.0.0.1",
                      NULL, resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 204);
    CHECK(strstr(resp, "Allow: GET, HEAD, OPTIONS") != NULL);

    rc = http_request(HTTP_PORT, "HEAD", "/system/info", "127.0.0.1",
                      NULL, resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 200);
    CHECK_EQ_INT((int)strlen(http_body(resp)), 0);

    rc = http_request(HTTP_PORT, "DELETE", "/system/info", "127.0.0.1",
                      NULL, resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 405);
    CHECK(strstr(resp, "Allow:") != NULL);

    rc = http_get(HTTP_PORT, "/openapi.json", resp, sizeof(resp), "127.0.0.1");
    CHECK_EQ_INT(http_status(resp), 200);
    CHECK(strstr(http_body(resp), "\"openapi\":\"3.1.0\"") != NULL);
    CHECK(strstr(http_body(resp), "\"rpm\"") != NULL);

    /* malformed / oversized HTTP input */
    rc = http_raw(HTTP_PORT, "BAD\r\n\r\n", resp, sizeof(resp));
    CHECK_EQ_INT(http_status(resp), 400);

    {
        size_t cap = 20 * 1024;
        char *raw = malloc(cap);
        CHECK(raw != NULL);
        if (raw) {
            int n = snprintf(raw, cap, "GET / HTTP/1.1\r\nX-Overflow: ");
            memset(raw + n, 'a', 18 * 1024);
            memcpy(raw + n + 18 * 1024, "\r\n\r\n", 4);
            raw[n + 18 * 1024 + 4] = '\0';
            rc = http_raw(HTTP_PORT, raw, resp, sizeof(resp));
            CHECK_EQ_INT(http_status(resp), 431);
            free(raw);
        }
    }
    {
        const char *raw =
            "POST /demo/echo HTTP/1.1\r\nHost: x\r\n"
            "Content-Length: 300000\r\n\r\n";
        rc = http_raw(HTTP_PORT, raw, resp, sizeof(resp));
        CHECK_EQ_INT(http_status(resp), 413);
    }

    /* large HTTP body */
    {
        const size_t payload_len = 32 * 1024;
        char *body = make_echo_body(payload_len);
        char *big = malloc(128 * 1024);
        CHECK(body != NULL);
        CHECK(big != NULL);
        if (body && big) {
            rc = http_request(HTTP_PORT, "POST", "/demo/echo", "127.0.0.1",
                              body, big, 128 * 1024);
            CHECK(rc > 0);
            CHECK_EQ_INT(http_status(big), 200);
            CHECK(strstr(http_body(big), "xxxx") != NULL);
        }
        free(big);
        free(body);
    }

    /* ── raw IPC protocol robustness; server must survive ── */
    printf("[protocol-robustness]\n");
    send_bad_header(sock, 0xDEADBEEF, FLUXIPC_OP_CALL, 0, 0, NULL, 0);
    usleep(20 * 1000);
    CHECK_EQ_INT(server_alive(sock), 0);

    send_bad_header(sock, FLUXIPC_PROTO_MAGIC, 99, 0, 0, NULL, 0);
    usleep(20 * 1000);
    CHECK_EQ_INT(server_alive(sock), 0);

    /* A request larger than the per-connection buffer must be rejected, not
     * left waiting forever and occupying a pool slot. */
    send_bad_header(sock, FLUXIPC_PROTO_MAGIC, FLUXIPC_OP_CALL, 0,
                    600000, NULL, 0);
    usleep(20 * 1000);
    CHECK_EQ_INT(server_alive(sock), 0);

    send_bad_header(sock, FLUXIPC_PROTO_MAGIC, FLUXIPC_OP_CALL,
                    FLUXIPC_PATH_MAX, 0, NULL, 0);
    usleep(20 * 1000);
    CHECK_EQ_INT(server_alive(sock), 0);

    send_bad_header(sock, FLUXIPC_PROTO_MAGIC, FLUXIPC_OP_CALL,
                    (uint16_t)strlen("/system/info"), 4, "abcd", 4);
    usleep(20 * 1000);
    CHECK_EQ_INT(server_alive(sock), 0);

    {
        int fd = unix_connect(sock);
        CHECK(fd >= 0);
        if (fd >= 0) {
            const char partial[5] = { 'F', 'L', 'X', '2', 0 };
            (void)send_all_fd(fd, partial, sizeof(partial));
            close(fd);
        }
        usleep(20 * 1000);
        CHECK_EQ_INT(server_alive(sock), 0);
    }

    /* ── concurrent mixed workload ── */
    printf("[concurrency]\n");
    {
        enum { WORKERS = 4, ITERS = 20 };
        pthread_t th[WORKERS];
        worker_arg_t args[WORKERS];
        int created = 0;

        memset(args, 0, sizeof(args));
        for (int i = 0; i < WORKERS; i++) {
            args[i].sock  = sock;
            args[i].iters = ITERS;
            args[i].id    = i;
            if (pthread_create(&th[i], NULL, mix_worker, &args[i]) == 0)
                created++;
        }
        CHECK_EQ_INT(created, WORKERS);
        for (int i = 0; i < created; i++) {
            pthread_join(th[i], NULL);
            CHECK_EQ_INT(args[i].errors, 0);
        }
    }

    /* ── coarse memory-growth check + throughput sanity ── */
    printf("[memory]\n");
    {
        char *jout = NULL;
        size_t jlen = 0;

        for (int i = 0; i < 100; i++) {
            jout = NULL;
            (void)fluxipc_call_json(sock, "/system/info", NULL, 0, NULL,
                                    &jout, &jlen);
            free(jout);
        }

        long rss0 = rss_kb(g_child);
        double t0 = now_ms();
        int errors = 0;

        for (int i = 0; i < 1000; i++) {
            jout = NULL;
            if (fluxipc_call_json(sock, "/system/info", NULL, 0, NULL,
                                  &jout, &jlen) != 0)
                errors++;
            free(jout);
        }

        for (int i = 0; i < 100; i++) {
            if (http_get(HTTP_PORT, "/system/info", resp, sizeof(resp),
                         "127.0.0.1") <= 0)
                errors++;
        }

        double elapsed = now_ms() - t0;
        long rss1 = rss_kb(g_child);
        CHECK_EQ_INT(errors, 0);
        CHECK(rss0 > 0);
        CHECK(rss1 > 0);
        printf("    1000 JSON IPC + 100 HTTP in %.1f ms (%.0f ops/s), "
               "server RSS %ld -> %ld kB\n",
               elapsed, 1100.0 * 1000.0 / elapsed, rss0, rss1);

#if !defined(__SANITIZE_ADDRESS__)
        CHECK(rss1 - rss0 < 32 * 1024);       /* < 32 MiB growth */
#else
        (void)rss0; (void)rss1;
#endif
    }

    /* ── graceful shutdown (also lets LeakSanitizer report the server) ── */
    printf("[shutdown]\n");
    CHECK_EQ_INT(shutdown_server(sock), 0);
    stop_server();                            /* no-op after graceful exit */

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
