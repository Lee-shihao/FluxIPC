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

# Debug build with verbose MCP request/response logging
make debug         # equivalent to: make MCP_DEBUG=1 all

# Install to system (default PREFIX=/usr)
sudo make install
sudo make install PREFIX=/usr/local

# Uninstall
sudo make uninstall

# Clean build artifacts (also removes leftover /dev/shm/fluxipc_* and /tmp/fluxipc_*.sock)
make clean

# Run the demo
./example_server                          # terminal 1
fluxipc-cli --server example_server /system/info  # terminal 2

# Python integration tests (requires example_server running on the target host)
python3 python/test_mcp.py
python3 python/test_mcp.py --host 192.168.1.100 --port 32100
```

## Architecture

FluxIPC is a **namespace-tree IPC library** for Linux. Applications embed it as a library; no external daemon is needed.

### Component layers (bottom-up)

1. **Namespace tree** (`fluxipc_tree.c`) — internal nodes are containers; only leaf nodes carry handlers. Removing a leaf prunes empty ancestors automatically. Thread-safe via `g_ctx->tree_lock` mutex.

2. **Shared memory registry** (`fluxipc_shm.c`) — one `fluxipc_shm_t` per server in `/dev/shm/fluxipc.<prog>`. Contains a process-shared `pthread_rwlock_t` so concurrent clients can read safely. Entry array capped at 512. Clients mmap read-only; the embedded rwlock is opened RDWR (kernel allows this for futex-based locks).

3. **Socket transport** (`fluxipc_sock.c`) — Unix domain socket at `/run/<prog>-fluxipc/<prog>.sock`. Binary wire protocol with magic `FLXP`, 4-byte header + payload. One request/response per connection.

4. **Symlink discovery** (`fluxipc_symlink.c`) — each endpoint gets a symlink under `/run/<prog>-fluxipc/<path>`. Calling the symlink directly (as `argv[0]`) triggers `fluxipc_client_dispatch()` which routes to the correct server.

5. **MCP server** (`fluxipc_mcp.c`) — hand-rolled HTTP + JSON parser (zero dependencies). Speaks both Streamable HTTP (2025-03-26) and legacy SSE (2024-11-05) on a single TCP port. All endpoints are auto-exposed as MCP tools (slashes → underscores). Session-isolated with `Mcp-Session-Id` header.

6. **Interactive shell** (`fluxipc_interactive.c`) — readline-based REPL with tab completion, `ls`/`cd`/`help`/`watch`/`reload` commands, and inline range syntax.

### Lifecycle flow

```
main()
 ├─ fluxipc_client_dispatch(argc, argv)   // check if invoked as symlink; return early if so
 ├─ fluxipc_server_init("myapp", port)    // creates socket, shm, symlinks, registers FLUXIPC_STATIC entries
 ├─ fluxipc_register(...)                 // optional dynamic endpoints
 ├─ while (running) fluxipc_poll(NULL)    // single select() loop: dispatches IPC socket + MCP connections
 └─ fluxipc_destroy()                     // stops MCP, removes symlinks, unlinks socket, closes shm
```

### Key global state

All server state lives in `extern fluxipc_ctx_t *g_ctx` (defined in `fluxipc_core.c`). Single-instance per process — `fluxipc_server_init()` returns `-EALREADY` if called twice.

### Compile-time registration mechanism

`FLUXIPC_STATIC(path, usage, slot_sz, handler, ctx)` places an `fluxipc_entry_t` into the `fluxipc_registry` ELF section. During `fluxipc_server_init()`, the linker-provided `__start_fluxipc_registry` / `__stop_fluxipc_registry` bounds are iterated. Requires `-rdynamic` (or `-Wl,--export-dynamic`) so the linker retains section symbols.

## Conventions

- **No external dependencies** — the MCP server hand-rolls HTTP + JSON parsing. The only link-time deps are `libpthread`, `librt`, and `libreadline`.
- **C standard** — GNU C (`_GNU_SOURCE`), compiled with `-Wall -Wextra -O2 -g`. Unused parameters suppressed with `-Wno-unused-parameter`.
- **Error returns** — functions return 0 on success, negative errno on failure.
- **Handler convention** — `argv[0]` is always the full IPC path; user args start at `argv[1]`.
- **Debug logging** — MCP module uses compile-time `MCP_DEBUG` flag (`make MCP_DEBUG=1`). Other modules use `fprintf(stderr, ...)` directly.
- **Single-threaded event loop** — the server is single-threaded via `select()`. Tree operations are mutex-protected for safety, but the intended use is one-thread polling.
- **File permissions** — runtime dir 0755, socket 0666, SHM 0644 (all world-accessible so unprivileged clients work).

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
