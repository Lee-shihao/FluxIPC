/**
 * example_server.c – FluxIPC demo server (IPC only)
 *
 * Demonstrates the recommended threading model: the library creates no
 * threads.  main() registers the endpoints and then starts a host-created
 * thread that runs fluxipc_run(); the main thread stays free for application
 * work and wakes up when /system/shutdown is called.
 *
 * Try:
 *   ./example_server
 *   fluxipc-cli example_server -c /system/info
 *   fluxipc-cli example_server -c /demo/arithmetic/add 7 3
 *   fluxipc-cli example_server --list
 */

#include "fluxipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

/* ==========================================================================
 *  Handler helpers
 * ========================================================================== */

/* IPC: formatted text into the caller buffer (the original contract). */
static int ipc_out(void *out, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)out, cap, fmt, ap);
    va_end(ap);
    if (n < 0) { if (len) *len = 0; return -EINVAL; }
    if (len) *len = (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
    return 0;
}

static int arg_int(int argc, char **argv, int idx, int def)
{
    if (idx >= argc || !argv[idx]) return def;
    char *end = NULL;
    long v = strtol(argv[idx], &end, 10);
    return (end && *end == '\0') ? (int)v : def;
}

/* ==========================================================================
 *  Shared application state
 * ========================================================================== */

static time_t  g_start_time;
static int     g_log_level   = 3;
static int     g_max_clients = 32;
static long    g_counter;

static sem_t   g_shutdown_sem;

/* ==========================================================================
 *  system
 * ========================================================================== */

static int system_info_ipc(void *d, int argc, char **argv,
                           void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len, "program=example_server pid=%d uid=%d",
                   getpid(), getuid());
}

static int system_uptime_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    long up = (long)(time(NULL) - g_start_time);
    return ipc_out(out, cap, len, "uptime_seconds=%ld uptime_minutes=%ld",
                   up, up / 60);
}

static int system_version_ipc(void *d, int argc, char **argv,
                              void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len, "version=2.2.0 build_date=%s", __DATE__);
}

static int system_shutdown_ipc(void *d, int argc, char **argv,
                               void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    fluxipc_stop();
    sem_post(&g_shutdown_sem);
    return ipc_out(out, cap, len, "shutting down... bye!");
}

/* ==========================================================================
 *  demo
 * ========================================================================== */

static int demo_echo_ipc(void *d, int argc, char **argv,
                         void *out, size_t cap, size_t *len)
{
    (void)d;
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        int n = snprintf((char *)out + used, used < cap ? cap - used : 0,
                         "%s%s", i > 1 ? " " : "", argv[i]);
        if (n > 0) used += (size_t)n;
        if (used >= cap) break;
    }
    if (used > cap) used = cap;
    if (len) *len = used;
    return 0;
}

static int demo_counter_ipc(void *d, int argc, char **argv,
                            void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    /* The dispatch thread is single-threaded; no extra locking needed. */
    return ipc_out(out, cap, len, "counter=%ld", ++g_counter);
}

static int demo_add_ipc(void *d, int argc, char **argv,
                        void *out, size_t cap, size_t *len)
{
    (void)d;
    int a = arg_int(argc, argv, 1, 0);
    int b = arg_int(argc, argv, 2, 0);
    return ipc_out(out, cap, len, "%d", a + b);
}

static int demo_mul_ipc(void *d, int argc, char **argv,
                        void *out, size_t cap, size_t *len)
{
    (void)d;
    int a = arg_int(argc, argv, 1, 0);
    int b = arg_int(argc, argv, 2, 0);
    return ipc_out(out, cap, len, "%d", a * b);
}

/* ==========================================================================
 *  module/sensor
 * ========================================================================== */

static struct {
    double temperature;
    double humidity;
    int    sample_rate_hz;
} g_sensor = { 21.5, 48.0, 10 };

static int sensor_status_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len,
                   "temperature=%.2f humidity=%.2f sample_rate=%d",
                   g_sensor.temperature, g_sensor.humidity,
                   g_sensor.sample_rate_hz);
}

static int sensor_config_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) {
        int rate = arg_int(argc, argv, 1, -1);
        if (rate <= 0 || rate > 1000)
            return ipc_out(out, cap, len, "error=rate_hz must be 1..1000");
        g_sensor.sample_rate_hz = rate;
    }
    return ipc_out(out, cap, len, "sample_rate=%d", g_sensor.sample_rate_hz);
}

/* ==========================================================================
 *  config
 * ========================================================================== */

static int config_log_level_ipc(void *d, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) {
        int lvl = arg_int(argc, argv, 1, -1);
        if (lvl < 0 || lvl > 5)
            return ipc_out(out, cap, len, "error=level must be 0..5");
        g_log_level = lvl;
    }
    return ipc_out(out, cap, len, "log_level=%d", g_log_level);
}

static int config_max_clients_ipc(void *d, int argc, char **argv,
                                  void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) {
        int n = arg_int(argc, argv, 1, -1);
        if (n <= 0 || n > 1024)
            return ipc_out(out, cap, len, "error=max_clients must be 1..1024");
        g_max_clients = n;
    }
    return ipc_out(out, cap, len, "max_clients=%d", g_max_clients);
}

/* ==========================================================================
 *  Dynamic device registration
 * ========================================================================== */

typedef struct {
    char name[64];
    int  value;
    int  min_val;
    int  max_val;
} device_ctx_t;

static int device_name_ipc(void *data, int argc, char **argv,
                           void *out, size_t cap, size_t *len)
{
    device_ctx_t *ctx = data;
    if (argc > 1 && argv[1] && argv[1][0])
        snprintf(ctx->name, sizeof(ctx->name), "%s", argv[1]);
    return ipc_out(out, cap, len, "%s", ctx->name);
}

static int device_value_ipc(void *data, int argc, char **argv,
                            void *out, size_t cap, size_t *len)
{
    device_ctx_t *ctx = data;
    if (argc > 1) {
        int v = arg_int(argc, argv, 1, ctx->value);
        if (v < ctx->min_val || v > ctx->max_val)
            return ipc_out(out, cap, len, "error=value out of range [%d,%d]",
                           ctx->min_val, ctx->max_val);
        ctx->value = v;
    }
    return ipc_out(out, cap, len, "value=%d", ctx->value);
}

static int device_info_ipc(void *data, int argc, char **argv,
                           void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    device_ctx_t *ctx = data;
    return ipc_out(out, cap, len, "name=%s value=%d range=[%d,%d]",
                   ctx->name, ctx->value, ctx->min_val, ctx->max_val);
}

static void register_device(const char *base, const char *name,
                            int value, int min_val, int max_val)
{
    static device_ctx_t devs[8];
    static int dev_idx = 0;
    if (dev_idx >= 8) return;

    device_ctx_t *ctx = &devs[dev_idx++];
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    ctx->value   = value;
    ctx->min_val = min_val;
    ctx->max_val = max_val;

    char path[FLUXIPC_PATH_MAX];

    snprintf(path, sizeof(path), "%s/name", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(device_name_ipc, ctx, "Get or set the device name"));

    snprintf(path, sizeof(path), "%s/value", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(
        device_value_ipc, ctx, "Get or set the device value (range checked)"));

    snprintf(path, sizeof(path), "%s/info", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(device_info_ipc, ctx, "Show full device info"));
}

/* ==========================================================================
 *  Request observation hooks
 * ========================================================================== */

static void req_pre_hook(const fluxipc_request_t *ev, void *user)
{
    (void)user;
    printf("[hook:pre ] %s matched=%d\n", ev->path, ev->matched);
}

static void req_post_hook(const fluxipc_request_t *ev, void *user)
{
    (void)user;
    printf("[hook:post] %s status=%d\n", ev->path, ev->status);
}

/* ==========================================================================
 *  Registration
 * ========================================================================== */

static void register_endpoints(void)
{
    /* One row per endpoint: intermediate containers are created for us. */
    static const fluxipc_endpoint_t endpoints[] = {
        { "/system/info",     FLUXIPC_METHOD(system_info_ipc, "Show server process info") },
        { "/system/uptime",   FLUXIPC_METHOD(system_uptime_ipc, "Show server uptime") },
        { "/system/version",  FLUXIPC_METHOD(system_version_ipc, "Show server version and build date") },
        { "/system/shutdown", FLUXIPC_METHOD(system_shutdown_ipc, "Gracefully stop the server") },

        { "/module/sensor/status", FLUXIPC_METHOD(sensor_status_ipc, "Query current sensor readings") },
        { "/module/sensor/config", FLUXIPC_METHOD(sensor_config_ipc, "Get or set the sensor sample rate") },

        { "/config/log_level",   FLUXIPC_METHOD(config_log_level_ipc,   "Get or set server log level (0-5)") },
        { "/config/max_clients", FLUXIPC_METHOD(config_max_clients_ipc, "Get or set max concurrent clients") },

        { "/demo/echo",           FLUXIPC_METHOD(demo_echo_ipc, "Echo back all arguments") },
        { "/demo/counter",        FLUXIPC_METHOD(demo_counter_ipc, "Atomically increment and return counter") },
        { "/demo/arithmetic/add", FLUXIPC_METHOD(demo_add_ipc, "Add two integers") },
        { "/demo/arithmetic/mul", FLUXIPC_METHOD(demo_mul_ipc, "Multiply two integers") },
    };
    fluxipc_register_all(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));

    register_device("/devices/stub/primary",   "stub-primary",   100,   0, 255);
    register_device("/devices/stub/secondary", "stub-secondary",  50, -50, 150);
    register_device("/devices/io/led",         "status-led",      1,   0, 1);
    register_device("/devices/io/relay",       "power-relay",     0,   0, 1);
}

/* ==========================================================================
 *  Compile-time endpoint (FLUXIPC_STATIC)
 *
 *  Queued by a constructor before main(); registered by
 *  fluxipc_server_init().  No linker section or -rdynamic required.
 * ========================================================================== */

static int compiled_info_ipc(void *data, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    return ipc_out(out, cap, len, "compiled endpoint: %s",
                   (const char *)data);
}
FLUXIPC_STATIC("/system/compiled", "Compile-time registered endpoint", 0,
               compiled_info_ipc, "FLUXIPC_STATIC");

/* ==========================================================================
 *  main – host-owned thread scheduling
 * ========================================================================== */

static void sig_handler(int s)
{
    (void)s;
    fluxipc_stop();
    sem_post(&g_shutdown_sem);
}

static void *server_thread(void *arg)
{
    (void)arg;
    int rc = fluxipc_run();
    if (rc < 0)
        fprintf(stderr, "fluxipc_run: %d (%s)\n", rc, strerror(-rc));
    sem_post(&g_shutdown_sem);
    return NULL;
}

int main(void)
{
    g_start_time = time(NULL);
    sem_init(&g_shutdown_sem, 0, 0);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int rc = fluxipc_server_init("example_server");
    if (rc < 0) {
        fprintf(stderr, "fluxipc_server_init: %d\n", rc);
        return 1;
    }

    fluxipc_set_request_hooks(req_pre_hook, req_post_hook, NULL);
    register_endpoints();
    fluxipc_usage("example_server");

    printf("Examples:\n"
           "  IPC   fluxipc-cli example_server -c /system/info\n"
           "  IPC   fluxipc-cli example_server -c /demo/arithmetic/add 7 3\n"
           "  IPC   fluxipc-cli example_server --list\n\n");

    /* The host application owns the polling thread. */
    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "example_server: cannot start polling thread\n");
        fluxipc_destroy();
        sem_destroy(&g_shutdown_sem);
        return 1;
    }

    /* Main thread is free to do application work; it sleeps until shutdown. */
    sem_wait(&g_shutdown_sem);

    printf("[server] shutting down (uptime %ld s)\n",
           (long)(time(NULL) - g_start_time));

    fluxipc_stop();            /* idempotent; also unblocks a failed loop */
    pthread_join(th, NULL);    /* destroy only after the poll thread is gone */
    fluxipc_destroy();
    sem_destroy(&g_shutdown_sem);
    return 0;
}
