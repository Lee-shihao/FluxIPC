#pragma once

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

/* ─── Handler callback ────────────────────────────────────────────────────── */
/**
 * Handler function signature for IPC endpoints.
 *
 * @param data     Opaque context pointer registered with the endpoint
 * @param argc     Argument count (argv[0] == last path component)
 * @param argv     Argument vector
 * @param out_buf  Buffer to write response into
 * @param out_cap  Capacity of out_buf
 * @param out_len  Set by handler: actual bytes written
 * @return 0 on success, negative errno on error
 */
typedef int (*fluxipc_handler_fn)(void       *data,
                                  int         argc,
                                  char      **argv,
                                  void       *out_buf,
                                  size_t      out_cap,
                                  size_t     *out_len);

/* ─── Static registration entry (linker section) ─────────────────────────── */
typedef struct {
    const char         *path;
    const char         *usage;
    size_t              slot_data_sz;
    fluxipc_handler_fn  handler;
    void               *data;
} fluxipc_entry_t;

/* Weak: provided by the linker only when FLUXIPC_STATIC() entries exist in the
 * final binary.  The shared library and standalone CLI link cleanly without them. */
extern fluxipc_entry_t __start_fluxipc_registry[] __attribute__((weak));
extern fluxipc_entry_t __stop_fluxipc_registry[]  __attribute__((weak));

#define _FLUXIPC_SECTION \
    __attribute__((section("fluxipc_registry"), used, aligned(sizeof(void *))))

/**
 * FLUXIPC_STATIC – declare a compile-time IPC endpoint.
 *
 * The entry is placed in the "fluxipc_registry" ELF section and iterated
 * automatically during fluxipc_server_init().
 *
 * @param ipc_path   Path string, e.g. "/devices/stub/xxx/name"
 * @param usage_str  Human-readable usage hint shown in CLI help
 * @param slot_sz    Per-slot shared-memory data size (may be 0)
 * @param handler_fn fluxipc_handler_fn callback
 * @param ctx        Opaque context pointer passed to handler
 */
#define FLUXIPC_STATIC(ipc_path, usage_str, slot_sz, handler_fn, ctx)  \
    static fluxipc_entry_t                                              \
        _fse_##handler_fn _FLUXIPC_SECTION = {                         \
        .path         = (ipc_path),                                    \
        .usage        = (usage_str),                                   \
        .slot_data_sz = (size_t)(slot_sz),                             \
        .handler      = (handler_fn),                                  \
        .data         = (void *)(ctx),                                 \
    }

/* ─── Opaque node handle ──────────────────────────────────────────────────── */
typedef struct fluxipc_node fluxipc_node_t;

/* ─── Dynamic registration ────────────────────────────────────────────────── */
/**
 * Register an IPC endpoint at runtime.
 * Creates the endpoint in the namespace tree, updates shared memory, and
 * creates a symlink under /run/<prog>-fluxipc/<path>.
 *
 * @return Opaque node handle, or NULL on failure.
 */
fluxipc_node_t *fluxipc_register(const char         *path,
                                 const char         *usage,
                                 size_t              slot_sz,
                                 fluxipc_handler_fn  handler,
                                 void               *data);

/**
 * Unregister a previously registered IPC endpoint and free resources.
 */
void fluxipc_unregister(fluxipc_node_t *node);

/* ─── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * Initialise a FluxIPC server.
 *
 * Creates the Unix socket, POSIX shared-memory registry, symlink tree,
 * and registers all compile-time FLUXIPC_STATIC entries.
 *
 * @param prog_name  Unique short name for this server (used in socket/shm paths).
 * @param mcp_port   TCP port for the MCP HTTP server, or 0 to disable.
 * @return 0 on success, negative errno on failure (-EALREADY if already init'd).
 */
int  fluxipc_server_init(const char *prog_name, uint16_t mcp_port);
int  fluxipc_interactive_init(const char *prog_name);

/* ─── Event loop ──────────────────────────────────────────────────────────── */
int  fluxipc_poll(void *unused);

/* ─── Shutdown ────────────────────────────────────────────────────────────── */
void fluxipc_stop(void);
void fluxipc_destroy(void);

/* ─── Help ────────────────────────────────────────────────────────────────── */
void fluxipc_usage(const char *prog);

/* ─── Server request hooks (observation only) ─────────────────────────────── */

/** Read-only view of one incoming request, passed to the pre/post hooks. */
typedef struct {
    const char        *path;    /* IPC path invoked */
    int                argc;
    char       *const *argv;    /* argv[0..argc-1]; argv[0] == last path component */
    int                matched; /* 1 if an endpoint handler was found for path */
    int                status;  /* handler return code — valid in POST hook only */
    const void        *out_buf; /* response bytes — valid in POST hook only */
    size_t             out_len; /* response length — valid in POST hook only */
    void              *data;    /* endpoint context (node->data); NULL if unmatched */
} fluxipc_request_t;

/**
 * Observation hook. Called once before the handler runs (pre) and once after it
 * runs, before the response is sent (post). It cannot alter the response and
 * must not block; it is intended for logging, auditing, and metrics.
 *
 * In the PRE hook, @p req->status / out_buf / out_len are not yet meaningful.
 * In the POST hook, they reflect the handler's actual result.
 */
typedef void (*fluxipc_request_hook_fn)(const fluxipc_request_t *req, void *user);

/**
 * Register global pre/post request hooks. Either may be NULL.
 * Must be called after fluxipc_server_init(). @p user is passed through to
 * both hooks. Calling again replaces the previous hooks.
 */
void fluxipc_set_request_hooks(fluxipc_request_hook_fn pre,
                               fluxipc_request_hook_fn post,
                               void                   *user);

/* ─── Client API ────────────────────────────────────────────────────────────── */

/** Callback for fluxipc_list_endpoints: receives one active endpoint. */
typedef void (*fluxipc_list_cb)(const char *path, uint32_t id,
                                const char *usage, void *userdata);

/**
 * Enumerate active IPC endpoints for a running server.
 * Opens the server's shared memory read-only, locks, iterates,
 * and calls @p cb for each active entry, then unlocks and closes.
 *
 * @return 0 on success, -ENOENT if shm cannot be opened.
 */
int fluxipc_list_endpoints(const char *prog_name,
                           fluxipc_list_cb cb, void *userdata);

/**
 * Low-level single IPC call to a Unix socket.
 * Sends path + arguments, receives response.
 *
 * @return status from the handler (0 on success, negative errno on error).
 */
int fluxipc_call(const char *sock_path, const char *ipc_path,
                 int argc, char **argv,
                 void *out_buf, size_t out_cap, size_t *out_len);

/**
 * Resolve a FluxIPC symlink argv[0] into server connection parameters.
 * e.g. /run/myprog-fluxipc/devices/stub → prog="myprog",
 *      sock="/run/myprog-fluxipc/myprog.sock", shm="/fluxipc.myprog".
 *
 * @return 0 on success, -1 if argv0 is not a FluxIPC symlink.
 */
int fluxipc_client_resolve(const char *argv0,
                           char *out_prog, size_t prog_sz,
                           char *out_sock, size_t sock_sz,
                           char *out_shm,  size_t shm_sz);

/**
 * Try to handle this process as a FluxIPC symlink-based client invocation.
 * Call this at the start of main(), before fluxipc_server_init().
 *
 * If argv[0] is a FluxIPC symlink (e.g. /run/<prog>-fluxipc/<path>),
 * executes the IPC call, prints the response, and returns 0.
 * The caller should immediately return this value.
 *
 * If argv[0] is NOT a FluxIPC symlink, returns -1.
 * The caller should proceed with normal server initialisation.
 *
 * @return 0 on success, -1 if not a symlink (continue as server).
 */
int fluxipc_client_dispatch(int argc, char **argv);

#ifdef __cplusplus
}
#endif
