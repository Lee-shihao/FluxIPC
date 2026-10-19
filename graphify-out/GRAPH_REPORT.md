# Graph Report - FluxIPC  (2026-10-19)

## Corpus Check
- 45 files · ~54,346 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 813 nodes · 2057 edges · 28 communities (25 shown, 3 thin omitted)
- Extraction: 65% EXTRACTED · 35% INFERRED · 0% AMBIGUOUS · INFERRED: 722 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `134650dc`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 1|Community 1]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 10|Community 10]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 18|Community 18]]
- [[_COMMUNITY_Community 19|Community 19]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 21|Community 21]]
- [[_COMMUNITY_Community 22|Community 22]]
- [[_COMMUNITY_Community 23|Community 23]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 35|Community 35]]
- [[_COMMUNITY_Community 36|Community 36]]

## God Nodes (most connected - your core abstractions)
1. `fluxipc_tree_root()` - 64 edges
2. `fluxipc_node_leaf()` - 64 edges
3. `ut_conn_free()` - 37 edges
4. `do_http()` - 31 edges
5. `ut_capture_begin()` - 29 edges
6. `ut_capture_end()` - 29 edges
7. `resp_status()` - 25 edges
8. `main()` - 23 edges
9. `dispatch()` - 22 edges
10. `fluxipc_node_t` - 21 edges

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

## Communities (28 total, 3 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.05
Nodes (90): fluxipc_endpoint_t, fluxipc_entry_t, fluxipc_handler_fn, fluxipc_leaf_cb, fluxipc_request_hook_fn, fluxipc_schema_type_t, bind_schema_fields(), cJSON (+82 more)

### Community 1 - "Community 1"
Cohesion: 0.12
Nodes (28): cmd_call(), cmd_list(), main(), print_help(), print_json_doc(), fluxipc_list_cb, fluxipc_list_endpoints(), fluxipc_conn_t (+20 more)

### Community 2 - "Community 2"
Cohesion: 0.05
Nodes (103): arg_spec_t, path_node_t, fluxipc_usage(), build_candidates(), builtin_generator(), cmd_cd(), cmd_help(), cmd_ls() (+95 more)

### Community 3 - "Community 3"
Cohesion: 0.15
Nodes (8): Exception, FluxIPC, FluxIPCError, fluxipc.py – Python client for the FluxIPC HTTP server.  Speaks plain HTTP/1.1 w, Return the generated OpenAPI 3.1 document., Raised for a non-2xx HTTP response (RFC 7807 problem+json)., Minimal REST client for a FluxIPC server., Return the leaf manifest for a container (all nodes support this).

### Community 4 - "Community 4"
Cohesion: 0.15
Nodes (23): Compile-Time Registration via ELF Section (FLUXIPC_STATIC), Six-Layer Bottom-Up Component Architecture, FluxIPC — Namespace-tree IPC library for Linux (CLAUDE.md), Global Server State (fluxipc_ctx_t *g_ctx), Server Lifecycle Flow (client_dispatch → server_init → register → poll → destroy), Zero External Dependencies Design Principle, Single-Threaded select() Event Loop, Codebase File Index (CODEBASE_INDEX.md) (+15 more)

### Community 5 - "Community 5"
Cohesion: 0.07
Nodes (64): fluxipc_stop(), bool_from_string(), cJSON, fluxipc_json_request_t, fluxipc_json_response_t, errno_from_status(), fluxipc_json_bool(), fluxipc_json_double() (+56 more)

### Community 7 - "Community 7"
Cohesion: 0.05
Nodes (37): 1. Register endpoints, 3. Handler signatures, API Reference, Build, CLI, Client, Concurrency & control, Current quality gates (+29 more)

### Community 8 - "Community 8"
Cohesion: 0.14
Nodes (41): accept_http(), accept_unix(), fluxipc_conn_kind_t, fluxipc_conn_t, fluxipc_ctx_t, conn_alloc(), conn_close(), conn_poll() (+33 more)

### Community 9 - "Community 9"
Cohesion: 0.83
Nodes (3): install.sh script, install_fluxipc(), uninstall_fluxipc()

### Community 10 - "Community 10"
Cohesion: 0.20
Nodes (23): http_buf_t, add_operation(), buf_append(), buf_printf(), buf_reserve(), build_allow(), build_openapi(), build_response() (+15 more)

### Community 11 - "Community 11"
Cohesion: 0.10
Nodes (20): 1. 编译期 + 运行期双注册机制, 2. 符号链接 — 零配置客户端调用, 3. 命名空间树 — 像文件系统一样组织接口, 4. MCP Server — 一键接入 AI Agent, 5. 交互式 Shell — 带 Tab 补全的 IPC 调试器, FluxIPC：轻量级 Linux IPC 框架 — 像操作文件一样调用进程间函数，还能接入 AI Agent, 与其他 IPC 方案对比, 五种客户端调用方式对比 (+12 more)

### Community 12 - "Community 12"
Cohesion: 0.16
Nodes (37): cJSON, fluxipc_method_t, fluxipc_node_t, fluxipc_schema_field_t, dup_or_null(), node_alloc(), node_schema_fields_build(), node_schema_fields_free() (+29 more)

### Community 13 - "Community 13"
Cohesion: 0.18
Nodes (9): Architecture, Codebase Navigation, Commands, Component layers (bottom-up), Conventions, graphify, Key global state, Lifecycle flow (+1 more)

### Community 14 - "Community 14"
Cohesion: 0.73
Nodes (5): check(), expect_error(), fail(), main(), ok()

### Community 15 - "Community 15"
Cohesion: 0.40
Nodes (4): Codebase Index, Config, Docs, Source

### Community 16 - "Community 16"
Cohesion: 0.07
Nodes (32): fluxipc_kv_t, fluxipc_find_content_length(), fluxipc_parse_query(), fluxipc_url_decode(), hexval(), main(), run_suite(), ut_cli() (+24 more)

### Community 17 - "Community 17"
Cohesion: 0.10
Nodes (75): fluxipc_node_leaf(), fluxipc_tree_root(), fluxipc_call_json(), sock_conn_process(), fluxipc_conn_t, fluxipc_http_request_t, fluxipc_http_response_t, fluxipc_http_request_t (+67 more)

### Community 18 - "Community 18"
Cohesion: 0.17
Nodes (27): http_body(), http_get(), http_json(), http_raw(), http_request(), http_status(), ipc(), json_call() (+19 more)

### Community 19 - "Community 19"
Cohesion: 0.20
Nodes (24): bench_http_seq(), bench_ipc_json_pos_seq(), bench_ipc_json_seq(), bench_ipc_seq(), bench_parallel(), env_long(), finish(), http_get_once() (+16 more)

### Community 20 - "Community 20"
Cohesion: 0.12
Nodes (23): fluxipc_poll(), fluxipc_http_request_t, fluxipc_http_response_t, cli_setup(), cli_teardown(), test_poll_no_descriptors(), core_setup(), core_teardown() (+15 more)

### Community 21 - "Community 21"
Cohesion: 0.12
Nodes (15): 1. 结论指标（实测）, 2. 工具链, 3. 目录结构, 4.1 分层, 4.2 cmocka 用法, 4.3 白盒可见性（零侵入为主）, 4.4 测试隔离接缝, 4. 测试策略 (+7 more)

### Community 22 - "Community 22"
Cohesion: 0.05
Nodes (37): 1. 注册端点, 3. 处理器签名, API 参考, CLI, FluxIPC, HTTP 接口, JSON Schema 辅助宏, JSON 处理器辅助 (+29 more)

### Community 23 - "Community 23"
Cohesion: 0.25
Nodes (7): env.sh script, CMOCKA_PREFIX, FLUXIPC_TOOLS, LD_LIBRARY_PATH, MULL_FRONTEND, MULL_RUNNER, PATH

## Knowledge Gaps
- **128 isolated node(s):** `fluxipc_conn_kind_t`, `fluxipc_endpoint_t`, `fluxipc_handler_fn`, `fluxipc_entry_t`, `fluxipc_kv_t` (+123 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **3 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `fluxipc_node_leaf()` connect `Community 17` to `Community 0`, `Community 2`, `Community 12`?**
  _High betweenness centrality (0.099) - this node is a cross-community bridge._
- **Why does `fluxipc_tree_root()` connect `Community 17` to `Community 0`, `Community 2`?**
  _High betweenness centrality (0.065) - this node is a cross-community bridge._
- **Why does `fluxipc_call()` connect `Community 1` to `Community 2`, `Community 17`, `Community 18`, `Community 19`?**
  _High betweenness centrality (0.055) - this node is a cross-community bridge._
- **Are the 62 inferred relationships involving `fluxipc_tree_root()` (e.g. with `register_endpoints()` and `test_call_json_success()`) actually correct?**
  _`fluxipc_tree_root()` has 62 INFERRED edges - model-reasoned connections that need verification._
- **Are the 60 inferred relationships involving `fluxipc_node_leaf()` (e.g. with `tree_leaf()` and `register_device()`) actually correct?**
  _`fluxipc_node_leaf()` has 60 INFERRED edges - model-reasoned connections that need verification._
- **Are the 35 inferred relationships involving `ut_conn_free()` (e.g. with `test_all_method_allow()` and `test_bad_request_line()`) actually correct?**
  _`ut_conn_free()` has 35 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `do_http()` (e.g. with `http_conn_process()` and `ut_conn_init()`) actually correct?**
  _`do_http()` has 2 INFERRED edges - model-reasoned connections that need verification._