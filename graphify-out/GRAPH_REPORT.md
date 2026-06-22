# Graph Report - .  (2026-06-22)

## Corpus Check
- Corpus is ~22,882 words - fits in a single context window. You may not need a graph.

## Summary
- 245 nodes · 524 edges · 11 communities
- Extraction: 86% EXTRACTED · 14% INFERRED · 0% AMBIGUOUS · INFERRED: 71 edges (avg confidence: 0.81)
- Token cost: 0 input · 43,027 output

## Community Hubs (Navigation)
- [[_COMMUNITY_MCP Server|MCP Server]]
- [[_COMMUNITY_Core IPC Engine|Core IPC Engine]]
- [[_COMMUNITY_Interactive Shell|Interactive Shell]]
- [[_COMMUNITY_Python Client|Python Client]]
- [[_COMMUNITY_Documentation & Design|Documentation & Design]]
- [[_COMMUNITY_Example Server & Handlers|Example Server & Handlers]]
- [[_COMMUNITY_CLI & Symlink Discovery|CLI & Symlink Discovery]]
- [[_COMMUNITY_Python Integration Tests|Python Integration Tests]]
- [[_COMMUNITY_Shared Memory Registry|Shared Memory Registry]]
- [[_COMMUNITY_Packaging Install Script|Packaging Install Script]]

## God Nodes (most connected - your core abstractions)
1. `FluxIPC` - 17 edges
2. `FluxIPC` - 16 edges
3. `dispatch_jsonrpc()` - 14 edges
4. `dispatch()` - 13 edges
5. `handle_http_request()` - 13 edges
6. `handle_tools_call()` - 12 edges
7. `mcp_conn_t` - 11 edges
8. `check()` - 10 edges
9. `main()` - 10 edges
10. `path_node_t` - 9 edges

## Surprising Connections (you probably didn't know these)
- `Six-Layer Bottom-Up Component Architecture` --semantically_similar_to--> `Four Core Design Principles of FluxIPC`  [INFERRED] [semantically similar]
  CLAUDE.md → FluxIPC_CSDN.md
- `Compile-Time Registration via ELF Section (FLUXIPC_STATIC)` --semantically_similar_to--> `Compile-time + Runtime Dual Registration Mechanism`  [INFERRED] [semantically similar]
  CLAUDE.md → FluxIPC_CSDN.md
- `Zero External Dependencies Design Principle` --semantically_similar_to--> `Design Notes — No Daemon, Symlink Resolution, Hand-rolled MCP`  [INFERRED] [semantically similar]
  CLAUDE.md → README.md
- `FluxIPC CSDN Article — Lightweight Linux IPC Framework` --semantically_similar_to--> `FluxIPC — Lightweight namespace-tree IPC library for Linux (README)`  [INFERRED] [semantically similar]
  FluxIPC_CSDN.md → README.md
- `Compile-time + Runtime Dual Registration Mechanism` --semantically_similar_to--> `FLUXIPC_STATIC Compile-time Registration Macro`  [INFERRED] [semantically similar]
  FluxIPC_CSDN.md → README.md

## Import Cycles
- 1-file cycle: `python/test_mcp.py -> python/test_mcp.py`

## Hyperedges (group relationships)
- **Four Core Design Principles of FluxIPC** — fluxipc_fluxipc_csdn_namespace_tree_design, fluxipc_fluxipc_csdn_dual_registration, fluxipc_fluxipc_csdn_symlink_zero_config, fluxipc_fluxipc_csdn_mcp_ai_integration [EXTRACTED 1.00]

## Communities (11 total, 0 thin omitted)

### Community 0 - "MCP Server"
Cohesion: 0.15
Nodes (39): mcp_conn_t, mcp_session_t, build_error(), build_result(), fluxipc_ctx_t, conn_alloc(), conn_close(), conn_read() (+31 more)

### Community 1 - "Core IPC Engine"
Cohesion: 0.11
Nodes (36): FILE, fluxipc_handler_fn, socket, fluxipc_ctx_t, fluxipc_node_t, ctx_alloc(), fluxipc_destroy(), fluxipc_poll() (+28 more)

### Community 2 - "Interactive Shell"
Cohesion: 0.13
Nodes (33): arg_spec_t, fluxipc_shm_entry_t, path_node_t, build_candidates(), cmd_cd(), cmd_help(), cmd_ls(), cmd_watch() (+25 more)

### Community 3 - "Python Client"
Cohesion: 0.09
Nodes (16): Any, Exception, FluxIPC, FluxIPCError, fluxipc.py – Python client for FluxIPC MCP server  Transport: Streamable HTTP (M, Extract body from a raw HTTP/1.1 response.          Returns the body bytes, whic, Send one JSON-RPC request, return result (raises FluxIPCError on error)., Send a JSON-RPC notification (no id, 202 response expected). (+8 more)

### Community 4 - "Documentation & Design"
Cohesion: 0.15
Nodes (23): Compile-Time Registration via ELF Section (FLUXIPC_STATIC), Six-Layer Bottom-Up Component Architecture, FluxIPC — Namespace-tree IPC library for Linux (CLAUDE.md), Global Server State (fluxipc_ctx_t *g_ctx), Server Lifecycle Flow (client_dispatch → server_init → register → poll → destroy), Zero External Dependencies Design Principle, Single-Threaded select() Event Loop, Codebase File Index (CODEBASE_INDEX.md) (+15 more)

### Community 5 - "Example Server & Handlers"
Cohesion: 0.09
Nodes (3): fluxipc_stop(), sig_handler(), system_shutdown_handler()

### Community 6 - "CLI & Symlink Discovery"
Cohesion: 0.19
Nodes (14): cmd_list(), main(), print_help(), mode_t, fluxipc_client_dispatch(), client_ipc_path(), fluxipc_client_resolve(), get_binary_path() (+6 more)

### Community 7 - "Python Integration Tests"
Cohesion: 0.45
Nodes (13): FluxIPC, check(), fail(), main(), ok(), test_initialize(), test_path_formats(), test_ping() (+5 more)

### Community 8 - "Shared Memory Registry"
Cohesion: 0.24
Nodes (12): fluxipc_list_cb, fluxipc_shm_t, fluxipc_list_endpoints(), fluxipc_ctx_t, fluxipc_node_t, make_shm_name(), shm_add_entry(), shm_close() (+4 more)

### Community 9 - "Packaging Install Script"
Cohesion: 0.83
Nodes (3): install.sh script, install_fluxipc(), uninstall_fluxipc()

## Knowledge Gaps
- **5 isolated node(s):** `fluxipc_handler_fn`, `fluxipc_list_cb`, `fluxipc_shm_entry_t`, `fluxipc_node_t`, `mode_t`
  These have ≤1 connection - possible missing edges or undocumented components.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `FluxIPC` connect `Python Integration Tests` to `Core IPC Engine`, `Interactive Shell`, `Python Client`, `Example Server & Handlers`, `CLI & Symlink Discovery`?**
  _High betweenness centrality (0.296) - this node is a cross-community bridge._
- **Why does `FluxIPC` connect `Python Client` to `Python Integration Tests`?**
  _High betweenness centrality (0.153) - this node is a cross-community bridge._
- **Why does `socket` connect `Core IPC Engine` to `MCP Server`, `Python Client`?**
  _High betweenness centrality (0.140) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `FluxIPC` (e.g. with `FluxIPC` and `FluxIPCError`) actually correct?**
  _`FluxIPC` has 2 INFERRED edges - model-reasoned connections that need verification._
- **What connects `fluxipc.py – Python client for FluxIPC MCP server  Transport: Streamable HTTP (M`, `Raised when the server returns a JSON-RPC error.`, `MCP Streamable-HTTP client for a FluxIPC server.      Parameters     ----------` to the rest of the system?**
  _20 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Core IPC Engine` be split into smaller, more focused modules?**
  _Cohesion score 0.10526315789473684 - nodes in this community are weakly interconnected._
- **Should `Interactive Shell` be split into smaller, more focused modules?**
  _Cohesion score 0.12660028449502134 - nodes in this community are weakly interconnected._