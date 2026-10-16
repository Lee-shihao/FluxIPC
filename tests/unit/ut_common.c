#include "ut_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* ── server fixture ──────────────────────────────────────────────────────── */

static char g_prog[FLUXIPC_NAME_MAX];
static int  g_seq;

const char *ut_next_name(void)
{
    static char name[FLUXIPC_NAME_MAX];
    snprintf(name, sizeof(name), "ut%05d_%d", g_seq++, (int)getpid());
    return name;
}

const char *ut_server_up(void)
{
    return ut_server_up_port(0);
}

const char *ut_server_up_port(uint16_t port)
{
    snprintf(g_prog, sizeof(g_prog), "%s", ut_next_name());
    int rc = fluxipc_server_init(g_prog, port);
    assert_int_equal(rc, 0);
    return g_prog;
}

void ut_server_down(void)
{
    /* Only destroy if this fixture is still the active context. */
    if (g_ctx) fluxipc_destroy();
}

const char *ut_server_name(void)
{
    return g_prog;
}

const char *ut_server_sock(void)
{
    static char path[FLUXIPC_PATH_MAX];
    snprintf(path, sizeof(path),
             "/run/user/%d/%s-fluxipc/%s.sock", (int)getuid(), g_prog, g_prog);
    return path;
}

/* ── server event-loop pump ──────────────────────────────────────────────── */

static pthread_t     g_pump;
static volatile int  g_pump_run;
static int           g_pump_started;

static void *pump_fn(void *arg)
{
    (void)arg;
    while (g_pump_run) fluxipc_poll(NULL);
    return NULL;
}

void ut_server_pump_start(void)
{
    g_pump_run     = 1;
    g_pump_started = 1;
    assert_int_equal(pthread_create(&g_pump, NULL, pump_fn, NULL), 0);
}

void ut_server_pump_stop(void)
{
    if (!g_pump_started) return;
    g_pump_run = 0;
    pthread_join(g_pump, NULL);
    g_pump_started = 0;
}

/* ── fake connection ─────────────────────────────────────────────────────── */

void ut_conn_init(fluxipc_conn_t *c, const void *data, size_t len,
                  fluxipc_conn_kind_t kind)
{
    memset(c, 0, sizeof(*c));
    c->fd    = -1;
    c->kind  = kind;
    c->state = FLUXIPC_CONN_READING;
    if (len) {
        c->rbuf = malloc(len);
        assert_non_null(c->rbuf);
        memcpy(c->rbuf, data, len);
        c->rcap = len;
        c->rlen = len;
    }
}

void ut_conn_free(fluxipc_conn_t *c)
{
    free(c->rbuf);
    free(c->wbuf);
    c->rbuf = c->wbuf = NULL;
}

/* ── output capture ──────────────────────────────────────────────────────── */

static int  g_saved_out = -1, g_saved_err = -1, g_pipe_rd = -1;

void ut_capture_begin(void)
{
    fflush(stdout);
    fflush(stderr);

    int fds[2];
    assert_int_equal(pipe(fds), 0);
    g_pipe_rd   = fds[0];
    g_saved_out = dup(STDOUT_FILENO);
    g_saved_err = dup(STDERR_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
}

const char *ut_capture_end(void)
{
    static char buf[65536];
    fflush(stdout);
    fflush(stderr);
    dup2(g_saved_out, STDOUT_FILENO);
    dup2(g_saved_err, STDERR_FILENO);
    close(g_saved_out);
    close(g_saved_err);
    g_saved_out = g_saved_err = -1;

    size_t n = 0;
    ssize_t r;
    while (n + 1 < sizeof(buf) &&
           (r = read(g_pipe_rd, buf + n, sizeof(buf) - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = '\0';
    close(g_pipe_rd);
    g_pipe_rd = -1;
    return buf;
}

/* ── handler stubs ───────────────────────────────────────────────────────── */

int ut_ipc_echo(void *data, int argc, char **argv,
                void *out, size_t out_cap, size_t *out_len)
{
    (void)argc; (void)argv;
    const char *msg = data ? (const char *)data : "";
    size_t n = strlen(msg);
    if (n > out_cap) n = out_cap;
    if (out) memcpy(out, msg, n);
    if (out_len) *out_len = n;
    return 0;
}

int ut_http_ok(void *data, const fluxipc_http_request_t *req,
               fluxipc_http_response_t *resp)
{
    (void)req;
    resp->json = cJSON_CreateObject();
    if (resp->json)
        cJSON_AddStringToObject(resp->json, "handler",
                                data ? (const char *)data : "ok");
    return 0;
}
