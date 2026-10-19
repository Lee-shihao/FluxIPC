# Codebase Index
> 2026-06-22 · 23 files · ~65.0k tokens total
>
> **How to use:** Read this file first. Navigate to the exact file you need,
> then read only that file. Do not read entire directories.

## Source

**cli/**
- `fluxipc_cli.c`

**include/**
- `fluxipc_internal.h`
- `fluxipc.h`

**python/**
- `fluxipc.py` — FluxIPCError, FluxIPC (stdlib REST client)
- `test_http.py` — integration test for the HTTP server

**src/**
- `fluxipc_core.c`
- `fluxipc_interactive.c`
- `fluxipc_http.c`
- `fluxipc_conn.c`
- `fluxipc_util.c`
- `fluxipc_sock.c`
- `fluxipc_tree.c`

**tests/**
- `example_server.c` — demo/dual IPC + JSON server
- `unit/` — cmocka suites (`fluxipc-ut`)
- `integration/main.c` — end-to-end IPC/JSON/HTTP + robustness (`fluxipc-it`)
- `bench/bench.c` — latency/throughput/RSS baseline (`fluxipc-bench`)

## Config
- `.claude/settings.json`

## Docs
- `CLAUDE.md`
- `CODEBASE_INDEX.md`
- `FluxIPC_CSDN.md`
- `README.md`

---
*Index: ~221 tokens · Full codebase: ~65.0k tokens · Saves ~100%*
