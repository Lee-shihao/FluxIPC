# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Codebase Navigation
**Always read `CODEBASE_INDEX.md` before opening any source file.**
It contains the complete file map with exports and purpose for every file.
Use it to locate the exact file you need, then read only that file.

## Commands

```bash
# Build everything (library, CLI, examples)
make -j$(nproc)

# Individual targets
make lib           # shared library only (libfluxipc.so.1.0.0)
make static        # static library only (libfluxipc.a)
make cli           # CLI tool only (fluxipc-cli)
make examples      # example server (example_server)

# Install to system (default PREFIX=/usr)
sudo make install
sudo make install PREFIX=/usr/local

# Uninstall
sudo make uninstall

# Clean build artifacts (also removes leftover /tmp/fluxipc_*.sock)
make clean

# Run the demo (the example server runs fluxipc_run() in a host thread)
./example_server                                  # terminal 1
fluxipc-cli example_server                        # terminal 2: list
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3

# Tests
make test          # unit tests + integration tests
make ut            # unit tests only (pure, no server/network)
make it            # integration tests (forks example_server, IPC only)
make test-mem      # ASan/UBSan unit + instrumented integration/leak checks
make it-asan       # instrumented server + integration suite (LeakSanitizer)
make bench         # latency/throughput baseline (p50/p95/p99, ops/s, RSS)
```

## Architecture

FluxIPC is a **namespace-tree IPC library** for Linux. Applications embed it as a library; no external daemon is needed. There is no HTTP or JSON layer: every call is positional `argv` in, raw bytes out, over a Unix domain socket.

### Component layers (bottom-up)

1. **Namespace tree** (`fluxipc_tree.c`) — internal nodes are containers; leaf nodes carry a `fluxipc_method_t` (`summary`, `ipc`, `data`). Registration is path-based (`fluxipc_register()`, `fluxipc_register_method()`) with node-based primitives (`fluxipc_node_mkdir()` / `fluxipc_node_leaf()`) available for runtime mutation. Removing a leaf/subtree prunes empty ancestors automatically. Thread-safe via `g_ctx->tree_lock` mutex. `fluxipc_node_leaves()` walks the subtree recursively (no side index), so listing cost is proportional to the subtree size.

2. **Unified connection layer** (`fluxipc_conn.c`) — one `select()` loop over the Unix listener, the wake eventfd and a fixed pool of connections. A non-blocking, buffered read / queued-write state machine means a slow peer never blocks the server. One request/response per connection. The pool is initialised to `fd == -1` by `conn_pool_init()` (called from `fluxipc_server_init`) — a zeroed pool would treat fd 0 as busy.

3. **Socket transport** (`fluxipc_sock.c`) — Unix domain socket at `/run/user/<uid>/<prog>-fluxipc/<prog>.sock`. Minimal 12-byte wire header (`magic, op, path_len, arg_len`). `op=CALL` invokes the leaf's IPC handler and returns its raw bytes; `op=LIST` returns a compact text manifest (`<id>\t<path>\t<summary>\n`) used by `fluxipc_list_endpoints` and the interactive shell.

4. **Interactive shell** (`fluxipc_interactive.c`) — readline-based REPL with tab completion, `ls`/`cd`/`help`/`watch`/`reload` commands, and inline range syntax. It is linked into `fluxipc-cli` only, not into the core library. It discovers running servers by scanning the per-user run directory and queries each server's socket for its manifest.

### Threading model

The library **never creates a thread**. The host application owns scheduling:

```c
static void *serve(void *arg) { fluxipc_run(); return NULL; }

fluxipc_server_init("myapp");
fluxipc_register(...);
pthread_create(&th, NULL, serve, NULL);
/* host does its own work; shutdown handler calls fluxipc_stop() */
pthread_join(th, NULL);
fluxipc_destroy();
```

- `fluxipc_run()` blocks until `fluxipc_stop()`.
- `fluxipc_poll_timeout(ms)` is for host schedulers that want their own loop
  (`<0` infinite, `0` non-blocking, `>0` capped).
- `fluxipc_wakeup()` wakes a blocked poll without stopping it.
- `fluxipc_stop()` and `fluxipc_wakeup()` are safe from any thread.
- Registration/removal are protected by `tree_lock` and may run concurrently
  with the polling thread.
- Exactly one thread may poll at a time; a second poll returns `-EBUSY`.
- `fluxipc_destroy()` must run after the polling thread has exited (join it).

### Lifecycle flow

```
main()
 ├─ fluxipc_server_init("myapp")          // creates socket + run dir
 ├─ fluxipc_register("/a", "usage", 0, handler, data)
 ├─ pthread_create(&th, run_fluxipc, NULL) // host-owned scheduling
 ├─ ... application work ...
 ├─ fluxipc_stop()
 ├─ pthread_join(th, NULL)                // no poll thread remains
 └─ fluxipc_destroy()                     // unlinks socket, removes run dir
```

### Key global state

All server state lives in `extern fluxipc_ctx_t *g_ctx` (defined in
`fluxipc_core.c`). Single-instance per process — `fluxipc_server_init()` returns
`-EALREADY` if called twice.

### Registration mechanism

There is no linker-section registration: `FLUXIPC_STATIC(path, usage, slot_sz, handler, ctx)` queues an entry from a `__attribute__((constructor))`, and `fluxipc_server_init()` applies the queue. This restores main-branch source compatibility without the old `fluxipc_registry` ELF section or the `-rdynamic` requirement.

The recommended API is path-based: `fluxipc_register_method("/a/b/c", &method)` creates any missing intermediate containers and is atomic on failure; `fluxipc_register_all(table, count)` registers a static `fluxipc_endpoint_t[]` in one call. The classic `main`-branch signature `fluxipc_register(path, usage, slot_sz, handler, data)` is also exported (usage→summary, slot_sz ignored) with `fluxipc_unregister()` as a removal alias, so existing main-branch code links unchanged. Registering the same path again merges: still-empty metadata is filled, and an existing handler is never overwritten. For runtime tree mutation, the underlying primitives `fluxipc_node_mkdir(parent, name)` (get-or-create container) and `fluxipc_node_leaf(parent, name, &method)` (create leaf, strict EEXIST) remain. Leaf metadata (`summary`) and the `ipc` handler live in a `fluxipc_method_t`; `FLUXIPC_METHOD` / `FLUXIPC_METHOD_D` build one inline. Endpoint discovery is served over the socket (the `LIST` op).

## Conventions

- **Dependencies** — the core library needs only libc / POSIX and `libpthread`/`librt`; `libreadline` is used by `fluxipc-cli` for the REPL (never by `libfluxipc`).
- **C standard** — GNU C11 (`-std=gnu11`, `_GNU_SOURCE`), compiled with `-Wall -Wextra -O2 -g`. Unused parameters suppressed with `-Wno-unused-parameter`.
- **Error returns** — functions return 0 on success, negative errno on failure.
- **Handler convention** — a leaf has one handler: `ipc` receives positional `argv` (argv[0] is the endpoint path), writes raw bytes into `out`, and sets `*out_len`. A missing handler yields `-ENOENT`. The server's per-call output buffer is 64 KiB; handlers must respect `out_cap`.
- **Debug logging** — modules use `fprintf(stderr, ...)` directly.
- **Single-threaded dispatch** — handlers run on the polling thread. Registration and removal are mutex-protected; public client calls (`fluxipc_call`, `fluxipc_list_endpoints`) open independent connections and are thread-safe.
- **File permissions** — runtime dir 0755, socket 0666 (world-accessible so unprivileged clients work).

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
