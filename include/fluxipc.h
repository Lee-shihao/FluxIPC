#pragma once

/*
 * FluxIPC – namespace-tree IPC for Linux.
 *
 * A FluxIPC server owns a namespace tree.  Every node is either a container
 * (namespace) or a leaf (callable IPC method).  A leaf carries a single
 * handler with the classic FluxIPC contract: positional argv in, raw bytes
 * out.
 *
 * Threading model
 * ---------------
 * The library never creates a thread of its own.  The host application keeps
 * full control over scheduling:
 *
 *   • Run fluxipc_run() in a thread you created (the simplest option).
 *   • Or drive the loop from your own scheduler with fluxipc_poll_timeout().
 *   • fluxipc_wakeup() / fluxipc_stop() may be called from any thread.
 *
 * Registration and removal are protected by the tree lock, so they may run
 * concurrently with the polling thread.  Exactly one thread may poll at a
 * time; a second concurrent poll returns -EBUSY.  fluxipc_destroy() must be
 * called after the polling thread has exited (join it first).
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Limits ──────────────────────────────────────────────────────────────── */
#define FLUXIPC_PATH_MAX    256
#define FLUXIPC_NAME_MAX    64
#define FLUXIPC_USAGE_MAX   256
#define FLUXIPC_MAX_ARGS    32

/* ─── Forward declarations ────────────────────────────────────────────────── */
typedef struct fluxipc_node fluxipc_node_t;

/* ─── IPC handler ─────────────────────────────────────────────────────────── */
/**
 * Invoked for Unix-socket / CLI calls.
 *
 * @param data     the method's data pointer
 * @param argc     argument count; argv[0] is the endpoint path
 * @param argv     argument vector (NUL-terminated strings)
 * @param out_buf  output buffer (may be NULL when @p out_cap is 0)
 * @param out_cap  capacity of @p out_buf
 * @param out_len  set to the number of bytes written
 * @return 0 on success, negative errno on error
 */
typedef int (*fluxipc_handler_fn)(void   *data,
                                  int     argc,
                                  char  **argv,
                                  void   *out_buf,
                                  size_t  out_cap,
                                  size_t *out_len);

/* ─── Method descriptor ───────────────────────────────────────────────────── */
/**
 * Metadata + implementation of a leaf.  A leaf must have a non-NULL @c ipc
 * handler.  @c summary is copied by the library and exposed through LIST.
 */
typedef struct {
    const char         *summary;
    fluxipc_handler_fn  ipc;
    void               *data;
} fluxipc_method_t;

/**
 * Convenience initialiser: FLUXIPC_METHOD(ipc_fn, summary).
 * Produces a compound literal, so it can be passed by address to
 * fluxipc_node_leaf() / fluxipc_register_method() or stored in a static table:
 *
 *     fluxipc_register_method("/greet/hello", &FLUXIPC_METHOD(
 *         hello_ipc, "Say hello"));
 */
#define FLUXIPC_METHOD(ipc_fn, sum) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .summary = (sum) }

/** Like FLUXIPC_METHOD() but also sets the per-leaf @c data pointer. */
#define FLUXIPC_METHOD_D(ipc_fn, data_ptr, sum) \
    (fluxipc_method_t){ .ipc = (ipc_fn), .data = (data_ptr), .summary = (sum) }

/* ─── Namespace tree ──────────────────────────────────────────────────────── */

/** Root node of the tree; valid after fluxipc_server_init(). */
fluxipc_node_t *fluxipc_tree_root(void);

/** Get or create a container child.  Returns a borrowed handle or NULL. */
fluxipc_node_t *fluxipc_node_mkdir(fluxipc_node_t *parent, const char *name);

/**
 * Register a leaf method under @p parent.  Low-level primitive; prefer
 * fluxipc_register_method() unless you already hold the parent container.
 * Returns a borrowed handle, or NULL (errno set; EEXIST if already present).
 */
fluxipc_node_t *fluxipc_node_leaf(fluxipc_node_t *parent,
                                  const char     *name,
                                  const fluxipc_method_t *method);

/** Look up a direct child.  Returns a borrowed handle or NULL. */
fluxipc_node_t *fluxipc_node_find(fluxipc_node_t *parent, const char *name);

/* ─── High-level registration ─────────────────────────────────────────────── */

/**
 * Register a leaf from a full method descriptor (path-based).  Creates every
 * missing intermediate container on the way:
 *
 *     fluxipc_register_method("/module/sensor/status", &FLUXIPC_METHOD(
 *         status_ipc, "Read the sensor"));
 *
 * The method is copied exactly like fluxipc_node_leaf(), so passing a stack
 * temporary or a compound literal is safe.  The call is atomic: on failure no
 * half-created containers are left behind.
 *
 * Registering the same path again merges the two calls: only still-empty
 * metadata is filled, so an existing handler is never overwritten.
 *
 * @param path    absolute path, e.g. "/a/b/c" (leading '/' required).
 * @param method  metadata + a non-NULL IPC handler.
 * @return borrowed leaf handle, or NULL with errno set
 *         (EINVAL bad path/method, EEXIST name is a container, ENOMEM).
 */
fluxipc_node_t *fluxipc_register_method(const char *path,
                                        const fluxipc_method_t *method);

/**
 * Classic main-branch registration: register an IPC endpoint at @p path.
 *
 *     fluxipc_register("/devices/foo/name", "[<new-name>]", 0, handler, ctx);
 *
 * @p usage becomes the leaf summary.  @p slot_sz is accepted for source
 * compatibility only: the shared-memory slots of the old architecture are gone
 * and the value is ignored.
 *
 * @return borrowed leaf handle, or NULL with errno set.
 */
fluxipc_node_t *fluxipc_register(const char *path, const char *usage,
                                 size_t slot_sz,
                                 fluxipc_handler_fn handler, void *data);

/** One row of a static registration table (see fluxipc_register_all()). */
typedef struct {
    const char       *path;    /* absolute namespace path, e.g. "/a/b" */
    fluxipc_method_t  method;
} fluxipc_endpoint_t;

/**
 * Register an array of endpoints in one call.  Every row is registered with
 * fluxipc_register_method(); the first failure stops the loop.
 * @return 0 on success, or the negative errno of the offending entry.
 */
int fluxipc_register_all(const fluxipc_endpoint_t *endpoints, size_t count);

/** Main-branch alias for fluxipc_node_remove(). */
void fluxipc_unregister(fluxipc_node_t *node);

/* ─── Compile-time registration (main-branch FLUXIPC_STATIC) ──────────────── */

/**
 * One compile-time endpoint.  Created by FLUXIPC_STATIC() and queued by a
 * constructor at load time; fluxipc_server_init() then registers every queued
 * entry.  The opaque @c next link is owned by the registry — do not touch it.
 */
typedef struct fluxipc_entry {
    const char           *path;
    const char           *usage;
    fluxipc_handler_fn    handler;
    void                 *data;
    struct fluxipc_entry *next;
} fluxipc_entry_t;

/** Internal: called by the FLUXIPC_STATIC() constructor.  Not for direct use. */
void fluxipc_static_add(fluxipc_entry_t *entry);

#define FLUXIPC_PP_CAT_(a, b) a##b
#define FLUXIPC_PP_CAT(a, b)  FLUXIPC_PP_CAT_(a, b)

/**
 * FLUXIPC_STATIC – declare a compile-time IPC endpoint (main-branch API).
 *
 * The entry is queued before @c main() and registered by
 * fluxipc_server_init(), so no @c -rdynamic / linker section is required.
 * @p usage becomes the leaf summary; @p slot_sz is accepted and ignored
 * for source compatibility.
 *
 *     static int hello(void *d, int argc, char **argv,
 *                      void *out, size_t cap, size_t *len) { ... }
 *     FLUXIPC_STATIC("/greet/hello", "say hello", 0, hello, NULL);
 */
#define FLUXIPC_STATIC(ipc_path, usage_str, slot_sz, handler_fn, ctx)        \
    static fluxipc_entry_t FLUXIPC_PP_CAT(_fluxipc_static_, __LINE__) = {    \
        .path         = (ipc_path),                                          \
        .usage        = (usage_str),                                         \
        .handler      = (handler_fn),                                        \
        .data         = (void *)(ctx),                                       \
    };                                                                       \
    __attribute__((constructor, used))                                       \
    static void FLUXIPC_PP_CAT(_fluxipc_static_ctor_, __LINE__)(void)        \
    {                                                                        \
        fluxipc_static_add(&FLUXIPC_PP_CAT(_fluxipc_static_, __LINE__));     \
    }

/** Remove a leaf, or a whole container subtree.  Prunes empty ancestors. */
void fluxipc_node_remove(fluxipc_node_t *node);

/** Callback for fluxipc_node_leaves(). Non-zero stops; runs under tree lock. */
typedef int (*fluxipc_leaf_cb)(const fluxipc_node_t *leaf, void *user);

/** Enumerate every leaf at or below @p node. @return count, or negative errno. */
int fluxipc_node_leaves(fluxipc_node_t *node,
                        fluxipc_leaf_cb cb, void *user);

/* ─── Node accessors (no locking) ─────────────────────────────────────────── */
const char             *fluxipc_node_path(const fluxipc_node_t *node);
const char             *fluxipc_node_name(const fluxipc_node_t *node);
const fluxipc_method_t *fluxipc_node_method(const fluxipc_node_t *node);
int                     fluxipc_node_is_leaf(const fluxipc_node_t *node);

/* ─── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * Initialise the process-wide FluxIPC server.
 * @param prog_name  Unique short server name (socket / run-dir).
 * @return 0 on success, negative errno (-EALREADY if already initialised).
 *
 * Call this once, before starting any polling thread.
 */
int  fluxipc_server_init(const char *prog_name);

/* ─── Event loop (owned by the host application) ──────────────────────────── */

/**
 * Blocking convenience wrapper for fluxipc_poll_timeout(-1).
 * @return 1 if activity was handled, 0 on timeout/wakeup, negative errno.
 */
int  fluxipc_poll(void);

/**
 * Wait for and service socket activity.
 *
 * @param timeout_ms  < 0: wait indefinitely (until activity / wakeup / idle
 *                    deadline); 0: poll without blocking; > 0: upper bound.
 * @return 1 if activity was handled, 0 on timeout or EINTR, -EINVAL if the
 *         server is not running, -EBUSY if another thread is already polling,
 *         or another negative errno on a fatal select() error.
 *
 * May be called from an application-created thread.  Only one thread may be
 * inside this function at a time.
 */
int  fluxipc_poll_timeout(int timeout_ms);

/**
 * Run the event loop until fluxipc_stop() is called.
 *
 * Intended to be used directly as a thread entry point:
 *
 *     static void *serve(void *arg) { fluxipc_run(); return NULL; }
 *     pthread_create(&th, NULL, serve, NULL);
 *
 * @return 0 after a normal stop, negative errno on a fatal error or when the
 *         server has not been initialised.
 */
int  fluxipc_run(void);

/** Wake a blocked event loop from any thread (does not stop it). */
void fluxipc_wakeup(void);

/** Request event-loop exit and wake it.  Safe to call from any thread. */
void fluxipc_stop(void);

/** True while the event loop is expected to keep running. */
int  fluxipc_is_running(void);

/* ─── Shutdown ────────────────────────────────────────────────────────────── */
/**
 * Tear down the server and free all resources.  The polling thread must have
 * exited (join it) before this call; fluxipc_destroy() is not re-entrant with
 * fluxipc_poll*().
 */
void fluxipc_destroy(void);

/* ─── Help ───────────────────────────────────────────────────────────────── */
void fluxipc_usage(const char *prog);

/* ─── Request observation hooks ───────────────────────────────────────────── */
/** Read-only view of one request passed to the pre/post hooks. */
typedef struct {
    const char  *path;      /* namespace path                              */
    int          matched;   /* 1 if a handler was found                    */
    int          status;    /* handler return code — valid in POST only    */
    const void  *raw;       /* response bytes — valid in POST hook only    */
    size_t       raw_len;
} fluxipc_event_t;

typedef void (*fluxipc_request_hook_fn)(const fluxipc_event_t *ev, void *user);

/**
 * Register global pre/post request hooks.  Either may be NULL.  Must be called
 * after fluxipc_server_init().  Calling again replaces the previous hooks.
 * Setting hooks is safe from any thread; requests in flight keep the pair that
 * was current when they started.
 */
void fluxipc_set_request_hooks(fluxipc_request_hook_fn pre,
                               fluxipc_request_hook_fn post,
                               void                   *user);

/* ─── Client API ──────────────────────────────────────────────────────────── */

/** Callback for fluxipc_list_endpoints: receives one active endpoint. */
typedef void (*fluxipc_list_cb)(const char *path, uint32_t id,
                                const char *usage, void *userdata);

/**
 * Enumerate active IPC endpoints for a running server by querying its
 * Unix socket for the leaf manifest.  The manifest is a compact text protocol
 * (`<id>\t<path>\t<summary>\n`) served directly over the Unix socket.
 * @return 0 on success, negative errno if the server cannot be reached.
 */
int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata);

/**
 * Low-level single IPC call to a Unix socket.  Thread-safe: every call opens
 * its own connection.
 * @return status from the handler (0 on success, negative errno on error).
 */
int fluxipc_call(const char *sock_path, const char *ipc_path,
                 int argc, char **argv,
                 void *out_buf, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
