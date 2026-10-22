# Codebase Index
> IPC-only FluxIPC · namespace-tree calls over a Unix socket

## Source

**cli/**
- `fluxipc_cli.c` — `fluxipc-cli` list / call / interactive entry point

**include/**
- `fluxipc.h` — public API: handlers, methods, tree, lifecycle, event loop,
  host-thread helpers, client calls
- `fluxipc_internal.h` — wire header, tree node, server context, connection
  pool and internal dispatch declarations

**src/**
- `fluxipc_core.c` — context, registration, removal, text manifest, IPC
  dispatch, hooks, poll/run/stop/destroy, endpoint listing
- `fluxipc_tree.c` — namespace tree (containers + IPC leaves)
- `fluxipc_sock.c` — Unix-socket listener, CALL/LIST request processor,
  client helpers
- `fluxipc_conn.c` — non-blocking connection pool and `select()` event loop
- `fluxipc_interactive.c` — readline REPL (`ls`, `cd`, `help`, `watch`,
  `reload`, tab completion)

**tests/**
- `example_server.c` — IPC-only demo; runs `fluxipc_run()` in a host-created
  thread
- `unit/` — cmocka suites (`fluxipc-ut`)
- `integration/main.c` — end-to-end IPC, concurrency, large payloads,
  graceful shutdown (`fluxipc-it`)
- `bench/bench.c` — latency/throughput/RSS baseline (`fluxipc-bench`)

## Config
- `.claude/settings.json`

## Docs
- `CLAUDE.md`
- `CODEBASE_INDEX.md`
- `README.md`
- `README.zh-CN.md`
- `FluxIPC_CSDN.md`
