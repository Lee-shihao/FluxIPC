/**
 * example_server.c – FluxIPC demo server
 *
 * Every leaf implements BOTH transports:
 *   • an IPC handler  (positional argv → formatted text buffer)
 *   • an HTTP handler (parsed request → cJSON response)
 *
 * Try:
 *   HTTP   curl -s http://localhost:32100/system/info
 *   HTTP   curl -s -X PUT -H 'Content-Type: application/json' \
 *                 -d '{"rate_hz":20}' http://localhost:32100/module/sensor/config
 *   IPC    fluxipc-cli example_server -c /system/info
 *   IPC    fluxipc-cli example_server -c /demo/arithmetic/add 7 3
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

/* ==========================================================================
 *  Handler helpers
 * ========================================================================== */

/* IPC: formatted text into the caller buffer (the original contract). */
static int ipc_out(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0) { *len = 0; return -EINVAL; }
    *len = (size_t)n < cap ? (size_t)n : cap - 1;
    return 0;
}

/* HTTP: read a named field from the JSON body, else a query parameter. */
static const char *q_str(const fluxipc_http_request_t *r, const char *key,
                         const char *def)
{
    if (r->json) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->json, key);
        if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    }
    for (size_t i = 0; i < r->query_count; i++)
        if (strcmp(r->query[i].key, key) == 0) return r->query[i].value;
    return def;
}

static long q_int(const fluxipc_http_request_t *r, const char *key, long def)
{
    if (r->json) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->json, key);
        if (cJSON_IsNumber(v)) return (long)v->valuedouble;
        if (cJSON_IsString(v) && v->valuestring) return strtol(v->valuestring, NULL, 10);
        if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    }
    for (size_t i = 0; i < r->query_count; i++)
        if (strcmp(r->query[i].key, key) == 0)
            return strtol(r->query[i].value, NULL, 10);
    return def;
}

static int http_error(fluxipc_http_response_t *resp, int status, const char *msg)
{
    resp->status = status;
    cJSON *o = cJSON_CreateObject();
    if (o) cJSON_AddStringToObject(o, "error", msg);
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  system
 * ========================================================================== */

static time_t g_start_time;

static int system_info_ipc(void *d, int argc, char **argv,
                          void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len, "program=example_server pid=%d uid=%d",
                   getpid(), getuid());
}
static int system_info_http(void *d, const fluxipc_http_request_t *r,
                            fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "program", "example_server");
    cJSON_AddNumberToObject(o, "pid", getpid());
    cJSON_AddNumberToObject(o, "uid", getuid());
    resp->json = o;
    return 0;
}

static int system_uptime_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    long up = (long)(time(NULL) - g_start_time);
    return ipc_out(out, cap, len, "uptime_seconds=%ld uptime_minutes=%ld",
                   up, up / 60);
}
static int system_uptime_http(void *d, const fluxipc_http_request_t *r,
                              fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    long up = (long)(time(NULL) - g_start_time);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "uptime_seconds", up);
    cJSON_AddNumberToObject(o, "uptime_minutes", up / 60);
    resp->json = o;
    return 0;
}

static int system_version_ipc(void *d, int argc, char **argv,
                              void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len, "version=2.1.0 build_date=%s", __DATE__);
}
static int system_version_http(void *d, const fluxipc_http_request_t *r,
                               fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "version", "2.1.0");
    cJSON_AddStringToObject(o, "build_date", __DATE__);
    resp->json = o;
    return 0;
}

static int system_shutdown_ipc(void *d, int argc, char **argv,
                               void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    fluxipc_stop();
    return ipc_out(out, cap, len, "shutting down... bye!");
}
static int system_shutdown_http(void *d, const fluxipc_http_request_t *r,
                                fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    fluxipc_stop();
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "message", "shutting down");
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  module/sensor
 * ========================================================================== */

typedef struct {
    double temperature;
    double humidity;
    int    sample_rate_hz;
} sensor_state_t;

static sensor_state_t g_sensor = { 23.5, 58.2, 10 };

static int sensor_status_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    return ipc_out(out, cap, len,
                   "temperature=%.1f humidity=%.1f sample_rate=%d ok=true",
                   g_sensor.temperature, g_sensor.humidity,
                   g_sensor.sample_rate_hz);
}
static int sensor_status_http(void *d, const fluxipc_http_request_t *r,
                              fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "temperature", g_sensor.temperature);
    cJSON_AddNumberToObject(o, "humidity", g_sensor.humidity);
    cJSON_AddNumberToObject(o, "sample_rate", g_sensor.sample_rate_hz);
    cJSON_AddBoolToObject(o, "ok", 1);
    resp->json = o;
    return 0;
}

static int sensor_calibrate_ipc(void *d, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)d;
    const char *offset = (argc > 1) ? argv[1] : "0";
    return ipc_out(out, cap, len, "calibration applied: offset=%s result=ok",
                   offset);
}
static int sensor_calibrate_http(void *d, const fluxipc_http_request_t *r,
                                 fluxipc_http_response_t *resp)
{
    (void)d;
    const char *offset = q_str(r, "offset", "0");
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "offset", offset);
    cJSON_AddStringToObject(o, "result", "ok");
    resp->json = o;
    return 0;
}

static int sensor_config_ipc(void *d, int argc, char **argv,
                             void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) g_sensor.sample_rate_hz = atoi(argv[1]);
    return ipc_out(out, cap, len, "sample_rate=%d", g_sensor.sample_rate_hz);
}
static int sensor_config_http(void *d, const fluxipc_http_request_t *r,
                              fluxipc_http_response_t *resp)
{
    (void)d;
    if (strcmp(r->method, "PUT") == 0 || strcmp(r->method, "POST") == 0) {
        long rate = q_int(r, "rate_hz", -1);
        if (rate > 0) g_sensor.sample_rate_hz = (int)rate;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "sample_rate", g_sensor.sample_rate_hz);
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  module/motor
 * ========================================================================== */

typedef struct { int speed_rpm; int position_deg; int enabled; } motor_state_t;
static motor_state_t g_motor = { 1200, 90, 1 };

static int motor_speed_ipc(void *d, int argc, char **argv,
                           void *out, size_t cap, size_t *len)
{
    (void)d;
    if (!g_motor.enabled) {
        ipc_out(out, cap, len, "error: motor is disabled");
        return -EPERM;
    }
    if (argc > 1) g_motor.speed_rpm = atoi(argv[1]);
    return ipc_out(out, cap, len, "speed=%d rpm", g_motor.speed_rpm);
}
static int motor_speed_http(void *d, const fluxipc_http_request_t *r,
                            fluxipc_http_response_t *resp)
{
    (void)d;
    if (!g_motor.enabled) return http_error(resp, 409, "motor is disabled");
    if (strcmp(r->method, "PUT") == 0 || strcmp(r->method, "POST") == 0) {
        long rpm = q_int(r, "rpm", -1);
        if (rpm >= 0) g_motor.speed_rpm = (int)rpm;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "speed", g_motor.speed_rpm);
    resp->json = o;
    return 0;
}

static int motor_position_ipc(void *d, int argc, char **argv,
                              void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    if (!g_motor.enabled) {
        ipc_out(out, cap, len, "error: motor is disabled");
        return -EPERM;
    }
    return ipc_out(out, cap, len, "position=%d deg", g_motor.position_deg);
}
static int motor_position_http(void *d, const fluxipc_http_request_t *r,
                               fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    if (!g_motor.enabled) return http_error(resp, 409, "motor is disabled");
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "position", g_motor.position_deg);
    resp->json = o;
    return 0;
}

static int motor_stop_ipc(void *d, int argc, char **argv,
                          void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    g_motor.enabled = 0;
    g_motor.speed_rpm = 0;
    return ipc_out(out, cap, len, "motor stopped: enabled=%d speed=%d",
                   g_motor.enabled, g_motor.speed_rpm);
}
static int motor_stop_http(void *d, const fluxipc_http_request_t *r,
                           fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    g_motor.enabled = 0;
    g_motor.speed_rpm = 0;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddBoolToObject(o, "enabled", g_motor.enabled);
    cJSON_AddNumberToObject(o, "speed", g_motor.speed_rpm);
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  config
 * ========================================================================== */

static int g_log_level = 2;
static int g_max_clients = 16;

static int config_log_level_ipc(void *d, int argc, char **argv,
                                void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) {
        int lvl = atoi(argv[1]);
        if (lvl < 0 || lvl > 5) {
            ipc_out(out, cap, len, "error: log_level must be 0-5");
            return -EINVAL;
        }
        g_log_level = lvl;
    }
    return ipc_out(out, cap, len, "log_level=%d", g_log_level);
}
static int config_log_level_http(void *d, const fluxipc_http_request_t *r,
                                 fluxipc_http_response_t *resp)
{
    (void)d;
    if (strcmp(r->method, "PUT") == 0 || strcmp(r->method, "POST") == 0) {
        long lvl = q_int(r, "level", g_log_level);
        if (lvl < 0 || lvl > 5)
            return http_error(resp, 422, "level must be 0-5");
        g_log_level = (int)lvl;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "log_level", g_log_level);
    resp->json = o;
    return 0;
}

static int config_max_clients_ipc(void *d, int argc, char **argv,
                                  void *out, size_t cap, size_t *len)
{
    (void)d;
    if (argc > 1) g_max_clients = atoi(argv[1]);
    return ipc_out(out, cap, len, "max_clients=%d", g_max_clients);
}
static int config_max_clients_http(void *d, const fluxipc_http_request_t *r,
                                   fluxipc_http_response_t *resp)
{
    (void)d;
    if (strcmp(r->method, "PUT") == 0 || strcmp(r->method, "POST") == 0) {
        long n = q_int(r, "max_clients", -1);
        if (n > 0) g_max_clients = (int)n;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "max_clients", g_max_clients);
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  demo
 * ========================================================================== */

static int g_counter = 0;

static int demo_echo_ipc(void *d, int argc, char **argv,
                         void *out, size_t cap, size_t *len)
{
    (void)d;
    int off = snprintf(out, cap, "echo:");
    for (int i = 1; i < argc && (size_t)off < cap - 1; i++)
        off += snprintf(out + off, cap - (size_t)off, " %s", argv[i]);
    *len = (size_t)(off > 0 ? off : 0);
    return 0;
}
static int demo_echo_http(void *d, const fluxipc_http_request_t *r,
                          fluxipc_http_response_t *resp)
{
    (void)d;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -ENOMEM;
    const cJSON *jargs = r->json
        ? cJSON_GetObjectItemCaseSensitive(r->json, "args") : NULL;
    if (cJSON_IsArray(jargs)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, jargs)
            cJSON_AddItemToArray(arr, cJSON_CreateString(
                cJSON_IsString(it) ? it->valuestring : ""));
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) { cJSON_Delete(arr); return -ENOMEM; }
    cJSON_AddItemToObject(o, "echo", arr);
    resp->json = o;
    return 0;
}

static int demo_counter_ipc(void *d, int argc, char **argv,
                            void *out, size_t cap, size_t *len)
{
    (void)d; (void)argc; (void)argv;
    int val = __sync_fetch_and_add(&g_counter, 1);
    return ipc_out(out, cap, len, "counter=%d", val);
}
static int demo_counter_http(void *d, const fluxipc_http_request_t *r,
                             fluxipc_http_response_t *resp)
{
    (void)d; (void)r;
    int val = __sync_fetch_and_add(&g_counter, 1);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "counter", val);
    resp->json = o;
    return 0;
}

static int demo_add_ipc(void *d, int argc, char **argv,
                        void *out, size_t cap, size_t *len)
{
    (void)d;
    int a = (argc > 1) ? atoi(argv[1]) : 0;
    int b = (argc > 2) ? atoi(argv[2]) : 0;
    return ipc_out(out, cap, len, "add: %d + %d = %d", a, b, a + b);
}
static int demo_add_http(void *d, const fluxipc_http_request_t *r,
                         fluxipc_http_response_t *resp)
{
    (void)d;
    long a = q_int(r, "a", 0);
    long b = q_int(r, "b", 0);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "a", a);
    cJSON_AddNumberToObject(o, "b", b);
    cJSON_AddNumberToObject(o, "result", a + b);
    resp->json = o;
    return 0;
}

static int demo_mul_ipc(void *d, int argc, char **argv,
                        void *out, size_t cap, size_t *len)
{
    (void)d;
    int a = (argc > 1) ? atoi(argv[1]) : 0;
    int b = (argc > 2) ? atoi(argv[2]) : 0;
    return ipc_out(out, cap, len, "mul: %d * %d = %d", a, b, a * b);
}
static int demo_mul_http(void *d, const fluxipc_http_request_t *r,
                         fluxipc_http_response_t *resp)
{
    (void)d;
    long a = q_int(r, "a", 0);
    long b = q_int(r, "b", 0);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "a", a);
    cJSON_AddNumberToObject(o, "b", b);
    cJSON_AddNumberToObject(o, "result", a * b);
    resp->json = o;
    return 0;
}

/* ==========================================================================
 *  dynamic devices
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
    if (argc > 1) snprintf(ctx->name, sizeof(ctx->name), "%s", argv[1]);
    return ipc_out(out, cap, len, "name: %s", ctx->name);
}
static int device_name_http(void *data, const fluxipc_http_request_t *r,
                            fluxipc_http_response_t *resp)
{
    device_ctx_t *ctx = data;
    const char *nv = q_str(r, "name", NULL);
    if (nv) snprintf(ctx->name, sizeof(ctx->name), "%s", nv);
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "name", ctx->name);
    resp->json = o;
    return 0;
}

static int device_value_ipc(void *data, int argc, char **argv,
                            void *out, size_t cap, size_t *len)
{
    device_ctx_t *ctx = data;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v < ctx->min_val || v > ctx->max_val) {
            ipc_out(out, cap, len, "error: value %d out of range [%d, %d]",
                    v, ctx->min_val, ctx->max_val);
            return -ERANGE;
        }
        ctx->value = v;
    }
    return ipc_out(out, cap, len, "value: %d", ctx->value);
}
static int device_value_http(void *data, const fluxipc_http_request_t *r,
                             fluxipc_http_response_t *resp)
{
    device_ctx_t *ctx = data;
    if (strcmp(r->method, "PUT") == 0 || strcmp(r->method, "POST") == 0) {
        long v = q_int(r, "value", ctx->value);
        if (v < ctx->min_val || v > ctx->max_val) {
            char msg[96];
            snprintf(msg, sizeof(msg), "value out of range [%d, %d]",
                     ctx->min_val, ctx->max_val);
            return http_error(resp, 422, msg);
        }
        ctx->value = (int)v;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddNumberToObject(o, "value", ctx->value);
    resp->json = o;
    return 0;
}

static int device_reset_ipc(void *data, int argc, char **argv,
                            void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    device_ctx_t *ctx = data;
    ctx->value = 0;
    return ipc_out(out, cap, len, "device %s reset: value=%d",
                   ctx->name, ctx->value);
}
static int device_reset_http(void *data, const fluxipc_http_request_t *r,
                             fluxipc_http_response_t *resp)
{
    (void)r;
    device_ctx_t *ctx = data;
    ctx->value = 0;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "name", ctx->name);
    cJSON_AddNumberToObject(o, "value", ctx->value);
    resp->json = o;
    return 0;
}

static int device_info_ipc(void *data, int argc, char **argv,
                           void *out, size_t cap, size_t *len)
{
    (void)argc; (void)argv;
    device_ctx_t *ctx = data;
    return ipc_out(out, cap, len, "name=%s value=%d range=[%d,%d]",
                   ctx->name, ctx->value, ctx->min_val, ctx->max_val);
}
static int device_info_http(void *data, const fluxipc_http_request_t *r,
                            fluxipc_http_response_t *resp)
{
    (void)r;
    device_ctx_t *ctx = data;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -ENOMEM;
    cJSON_AddStringToObject(o, "name", ctx->name);
    cJSON_AddNumberToObject(o, "value", ctx->value);
    cJSON *range = cJSON_AddArrayToObject(o, "range");
    if (range) {
        cJSON_AddItemToArray(range, cJSON_CreateNumber(ctx->min_val));
        cJSON_AddItemToArray(range, cJSON_CreateNumber(ctx->max_val));
    }
    resp->json = o;
    return 0;
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
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(
        device_name_ipc, device_name_http, ctx,
        "Get or set the device name", NULL,
        FLUXIPC_M_GET | FLUXIPC_M_PUT));

    snprintf(path, sizeof(path), "%s/value", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(
        device_value_ipc, device_value_http, ctx,
        "Get or set the device value (range checked)",
        "{\"type\":\"object\",\"properties\":{"
        "\"value\":{\"type\":\"integer\"}}}",
        FLUXIPC_M_GET | FLUXIPC_M_PUT));

    snprintf(path, sizeof(path), "%s/reset", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(
        device_reset_ipc, device_reset_http, ctx,
        "Reset the device value to 0", NULL, FLUXIPC_M_POST));

    snprintf(path, sizeof(path), "%s/info", base);
    fluxipc_register_method(path, &FLUXIPC_METHOD_D(
        device_info_ipc, device_info_http, ctx,
        "Show full device info", NULL, FLUXIPC_M_GET));
}

/* ==========================================================================
 *  Request observation hooks
 * ========================================================================== */

static void req_pre_hook(const fluxipc_event_t *ev, void *user)
{
    (void)user;
    printf("[hook:pre ] %-4s %-5s %s matched=%d\n",
           ev->transport, ev->method, ev->path, ev->matched);
}

static void req_post_hook(const fluxipc_event_t *ev, void *user)
{
    (void)user;
    printf("[hook:post] %-4s %-5s %s status=%d\n",
           ev->transport, ev->method, ev->path, ev->status);
}

/* ==========================================================================
 *  Registration
 * ========================================================================== */

static void register_endpoints(void)
{
    /* One row per endpoint: intermediate containers are created for us. */
    static const fluxipc_endpoint_t endpoints[] = {
        { "/system/info",     FLUXIPC_METHOD(system_info_ipc,     system_info_http,     "Show server process info",      NULL, FLUXIPC_M_GET) },
        { "/system/uptime",   FLUXIPC_METHOD(system_uptime_ipc,   system_uptime_http,   "Show server uptime",            NULL, FLUXIPC_M_GET) },
        { "/system/version",  FLUXIPC_METHOD(system_version_ipc,  system_version_http,  "Show server version and build date", NULL, FLUXIPC_M_GET) },
        { "/system/shutdown", FLUXIPC_METHOD(system_shutdown_ipc, system_shutdown_http, "Gracefully stop the server",    NULL, FLUXIPC_M_POST) },

        { "/module/sensor/status",    FLUXIPC_METHOD(sensor_status_ipc,    sensor_status_http,    "Query current sensor readings", NULL, FLUXIPC_M_GET) },
        { "/module/sensor/calibrate", FLUXIPC_METHOD(sensor_calibrate_ipc, sensor_calibrate_http, "Apply temperature calibration offset",
                                                     "{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"number\"}}}", FLUXIPC_M_POST) },
        { "/module/sensor/config",    FLUXIPC_METHOD(sensor_config_ipc,    sensor_config_http,    "Get or set the sensor sample rate",
                                                     "{\"type\":\"object\",\"properties\":{\"rate_hz\":{\"type\":\"integer\"}}}",
                                                     FLUXIPC_M_GET | FLUXIPC_M_PUT) },
        { "/module/motor/speed",    FLUXIPC_METHOD(motor_speed_ipc,    motor_speed_http,    "Get or set motor speed",
                                                    "{\"type\":\"object\",\"properties\":{\"rpm\":{\"type\":\"integer\"}}}",
                                                    FLUXIPC_M_GET | FLUXIPC_M_PUT) },
        { "/module/motor/position", FLUXIPC_METHOD(motor_position_ipc, motor_position_http, "Query current motor position", NULL, FLUXIPC_M_GET) },
        { "/module/motor/stop",     FLUXIPC_METHOD(motor_stop_ipc,     motor_stop_http,     "Emergency stop motor",        NULL, FLUXIPC_M_POST) },

        { "/config/log_level",   FLUXIPC_METHOD(config_log_level_ipc,   config_log_level_http,   "Get or set server log level (0-5)", NULL,
                                                 FLUXIPC_M_GET | FLUXIPC_M_PUT) },
        { "/config/max_clients", FLUXIPC_METHOD(config_max_clients_ipc, config_max_clients_http, "Get or set max concurrent clients", NULL,
                                                 FLUXIPC_M_GET | FLUXIPC_M_PUT) },

        { "/demo/echo",    FLUXIPC_METHOD(demo_echo_ipc,    demo_echo_http,    "Echo back all arguments",
                                           "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}",
                                           FLUXIPC_M_POST) },
        { "/demo/counter", FLUXIPC_METHOD(demo_counter_ipc, demo_counter_http, "Atomically increment and return counter", NULL, FLUXIPC_M_GET) },
        { "/demo/arithmetic/add", FLUXIPC_METHOD(demo_add_ipc, demo_add_http, "Add two integers",
                                                  "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}}}",
                                                  FLUXIPC_M_GET | FLUXIPC_M_POST) },
        { "/demo/arithmetic/mul", FLUXIPC_METHOD(demo_mul_ipc, demo_mul_http, "Multiply two integers",
                                                  "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}}}",
                                                  FLUXIPC_M_GET | FLUXIPC_M_POST) },
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
    int n = snprintf((char *)out, cap, "compiled endpoint: %s",
                     (const char *)data);
    if (len) *len = (n > 0) ? (size_t)n : 0;
    return 0;
}
FLUXIPC_STATIC("/system/compiled", "Compile-time registered endpoint", 0,
               compiled_info_ipc, "FLUXIPC_STATIC");

/* ==========================================================================
 *  main
 * ========================================================================== */

static volatile int g_quit = 0;
static void sig_handler(int s) { (void)s; g_quit = 1; fluxipc_stop(); }

int main(void)
{
    g_start_time = time(NULL);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int rc = fluxipc_server_init("example_server", 32100);
    if (rc < 0) {
        fprintf(stderr, "fluxipc_server_init: %d\n", rc);
        return 1;
    }

    fluxipc_set_request_hooks(req_pre_hook, req_post_hook, NULL);

    register_endpoints();
    fluxipc_usage("example_server");

    printf("Examples:\n"
           "  HTTP  curl -s http://localhost:32100/system/info\n"
           "  HTTP  curl -s http://localhost:32100/module      # leaf manifest\n"
           "  IPC   fluxipc-cli example_server -c /system/info\n"
           "  IPC   fluxipc-cli example_server -c /demo/arithmetic/add 7 3\n\n");

    while (!g_quit) {
        if (fluxipc_poll(NULL) < 0) break;
    }

    printf("[server] shutting down (uptime %ld s)\n",
           (long)(time(NULL) - g_start_time));
    fluxipc_destroy();
    return 0;
}
