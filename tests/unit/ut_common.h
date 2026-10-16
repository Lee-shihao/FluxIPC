#pragma once

/*
 * ut_common.h – shared helpers for the FluxIPC unit-test suite (cmocka).
 *
 * cmocka requires <stdarg.h>, <stddef.h> and <setjmp.h> to be included
 * *before* <cmocka.h>; this header centralises that ordering.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <cmocka.h>

#include "fluxipc.h"
#include "fluxipc_internal.h"

/* ── Server fixture ──────────────────────────────────────────────────────── */

/** Reserve a unique, valid prog name (does not start a server). */
const char *ut_next_name(void);
/** Start a FluxIPC server with a unique auto-generated prog name (no HTTP).
 *  Fails the current test on error.  Returns the prog name. */
const char *ut_server_up(void);
/** Like ut_server_up() but with an explicit HTTP port (0 disables HTTP). */
const char *ut_server_up_port(uint16_t port);
/** Stop/destroy the server started by ut_server_up(). Safe to call twice. */
void        ut_server_down(void);
/** Prog name of the current test server (as passed to server_init). */
const char *ut_server_name(void);
/** Absolute socket path of the current test server. */
const char *ut_server_sock(void);

/** Run the server event loop on a helper thread so blocking client helpers
 *  (fluxipc_call / sock_call_alloc) can talk to the in-process server. */
void ut_server_pump_start(void);
/** Stop the helper thread (join). Safe to call when not started. */
void ut_server_pump_stop(void);

/* ── Fake connection (white-box transport processors) ────────────────────── */

/** Prepare @p c as a READING connection whose request bytes are a copy of
 *  @p data (the processors mutate rbuf in place, so we own a writable copy). */
void ut_conn_init(fluxipc_conn_t *c, const void *data, size_t len,
                  fluxipc_conn_kind_t kind);
/** Release buffers owned by ut_conn_init()/conn_respond(). */
void ut_conn_free(fluxipc_conn_t *c);

/* ── Output capture ──────────────────────────────────────────────────────── */

/** Redirect stdout+stderr into an internal pipe (for print-style functions). */
void        ut_capture_begin(void);
/** Restore stdout+stderr and return the captured text (valid until next call). */
const char *ut_capture_end(void);

/* ── Handler stubs ───────────────────────────────────────────────────────── */

/** Simple IPC handler that copies @p msg (the method's data) into @p out. */
int ut_ipc_echo(void *data, int argc, char **argv,
                void *out, size_t out_cap, size_t *out_len);

/** HTTP handler that stores @c data into a JSON object {"handler":data}. */
int ut_http_ok(void *data, const fluxipc_http_request_t *req,
               fluxipc_http_response_t *resp);
