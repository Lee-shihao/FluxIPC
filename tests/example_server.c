/**
 * example_server.c – rich IPC demo: static + dynamic registration,
 *                     deep hierarchy, multiple subsystems.
 *
 * Symlink dispatch via /run/example_server-fluxipc/<path> acts as client.
 */

#include "fluxipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

/* ==========================================================================
 *  Static handlers – system
 * ========================================================================== */

static time_t g_start_time;

static int system_info_handler(void *data, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    int n = snprintf(out, cap,
        "program=example_server pid=%d uid=%d", getpid(), getuid());
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/system/info",
               "Show server process info",
               0, system_info_handler, NULL);

static int system_uptime_handler(void *data, int argc, char **argv,
                                  void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    long uptime = (long)(time(NULL) - g_start_time);
    int n = snprintf(out, cap,
        "uptime_seconds=%ld uptime_minutes=%ld", uptime, uptime / 60);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/system/uptime",
               "Show server uptime",
               0, system_uptime_handler, NULL);

static int system_version_handler(void *data, int argc, char **argv,
                                   void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    int n = snprintf(out, cap,
        "version=2.1.0 build_date=%s", __DATE__);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/system/version",
               "Show server version and build date",
               0, system_version_handler, NULL);

static int system_shutdown_handler(void *data, int argc, char **argv,
                                    void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    int n = snprintf(out, cap, "shutting down... bye!");
    *len = (size_t)(n > 0 ? n : 0);
    fluxipc_stop();
    return 0;
}
FLUXIPC_STATIC("/system/shutdown",
               "Gracefully stop the server",
               0, system_shutdown_handler, NULL);

/* ==========================================================================
 *  Static handlers – module/sensor
 * ========================================================================== */

typedef struct {
    double temperature;
    double humidity;
    int    sample_rate_hz;
} sensor_state_t;

static sensor_state_t g_sensor = { 23.5, 58.2, 10 };

static int sensor_status_handler(void *data, int argc, char **argv,
                                  void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    sensor_state_t *s = &g_sensor;
    int n = snprintf(out, cap,
        "temperature=%.1f humidity=%.1f sample_rate=%d ok=true",
        s->temperature, s->humidity, s->sample_rate_hz);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/sensor/status",
               "Query current sensor readings",
               0, sensor_status_handler, NULL);

static int sensor_calibrate_handler(void *data, int argc, char **argv,
                                     void *out, size_t cap, size_t *len)
{
    (void)data;
    const char *offset = (argc > 1) ? argv[1] : "0";
    int n = snprintf(out, cap, "calibration applied: offset=%s result=ok", offset);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/sensor/calibrate",
               "<offset>  Apply temperature calibration offset",
               0, sensor_calibrate_handler, NULL);

static int sensor_config_handler(void *data, int argc, char **argv,
                                  void *out, size_t cap, size_t *len)
{
    (void)data;
    if (argc > 1) g_sensor.sample_rate_hz = atoi(argv[1]);
    int n = snprintf(out, cap, "sample_rate=%d", g_sensor.sample_rate_hz);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/sensor/config",
               "[<rate_hz>]  Get/set sensor sample rate",
               0, sensor_config_handler, NULL);

/* ==========================================================================
 *  Static handlers – module/motor
 * ========================================================================== */

typedef struct {
    int speed_rpm;
    int position_deg;
    int enabled;
} motor_state_t;

static motor_state_t g_motor = { 1200, 90, 1 };

static int motor_speed_handler(void *data, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)data;
    if (!g_motor.enabled) {
        int n = snprintf(out, cap, "error: motor is disabled");
        *len = (size_t)(n > 0 ? n : 0);
        return -1;
    }
    if (argc > 1) g_motor.speed_rpm = atoi(argv[1]);
    int n = snprintf(out, cap, "speed=%d rpm", g_motor.speed_rpm);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/motor/speed",
               "[<rpm>]  Get/set motor speed",
               0, motor_speed_handler, NULL);

static int motor_position_handler(void *data, int argc, char **argv,
                                   void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    if (!g_motor.enabled) {
        int n = snprintf(out, cap, "error: motor is disabled");
        *len = (size_t)(n > 0 ? n : 0);
        return -1;
    }
    int n = snprintf(out, cap, "position=%d deg", g_motor.position_deg);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/motor/position",
               "Query current motor position",
               0, motor_position_handler, NULL);

static int motor_stop_handler(void *data, int argc, char **argv,
                               void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    g_motor.enabled = 0;
    g_motor.speed_rpm = 0;
    int n = snprintf(out, cap, "motor stopped: enabled=%d speed=%d",
                     g_motor.enabled, g_motor.speed_rpm);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/module/motor/stop",
               "Emergency stop motor",
               0, motor_stop_handler, NULL);

/* ==========================================================================
 *  Static handlers – config
 * ========================================================================== */

static int g_log_level = 2;
static int g_max_clients = 16;

static int config_log_level_handler(void *data, int argc, char **argv,
                                     void *out, size_t cap, size_t *len)
{
    (void)data;
    if (argc > 1) {
        int lvl = atoi(argv[1]);
        if (lvl < 0 || lvl > 5) {
            int n = snprintf(out, cap, "error: log_level must be 0–5");
            *len = (size_t)(n > 0 ? n : 0);
            return -1;
        }
        g_log_level = lvl;
    }
    int n = snprintf(out, cap, "log_level=%d", g_log_level);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/config/log_level",
               "[<0-5>]  Get/set server log level",
               0, config_log_level_handler, NULL);

static int config_max_clients_handler(void *data, int argc, char **argv,
                                       void *out, size_t cap, size_t *len)
{
    (void)data;
    if (argc > 1) g_max_clients = atoi(argv[1]);
    int n = snprintf(out, cap, "max_clients=%d", g_max_clients);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/config/max_clients",
               "[<n>]  Get/set max concurrent clients",
               0, config_max_clients_handler, NULL);

/* ==========================================================================
 *  Static handlers – demo utilities
 * ========================================================================== */

static int g_counter = 0;

static int demo_echo_handler(void *data, int argc, char **argv,
                              void *out, size_t cap, size_t *len)
{
    (void)data;
    int off = snprintf(out, cap, "echo:");
    for (int i = 1; i < argc && (size_t)off < cap - 1; i++)
        off += snprintf((char *)out + off, cap - (size_t)off, " %s", argv[i]);
    *len = (size_t)off;
    return 0;
}
FLUXIPC_STATIC("/demo/echo",
               "<args...>  Echo back all arguments",
               0, demo_echo_handler, NULL);

static int demo_counter_handler(void *data, int argc, char **argv,
                                 void *out, size_t cap, size_t *len)
{
    (void)data; (void)argc; (void)argv;
    int val = __sync_fetch_and_add(&g_counter, 1);
    int n = snprintf(out, cap, "counter=%d", val);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/demo/counter",
               "Atomically increment and return counter",
               0, demo_counter_handler, NULL);

static int demo_arithmetic_add_handler(void *data, int argc, char **argv,
                                        void *out, size_t cap, size_t *len)
{
    (void)data;
    int a = (argc > 1) ? atoi(argv[1]) : 0;
    int b = (argc > 2) ? atoi(argv[2]) : 0;
    int n = snprintf(out, cap, "add: %d + %d = %d", a, b, a + b);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/demo/arithmetic/add",
               "<a> <b>  Add two integers",
               0, demo_arithmetic_add_handler, NULL);

static int demo_arithmetic_mul_handler(void *data, int argc, char **argv,
                                        void *out, size_t cap, size_t *len)
{
    (void)data;
    int a = (argc > 1) ? atoi(argv[1]) : 0;
    int b = (argc > 2) ? atoi(argv[2]) : 0;
    int n = snprintf(out, cap, "mul: %d * %d = %d", a, b, a * b);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}
FLUXIPC_STATIC("/demo/arithmetic/mul",
               "<a> <b>  Multiply two integers",
               0, demo_arithmetic_mul_handler, NULL);

/* ==========================================================================
 *  Dynamic device context
 * ========================================================================== */

typedef struct {
    char name[64];
    int  value;
    int  min_val;
    int  max_val;
} device_ctx_t;

static int device_name_handler(void *data, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    device_ctx_t *ctx = data;
    if (argc > 1) snprintf(ctx->name, sizeof(ctx->name), "%s", argv[1]);
    int n = snprintf(out, cap, "name: %s", ctx->name);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}

static int device_value_handler(void *data, int argc, char **argv,
                                 void *out, size_t cap, size_t *len)
{
    device_ctx_t *ctx = data;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v < ctx->min_val || v > ctx->max_val) {
            int n = snprintf(out, cap,
                "error: value %d out of range [%d, %d]",
                v, ctx->min_val, ctx->max_val);
            *len = (size_t)(n > 0 ? n : 0);
            return -1;
        }
        ctx->value = v;
    }
    int n = snprintf(out, cap, "value: %d", ctx->value);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}

static int device_reset_handler(void *data, int argc, char **argv,
                                 void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    device_ctx_t *ctx = data;
    ctx->value = 0;
    int n = snprintf(out, cap, "device %s reset: value=%d",
                     ctx->name, ctx->value);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}

static int device_info_handler(void *data, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    device_ctx_t *ctx = data;
    int n = snprintf(out, cap,
        "name=%s value=%d range=[%d,%d]",
        ctx->name, ctx->value, ctx->min_val, ctx->max_val);
    *len = (size_t)(n > 0 ? n : 0);
    return 0;
}

static void register_device(const char *prefix,
                            const char *name,
                            int value, int min_val, int max_val)
{
    static device_ctx_t devs[8];
    static int dev_idx = 0;
    device_ctx_t *ctx = &devs[dev_idx++];

    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    ctx->value   = value;
    ctx->min_val = min_val;
    ctx->max_val = max_val;

    char path[FLUXIPC_PATH_MAX];
    snprintf(path, sizeof(path), "%s/name", prefix);
    fluxipc_register(path, "[<new-name>]  Get/set device name", 0,
                     device_name_handler, ctx);

    snprintf(path, sizeof(path), "%s/value", prefix);
    fluxipc_register(path, "[<new-value>]  Get/set device value (range-checked)", 0,
                     device_value_handler, ctx);

    snprintf(path, sizeof(path), "%s/reset", prefix);
    fluxipc_register(path, "Reset device value to 0", 0,
                     device_reset_handler, ctx);

    snprintf(path, sizeof(path), "%s/info", prefix);
    fluxipc_register(path, "Show full device info", 0,
                     device_info_handler, ctx);
}

/* ==========================================================================
 *  Signal handling
 * ========================================================================== */

static volatile int g_quit = 0;
static void sig_handler(int s) { (void)s; g_quit = 1; fluxipc_stop(); }

/* ==========================================================================
 *  Request observation hooks (demo)
 * ========================================================================== */

static void req_pre_hook(const fluxipc_request_t *req, void *user)
{
    (void)user;
    printf("[hook:pre ] path=%s argc=%d matched=%d\n",
           req->path, req->argc, req->matched);
}

static void req_post_hook(const fluxipc_request_t *req, void *user)
{
    (void)user;
    printf("[hook:post] path=%s status=%d out_len=%zu\n",
           req->path, req->status, req->out_len);
}

/* ==========================================================================
 *  main
 * ========================================================================== */

int main(int argc, char **argv)
{
    int rc = fluxipc_client_dispatch(argc, argv);
    if (rc >= 0) return rc;

    g_start_time = time(NULL);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    rc = fluxipc_server_init("example_server", 32100);
    if (rc < 0) {
        fprintf(stderr, "fluxipc_server_init: %d\n", rc);
        return 1;
    }

    printf("[server] started – socket: "
           "/run/example_server-fluxipc/example_server.sock\n");

    /* Observe every request before/after its handler runs */
    fluxipc_set_request_hooks(req_pre_hook, req_post_hook, NULL);

    /* Register dynamic per-device endpoints */
    register_device("/devices/stub/primary",   "stub-primary",   100,   0, 255);
    register_device("/devices/stub/secondary", "stub-secondary",  50, -50, 150);
    register_device("/devices/io/led",        "status-led",      1,   0,   1);
    register_device("/devices/io/relay",      "power-relay",     0,   0,   1);

    printf("[server] %d dynamic endpoints registered\n", 4 * 4 /* 4 devices × 4 endpoints */);
    printf("  /devices/stub/primary/{name,value,reset,info}\n"
           "  /devices/stub/secondary/{name,value,reset,info}\n"
           "  /devices/io/led/{name,value,reset,info}\n"
           "  /devices/io/relay/{name,value,reset,info}\n");

    fluxipc_usage("example_server");

    while (!g_quit) fluxipc_poll(NULL);

    printf("[server] shutting down (uptime %ld s)\n",
           (long)(time(NULL) - g_start_time));
    fluxipc_destroy();
    return 0;
}