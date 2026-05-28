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
int  fluxipc_server_init(const char *prog_name);
int  fluxipc_interactive_init(const char *prog_name);

/* ─── Event loop ──────────────────────────────────────────────────────────── */
int  fluxipc_poll(void *unused);

/* ─── Shutdown ────────────────────────────────────────────────────────────── */
void fluxipc_stop(void);
void fluxipc_destroy(void);

/* ─── Help ────────────────────────────────────────────────────────────────── */
void fluxipc_usage(const char *prog);

/* ─── Client symlink dispatch ─────────────────────────────────────────────── */
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
