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

# Debug build with verbose HTTP request/response logging
make debug         # equivalent to: make HTTP_DEBUG=1 all

# Install to system (default PREFIX=/usr)
sudo make install
sudo make install PREFIX=/usr/local

# Uninstall
sudo make uninstall

# Clean build artifacts (also removes leftover /tmp/fluxipc_*.sock)
make clean

# Run the demo
./example_server                          # terminal 1
fluxipc-cli --server example_server /system/info  # terminal 2

# Tests
make test          # unit tests + integration tests
make ut            # unit tests only (pure, no server/network)
make it            # integration tests (forks example_server, IPC + HTTP)

# Python HTTP integration tests (requires example_server running on the target)
python3 python/test_http.py
python3 python/test_http.py --host 192.168.1.100 --port 32100
```

## Architecture

FluxIPC is a **namespace-tree IPC library** for Linux. Applications embed it as a library; no external daemon is needed.

### Component layers (bottom-up)

1. **Namespace tree** (`fluxipc_tree.c`) — internal nodes are containers; leaf nodes carry a `fluxipc_method_t` (summary/description/schema/methods/`ipc`/`http`). Registration is path-based (`fluxipc_register()`) with node-based primitives (`fluxipc_node_mkdir()` / `fluxipc_node_leaf()`) available for runtime mutation. Removing a leaf/subtree prunes empty ancestors automatically. Thread-safe via `g_ctx->tree_lock` mutex. `fluxipc_node_leaves()` walks the subtree recursively (no side index), so listing cost is proportional to the subtree size.

2. **Unified connection layer** (`fluxipc_conn.c`) — one `select()` loop over both listeners (Unix socket + optional HTTP) and a fixed pool of connections. Both transports share a non-blocking, buffered read / queued-write state machine; a slow peer never blocks the server. One request/response per connection. The pool is initialised to `fd == -1` by `conn_pool_init()` (called from `fluxipc_server_init`) — a zeroed pool would treat fd 0 as busy.

3. **Socket transport** (`fluxipc_sock.c`) — Unix domain socket at `/run/user/<uid>/<prog>-fluxipc/<prog>.sock`. Minimal 12-byte wire header (`magic, op, path_len, arg_len`). `op=CALL` invokes the leaf's `ipc` handler and returns its raw bytes; `op=LIST` returns the leaf manifest (JSON) used by `fluxipc_list_endpoints` and the interactive shell.

4. **HTTP server** (`fluxipc_http.c`) — hand-rolled standard HTTP/1.1 request parser / response builder. Maps URL paths to tree nodes and exposes leaves as REST resources: `GET`/`POST`/`PUT`/`PATCH`/`DELETE` invoke the leaf's `http` handler, `GET` on a container returns its leaf manifest, `OPTIONS` returns `Allow`, `/openapi.json` serves an OpenAPI 3.1 document, and errors use RFC 7807 `application/problem+json`. JSON is parsed/serialised with cJSON. `Connection: close`, no sessions/SSE/keep-alive.

5. **Interactive shell** (`fluxipc_interactive.c`) — readline-based REPL with tab completion, `ls`/`cd`/`help`/`watch`/`reload` commands, and inline range syntax. It discovers running servers by scanning the per-user run directory and queries each server's socket for its manifest.

### Lifecycle flow

```
main()
 ├─ fluxipc_server_init("myapp", http_port) // creates socket + optional HTTP server
 ├─ fluxipc_register("/a/b", &method)       // build the tree (creates ancestors)
 ├─ while (fluxipc_poll(NULL) >= 0)       // single select() loop: IPC socket + HTTP connections
 └─ fluxipc_destroy()                     // stops HTTP, unlinks socket, removes run dir
```

### Key global state

All server state lives in `extern fluxipc_ctx_t *g_ctx` (defined in `fluxipc_core.c`). Single-instance per process — `fluxipc_server_init()` returns `-EALREADY` if called twice.

### Registration mechanism (dynamic only)

There is no compile-time registration. `FLUXIPC_STATIC` and the `fluxipc_registry` ELF section were removed, along with the `-rdynamic` requirement. The recommended API is path-based: `fluxipc_register("/a/b/c", &method)` creates any missing intermediate containers and is atomic on failure; `fluxipc_register_all(table, count)` registers a static `fluxipc_endpoint_t[]` in one call. For runtime tree mutation, the underlying primitives `fluxipc_node_mkdir(parent, name)` (get-or-create container) and `fluxipc_node_leaf(parent, name, &method)` (create leaf) remain. Leaf metadata (`summary`, `description`, `schema`, `methods`) plus the two optional handlers (`ipc`, `http`) live in a `fluxipc_method_t`; `FLUXIPC_METHOD` / `FLUXIPC_METHOD_D` build one inline. Endpoint discovery is served over the socket (the `LIST` op).

## Conventions

- **Dependencies** — cJSON (`libcjson`) for JSON, `libreadline` for the REPL, plus `libpthread` and `librt`. The HTTP layer is hand-rolled (no HTTP library).
- **C standard** — GNU C (`_GNU_SOURCE`), compiled with `-Wall -Wextra -O2 -g`. Unused parameters suppressed with `-Wno-unused-parameter`.
- **Error returns** — functions return 0 on success, negative errno on failure.
- **Handler convention** — a leaf has two independent handlers: `ipc` (positional `argv`, writes raw bytes into `out` and sets `*out_len`) and `http` (reads `req->json`/query, builds a cJSON response in `resp->json`). The server serialises and frees `resp->json`. A missing handler yields `-ENOSYS` (HTTP 501) / `-ENOENT` for IPC.
- **Debug logging** — the HTTP module uses the compile-time `HTTP_DEBUG` flag (`make HTTP_DEBUG=1`). Other modules use `fprintf(stderr, ...)` directly.
- **Single-threaded event loop** — the server is single-threaded. `fluxipc_conn.c` drives one `select()` over both listeners and all connections with a shared non-blocking buffered state machine. Tree operations are mutex-protected for safety.
- **File permissions** — runtime dir 0755, socket 0666 (world-accessible so unprivileged clients work).

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
