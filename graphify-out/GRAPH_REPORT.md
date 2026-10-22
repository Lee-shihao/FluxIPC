# Graph Report - FluxIPC  (2026-10-22)

## Corpus Check
- 38 files · ~33,435 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 545 nodes · 1373 edges · 21 communities (18 shown, 3 thin omitted)
- Extraction: 59% EXTRACTED · 41% INFERRED · 0% AMBIGUOUS · INFERRED: 565 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `e9bf42f1`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 21|Community 21]]
- [[_COMMUNITY_Community 23|Community 23]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 35|Community 35]]
- [[_COMMUNITY_Community 36|Community 36]]

## God Nodes (most connected - your core abstractions)
1. `fluxipc_node_leaf()` - 37 edges
2. `fluxipc_tree_root()` - 36 edges
3. `ut_capture_begin()` - 27 edges
4. `ut_capture_end()` - 27 edges
5. `fluxipc_call()` - 24 edges
6. `dispatch()` - 22 edges
7. `tree_insert_path()` - 21 edges
8. `fluxipc_node_t` - 19 edges
9. `fluxipc_register_method()` - 19 edges
10. `ut_server_down()` - 19 edges

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
- None detected.

## Hyperedges (group relationships)
- **Four Core Design Principles of FluxIPC** — fluxipc_fluxipc_csdn_namespace_tree_design, fluxipc_fluxipc_csdn_dual_registration, fluxipc_fluxipc_csdn_symlink_zero_config, fluxipc_fluxipc_csdn_mcp_ai_integration [EXTRACTED 1.00]

## Communities (21 total, 3 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.07
Nodes (67): fluxipc_endpoint_t, fluxipc_entry_t, fluxipc_handler_fn, fluxipc_leaf_cb, fluxipc_request_hook_fn, fluxipc_node_t, node_is_leaf(), fluxipc_ctx_t (+59 more)

### Community 2 - "Community 2"
Cohesion: 0.05
Nodes (103): arg_spec_t, path_node_t, fluxipc_usage(), build_candidates(), builtin_generator(), fluxipc_node_t, cmd_cd(), cmd_help() (+95 more)

### Community 3 - "Community 3"
Cohesion: 0.15
Nodes (6): ut_cli(), ut_core(), ut_interactive(), ut_protocol(), ut_sock(), main()

### Community 4 - "Community 4"
Cohesion: 0.15
Nodes (23): Compile-Time Registration via ELF Section (FLUXIPC_STATIC), Six-Layer Bottom-Up Component Architecture, FluxIPC — Namespace-tree IPC library for Linux (CLAUDE.md), Global Server State (fluxipc_ctx_t *g_ctx), Server Lifecycle Flow (client_dispatch → server_init → register → poll → destroy), Zero External Dependencies Design Principle, Single-Threaded select() Event Loop, Codebase File Index (CODEBASE_INDEX.md) (+15 more)

### Community 5 - "Community 5"
Cohesion: 0.15
Nodes (23): fluxipc_run(), arg_int(), fluxipc_event_t, compiled_info_ipc(), config_log_level_ipc(), config_max_clients_ipc(), demo_add_ipc(), demo_counter_ipc() (+15 more)

### Community 7 - "Community 7"
Cohesion: 0.05
Nodes (36): Build, CLI, Client API, Dependencies, Design Notes, Features, File Locations, FluxIPC (+28 more)

### Community 8 - "Community 8"
Cohesion: 0.18
Nodes (36): accept_unix(), fluxipc_conn_t, fluxipc_ctx_t, conn_alloc(), conn_close(), conn_effective_wait_ms(), conn_idle_wait_ms(), conn_poll() (+28 more)

### Community 9 - "Community 9"
Cohesion: 0.83
Nodes (3): install.sh script, install_fluxipc(), uninstall_fluxipc()

### Community 11 - "Community 11"
Cohesion: 0.15
Nodes (12): 1. 一次调用长什么样, 2. 线程模型：库不拥有线程, 3. 线协议, 4. 非阻塞连接层, 5. 命名空间树, 6. 兼容 main 分支, 7. 测试, 8. 小结 (+4 more)

### Community 12 - "Community 12"
Cohesion: 0.21
Nodes (27): fluxipc_method_t, fluxipc_node_t, dup_or_null(), node_alloc(), set_full_path(), tree_destroy(), tree_find_child(), tree_find_child_n() (+19 more)

### Community 13 - "Community 13"
Cohesion: 0.17
Nodes (10): Architecture, Codebase Navigation, Commands, Component layers (bottom-up), Conventions, graphify, Key global state, Lifecycle flow (+2 more)

### Community 15 - "Community 15"
Cohesion: 0.40
Nodes (4): Codebase Index, Config, Docs, Source

### Community 16 - "Community 16"
Cohesion: 0.25
Nodes (16): ipc(), main(), server_path(), shutdown_server(), sleep_ms(), start_server(), stop_server(), test_arithmetic() (+8 more)

### Community 17 - "Community 17"
Cohesion: 0.07
Nodes (60): bench_parallel(), bench_seq(), env_long(), finish(), ipc_math_once(), ipc_once(), main(), now_ns() (+52 more)

### Community 20 - "Community 20"
Cohesion: 0.09
Nodes (39): fluxipc_list_cb, ctx_free(), fluxipc_destroy(), fluxipc_is_running(), fluxipc_list_endpoints(), fluxipc_poll(), fluxipc_poll_timeout(), fluxipc_server_init() (+31 more)

### Community 21 - "Community 21"
Cohesion: 0.12
Nodes (15): 1. 测试入口, 2. 目录结构, 3. 测试策略, 4. 关键覆盖点, 5. 基准, 6. 工具链, cmocka 约定, FluxIPC 测试说明 (+7 more)

### Community 23 - "Community 23"
Cohesion: 0.25
Nodes (7): env.sh script, CMOCKA_PREFIX, FLUXIPC_TOOLS, LD_LIBRARY_PATH, MULL_FRONTEND, MULL_RUNNER, PATH

## Knowledge Gaps
- **84 isolated node(s):** `fluxipc_node_t`, `fluxipc_endpoint_t`, `fluxipc_handler_fn`, `fluxipc_entry_t`, `fluxipc_request_hook_fn` (+79 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **3 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `fluxipc_call()` connect `Community 17` to `Community 16`, `Community 2`?**
  _High betweenness centrality (0.095) - this node is a cross-community bridge._
- **Why does `fluxipc_node_leaf()` connect `Community 0` to `Community 17`, `Community 2`, `Community 12`?**
  _High betweenness centrality (0.092) - this node is a cross-community bridge._
- **Why does `fluxipc_tree_root()` connect `Community 0` to `Community 17`, `Community 2`?**
  _High betweenness centrality (0.054) - this node is a cross-community bridge._
- **Are the 33 inferred relationships involving `fluxipc_node_leaf()` (e.g. with `node_is_leaf()` and `tree_leaf()`) actually correct?**
  _`fluxipc_node_leaf()` has 33 INFERRED edges - model-reasoned connections that need verification._
- **Are the 34 inferred relationships involving `fluxipc_tree_root()` (e.g. with `register_endpoints()` and `test_call_success()`) actually correct?**
  _`fluxipc_tree_root()` has 34 INFERRED edges - model-reasoned connections that need verification._
- **Are the 26 inferred relationships involving `ut_capture_begin()` (e.g. with `test_call_success()` and `test_cmd_list_missing_server()`) actually correct?**
  _`ut_capture_begin()` has 26 INFERRED edges - model-reasoned connections that need verification._
- **Are the 26 inferred relationships involving `ut_capture_end()` (e.g. with `test_call_success()` and `test_cmd_list_missing_server()`) actually correct?**
  _`ut_capture_end()` has 26 INFERRED edges - model-reasoned connections that need verification._