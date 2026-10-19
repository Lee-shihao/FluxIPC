/**
 * tests/bench/bench.c – FluxIPC latency / throughput baseline.
 *
 * Self-contained: starts ./example_server (or FLUXIPC_EXAMPLE_SERVER), runs
 * sequential and parallel IPC/JSON/HTTP workloads, prints p50/p95/p99 and
 * throughput, then shuts the server down through the public IPC API.
 *
 * Environment:
 *   BENCH_IPC_ITERS    default 5000
 *   BENCH_HTTP_ITERS   default 1000
 *   BENCH_THREADS      default 4
 *   BENCH_WARMUP       default 200
 *   BENCH_VERBOSE=1    keep server stdout/stderr
 *
 * Optional gates (off by default):
 *   BENCH_MIN_IPC_OPS, BENCH_MIN_HTTP_OPS, BENCH_MAX_P99_MS
 */

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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_PORT 32100

typedef struct {
    const char *name;
    long        ops;
    double      total_ms;
    double      ops_per_sec;
    double      mean_ms;
    double      p50_ms;
    double      p95_ms;
    double      p99_ms;
    double      max_ms;
    long        errors;
} result_t;

static pid_t g_child = -1;

/* ── process helpers ────────────────────────────────────────────────────── */

static const char *server_path(void)
{
    const char *p = getenv("FLUXIPC_EXAMPLE_SERVER");
    return (p && *p) ? p : "./example_server";
}

static pid_t start_server(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (!getenv("BENCH_VERBOSE")) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
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

static int shutdown_server(const char *sock)
{
    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/shutdown" };
    int rc = fluxipc_call(sock, "/system/shutdown", 1, av,
                          out, sizeof(out), &len);

    if (g_child > 0) {
        int status = 0;
        if (waitpid(g_child, &status, 0) == g_child) {
            g_child = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
        }
    }
    return rc < 0 ? rc : 0;
}

static long rss_kb(pid_t pid)
{
    char path[64], line[256];
    long kb = -1;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── HTTP client ────────────────────────────────────────────────────────── */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int http_get_once(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(HTTP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    static const char req[] =
        "GET /system/info HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (send_all(fd, req, sizeof(req) - 1) < 0) { close(fd); return -1; }

    char buf[4096];
    size_t used = 0;
    int ok = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - used, 0);
        if (n > 0) {
            used += (size_t)n;
            if (used >= 12 && memcmp(buf, "HTTP/1.1 200", 12) == 0) ok = 1;
            if (used == sizeof(buf)) used = 0;   /* discard body */
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return ok ? 0 : -1;
}

/* ── workload primitives ────────────────────────────────────────────────── */

static int ipc_once(const char *sock)
{
    char out[4096];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };
    return fluxipc_call(sock, "/system/info", 1, av,
                        out, sizeof(out), &len);
}

static int ipc_json_once(const char *sock)
{
    char *out = NULL;
    size_t len = 0;
    int rc = fluxipc_call_json(sock, "/system/info", NULL, 0, NULL,
                               &out, &len);
    free(out);
    return rc;
}

static int ipc_json_pos_once(const char *sock)
{
    char *out = NULL;
    size_t len = 0;
    char *params[] = { (char *)"7", (char *)"3" };
    int rc = fluxipc_call_json(sock, "/demo/arithmetic/add", "GET",
                               2, params, &out, &len);
    free(out);
    return rc;
}

/* ── statistics ─────────────────────────────────────────────────────────── */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double pct(const double *v, long n, double p)
{
    if (n <= 0) return 0.0;
    long idx = (long)(p * (double)(n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return v[idx];
}

static void finish(result_t *r, const char *name, long ops, long errors,
                   double total_ms, double *lat)
{
    double sum = 0.0;
    for (long i = 0; i < ops; i++) sum += lat[i];
    qsort(lat, (size_t)ops, sizeof(double), cmp_double);

    r->name        = name;
    r->ops         = ops;
    r->errors      = errors;
    r->total_ms    = total_ms;
    r->ops_per_sec = total_ms > 0 ? (double)ops * 1000.0 / total_ms : 0.0;
    r->mean_ms     = ops > 0 ? sum / (double)ops : 0.0;
    r->p50_ms      = pct(lat, ops, 0.50);
    r->p95_ms      = pct(lat, ops, 0.95);
    r->p99_ms      = pct(lat, ops, 0.99);
    r->max_ms      = ops > 0 ? lat[ops - 1] : 0.0;
}

static void print_result(const result_t *r)
{
    printf("%-16s %8ld %10.2f %12.0f %9.3f %9.3f %9.3f %9.3f %9.3f %6ld\n",
           r->name, r->ops, r->total_ms, r->ops_per_sec,
           r->mean_ms, r->p50_ms, r->p95_ms, r->p99_ms, r->max_ms,
           r->errors);
}

/* ── sequential workloads ───────────────────────────────────────────────── */

static void bench_ipc_seq(const char *sock, long iters, long warmup,
                          result_t *out)
{
    for (long i = 0; i < warmup; i++) (void)ipc_once(sock);

    double *lat = malloc((size_t)iters * sizeof(*lat));
    if (!lat) { memset(out, 0, sizeof(*out)); out->name = "ipc-seq"; return; }

    long errors = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        uint64_t a = now_ns();
        if (ipc_once(sock) != 0) errors++;
        lat[i] = (double)(now_ns() - a) / 1.0e6;
    }
    double total = (double)(now_ns() - t0) / 1.0e6;
    finish(out, "ipc-seq", iters, errors, total, lat);
    free(lat);
}

static void bench_ipc_json_seq(const char *sock, long iters, long warmup,
                               result_t *out)
{
    for (long i = 0; i < warmup; i++) (void)ipc_json_once(sock);

    double *lat = malloc((size_t)iters * sizeof(*lat));
    if (!lat) { memset(out, 0, sizeof(*out)); out->name = "ipc-json-seq"; return; }

    long errors = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        uint64_t a = now_ns();
        if (ipc_json_once(sock) != 0) errors++;
        lat[i] = (double)(now_ns() - a) / 1.0e6;
    }
    double total = (double)(now_ns() - t0) / 1.0e6;
    finish(out, "ipc-json-seq", iters, errors, total, lat);
    free(lat);
}

static void bench_ipc_json_pos_seq(const char *sock, long iters, long warmup,
                                   result_t *out)
{
    for (long i = 0; i < warmup; i++) (void)ipc_json_pos_once(sock);

    double *lat = malloc((size_t)iters * sizeof(*lat));
    if (!lat) { memset(out, 0, sizeof(*out)); out->name = "ipc-json-pos"; return; }

    long errors = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        uint64_t a = now_ns();
        if (ipc_json_pos_once(sock) != 0) errors++;
        lat[i] = (double)(now_ns() - a) / 1.0e6;
    }
    double total = (double)(now_ns() - t0) / 1.0e6;
    finish(out, "ipc-json-pos", iters, errors, total, lat);
    free(lat);
}

static void bench_http_seq(long iters, long warmup, result_t *out)
{
    for (long i = 0; i < warmup; i++) (void)http_get_once();

    double *lat = malloc((size_t)iters * sizeof(*lat));
    if (!lat) { memset(out, 0, sizeof(*out)); out->name = "http-seq"; return; }

    long errors = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        uint64_t a = now_ns();
        if (http_get_once() != 0) errors++;
        lat[i] = (double)(now_ns() - a) / 1.0e6;
    }
    double total = (double)(now_ns() - t0) / 1.0e6;
    finish(out, "http-seq", iters, errors, total, lat);
    free(lat);
}

/* ── parallel workloads ─────────────────────────────────────────────────── */

typedef struct {
    const char *sock;
    long        iters;
    double     *lat;
    long        base;
    long        errors;
    int         http;
} thread_arg_t;

static void *thread_fn(void *p)
{
    thread_arg_t *a = p;
    for (long i = 0; i < a->iters; i++) {
        uint64_t t = now_ns();
        int rc = a->http ? http_get_once() : ipc_once(a->sock);
        a->lat[a->base + i] = (double)(now_ns() - t) / 1.0e6;
        if (rc != 0) a->errors++;
    }
    return NULL;
}

static void bench_parallel(const char *sock, long total_iters, int threads,
                           int http, long warmup, result_t *out)
{
    if (threads < 1) threads = 1;
    if (total_iters < threads) total_iters = threads;
    long per = total_iters / threads;
    long ops = per * threads;

    for (long i = 0; i < warmup; i++) {
        if (http) (void)http_get_once(); else (void)ipc_once(sock);
    }

    double *lat = malloc((size_t)ops * sizeof(*lat));
    pthread_t *th = malloc((size_t)threads * sizeof(*th));
    thread_arg_t *args = calloc((size_t)threads, sizeof(*args));
    if (!lat || !th || !args) {
        free(lat); free(th); free(args);
        memset(out, 0, sizeof(*out));
        out->name = http ? "http-parallel" : "ipc-parallel";
        return;
    }

    for (int i = 0; i < threads; i++) {
        args[i].sock  = sock;
        args[i].iters = per;
        args[i].lat   = lat;
        args[i].base  = (long)i * per;
        args[i].http  = http;
    }

    uint64_t t0 = now_ns();
    int created = 0;
    for (int i = 0; i < threads; i++)
        if (pthread_create(&th[i], NULL, thread_fn, &args[i]) == 0)
            created++;

    long errors = 0;
    for (int i = 0; i < created; i++) {
        pthread_join(th[i], NULL);
        errors += args[i].errors;
    }
    double total = (double)(now_ns() - t0) / 1.0e6;

    finish(out, http ? "http-parallel" : "ipc-parallel",
           (long)created * per, errors, total, lat);

    free(args);
    free(th);
    free(lat);
}

/* ── main ───────────────────────────────────────────────────────────────── */

static long env_long(const char *name, long def)
{
    const char *s = getenv(name);
    if (!s || !*s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    return (end && *end == '\0' && v > 0) ? v : def;
}

int main(void)
{
    long ipc_iters  = env_long("BENCH_IPC_ITERS", 5000);
    long http_iters = env_long("BENCH_HTTP_ITERS", 1000);
    int  threads    = (int)env_long("BENCH_THREADS", 4);
    long warmup     = env_long("BENCH_WARMUP", 200);

    char sock[FLUXIPC_PATH_MAX];
    snprintf(sock, sizeof(sock),
             "/run/user/%d/example_server-fluxipc/example_server.sock",
             (int)getuid());

    g_child = start_server();
    if (g_child <= 0) {
        fprintf(stderr, "bench: cannot start %s\n", server_path());
        return 1;
    }
    atexit(stop_server);

    char out[256];
    size_t len = 0;
    char *av[] = { (char *)"/system/info" };
    int rc = -1;
    for (int i = 0; i < 1000; i++) {
        rc = fluxipc_call(sock, "/system/info", 1, av,
                          out, sizeof(out), &len);
        if (rc == 0) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (rc != 0) {
        fprintf(stderr, "bench: server did not become ready (%d)\n", rc);
        return 1;
    }

    long rss0 = rss_kb(g_child);

    result_t results[6];
    bench_ipc_seq(sock, ipc_iters, warmup, &results[0]);
    bench_ipc_json_seq(sock, ipc_iters, warmup, &results[1]);
    bench_ipc_json_pos_seq(sock, ipc_iters, warmup, &results[2]);
    bench_http_seq(http_iters, warmup, &results[3]);
    bench_parallel(sock, ipc_iters / 2, threads, 0, warmup, &results[4]);
    bench_parallel(sock, http_iters / 2, threads, 1, warmup, &results[5]);

    long rss1 = rss_kb(g_child);

    printf("FluxIPC benchmark: ipc=%ld http=%ld threads=%d warmup=%ld\n",
           ipc_iters, http_iters, threads, warmup);
    printf("%-16s %8s %10s %12s %9s %9s %9s %9s %9s %6s\n",
           "benchmark", "ops", "total_ms", "ops/s", "mean_ms",
           "p50_ms", "p95_ms", "p99_ms", "max_ms", "errors");

    long total_errors = 0;
    for (int i = 0; i < 6; i++) {
        print_result(&results[i]);
        total_errors += results[i].errors;
    }
    printf("server RSS: %ld -> %ld kB\n", rss0, rss1);

    int failed = total_errors != 0;

    const char *min_ipc = getenv("BENCH_MIN_IPC_OPS");
    if (min_ipc && results[0].ops_per_sec < strtod(min_ipc, NULL)) {
        fprintf(stderr, "bench: ipc ops/s below %s\n", min_ipc);
        failed = 1;
    }
    const char *min_http = getenv("BENCH_MIN_HTTP_OPS");
    if (min_http && results[2].ops_per_sec < strtod(min_http, NULL)) {
        fprintf(stderr, "bench: http ops/s below %s\n", min_http);
        failed = 1;
    }
    const char *max_p99 = getenv("BENCH_MAX_P99_MS");
    if (max_p99) {
        double lim = strtod(max_p99, NULL);
        for (int i = 0; i < 6; i++) {
            if (results[i].p99_ms > lim) {
                fprintf(stderr, "bench: %s p99 %.3f > %.3f ms\n",
                        results[i].name, results[i].p99_ms, lim);
                failed = 1;
            }
        }
    }

    if (shutdown_server(sock) != 0) failed = 1;
    stop_server();

    return failed ? 1 : 0;
}
