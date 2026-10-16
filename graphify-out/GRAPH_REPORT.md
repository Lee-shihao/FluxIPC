# Graph Report - FluxIPC  (2026-10-16)

## Corpus Check
- 44 files · ~41,302 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 676 nodes · 1664 edges · 38 communities (35 shown, 3 thin omitted)
- Extraction: 63% EXTRACTED · 37% INFERRED · 0% AMBIGUOUS · INFERRED: 615 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `7720b009`
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
- [[_COMMUNITY_Community 24|Community 24]]
- [[_COMMUNITY_Community 25|Community 25]]
- [[_COMMUNITY_Community 26|Community 26]]
- [[_COMMUNITY_Community 27|Community 27]]
- [[_COMMUNITY_Community 28|Community 28]]
- [[_COMMUNITY_Community 29|Community 29]]
- [[_COMMUNITY_Community 30|Community 30]]
- [[_COMMUNITY_Community 31|Community 31]]
- [[_COMMUNITY_Community 32|Community 32]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 34|Community 34]]
- [[_COMMUNITY_Community 35|Community 35]]
- [[_COMMUNITY_Community 36|Community 36]]

## God Nodes (most connected - your core abstractions)
1. `fluxipc_node_leaf()` - 55 edges
2. `fluxipc_tree_root()` - 54 edges
3. `ut_conn_free()` - 36 edges
4. `do_http()` - 30 edges
5. `ut_capture_begin()` - 28 edges
6. `ut_capture_end()` - 28 edges
7. `resp_status()` - 25 edges
8. `dispatch()` - 22 edges
9. `fluxipc_http_request_t` - 22 edges
10. `tree_insert_path()` - 21 edges

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

## Communities (38 total, 3 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.22
Nodes (19): cJSON, fluxipc_method_t, fluxipc_node_t, fluxipc_build_manifest(), fluxipc_mask_to_method(), fluxipc_method_to_mask(), fluxipc_node_is_leaf(), fluxipc_node_method() (+11 more)

### Community 1 - "Community 1"
Cohesion: 0.10
Nodes (28): cmd_call(), cmd_list(), main(), print_help(), fluxipc_list_cb, http_get(), ipc(), main() (+20 more)

### Community 2 - "Community 2"
Cohesion: 0.05
Nodes (102): arg_spec_t, path_node_t, fluxipc_usage(), build_candidates(), builtin_generator(), fluxipc_node_t, cmd_cd(), cmd_help() (+94 more)

### Community 3 - "Community 3"
Cohesion: 0.15
Nodes (8): Exception, FluxIPC, FluxIPCError, fluxipc.py – Python client for the FluxIPC HTTP server.  Speaks plain HTTP/1.1 w, Return the generated OpenAPI 3.1 document., Raised for a non-2xx HTTP response (RFC 7807 problem+json)., Minimal REST client for a FluxIPC server., Return the leaf manifest for a container (all nodes support this).

### Community 4 - "Community 4"
Cohesion: 0.15
Nodes (23): Compile-Time Registration via ELF Section (FLUXIPC_STATIC), Six-Layer Bottom-Up Component Architecture, FluxIPC — Namespace-tree IPC library for Linux (CLAUDE.md), Global Server State (fluxipc_ctx_t *g_ctx), Server Lifecycle Flow (client_dispatch → server_init → register → poll → destroy), Zero External Dependencies Design Principle, Single-Threaded select() Event Loop, Codebase File Index (CODEBASE_INDEX.md) (+15 more)

### Community 5 - "Community 5"
Cohesion: 0.10
Nodes (50): fluxipc_stop(), fluxipc_event_t, fluxipc_http_request_t, fluxipc_http_response_t, config_log_level_http(), config_log_level_ipc(), config_max_clients_http(), config_max_clients_ipc() (+42 more)

### Community 7 - "Community 7"
Cohesion: 0.17
Nodes (10): API Reference, Client, Design Notes, File Locations, FluxIPC, HTTP Interface, Python Client, Server / tree (+2 more)

### Community 8 - "Community 8"
Cohesion: 0.16
Nodes (37): accept_http(), accept_unix(), fluxipc_conn_kind_t, fluxipc_conn_t, fluxipc_ctx_t, conn_alloc(), conn_close(), conn_poll() (+29 more)

### Community 9 - "Community 9"
Cohesion: 0.83
Nodes (3): install.sh script, install_fluxipc(), uninstall_fluxipc()

### Community 10 - "Community 10"
Cohesion: 0.18
Nodes (25): http_buf_t, add_operation(), buf_append(), buf_printf(), buf_reserve(), build_allow(), build_openapi(), build_response() (+17 more)

### Community 11 - "Community 11"
Cohesion: 0.10
Nodes (20): 1. 编译期 + 运行期双注册机制, 2. 符号链接 — 零配置客户端调用, 3. 命名空间树 — 像文件系统一样组织接口, 4. MCP Server — 一键接入 AI Agent, 5. 交互式 Shell — 带 Tab 补全的 IPC 调试器, FluxIPC：轻量级 Linux IPC 框架 — 像操作文件一样调用进程间函数，还能接入 AI Agent, 与其他 IPC 方案对比, 五种客户端调用方式对比 (+12 more)

### Community 12 - "Community 12"
Cohesion: 0.22
Nodes (26): fluxipc_method_t, fluxipc_node_t, dup_or_null(), node_alloc(), path_split(), set_full_path(), tree_destroy(), tree_find_child() (+18 more)

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
Cohesion: 0.08
Nodes (32): fluxipc_kv_t, fluxipc_find_content_length(), fluxipc_parse_query(), fluxipc_url_decode(), hexval(), main(), run_suite(), ut_cli() (+24 more)

### Community 17 - "Community 17"
Cohesion: 0.09
Nodes (76): fluxipc_dispatch_ipc(), fluxipc_node_leaf(), fluxipc_tree_root(), fluxipc_conn_t, send_unix_response(), sock_conn_process(), cJSON, fluxipc_conn_t (+68 more)

### Community 18 - "Community 18"
Cohesion: 0.16
Nodes (16): fluxipc_node_find(), fluxipc_node_mkdir(), fluxipc_event_t, fluxipc_http_request_t, fluxipc_http_response_t, fluxipc_node_t, count_cb(), http_handler() (+8 more)

### Community 19 - "Community 19"
Cohesion: 0.22
Nodes (16): fluxipc_destroy(), fluxipc_server_init(), sock_server_close(), cli_teardown(), core_teardown(), test_init_double(), test_init_http_port_in_use(), test_init_invalid() (+8 more)

### Community 20 - "Community 20"
Cohesion: 0.18
Nodes (13): fluxipc_poll(), fluxipc_http_request_t, fluxipc_http_response_t, cli_setup(), core_setup(), test_poll_and_stop(), http_setup(), sock_setup() (+5 more)

### Community 21 - "Community 21"
Cohesion: 0.14
Nodes (13): 1. 结论指标（实测）, 2. 工具链, 3. 目录结构, 4.1 分层, 4.2 cmocka 用法, 4.3 白盒可见性（零侵入为主）, 4.4 测试隔离接缝, 4. 测试策略 (+5 more)

### Community 22 - "Community 22"
Cohesion: 0.15
Nodes (13): API 参考, CLI, FluxIPC, HTTP 接口, Python 客户端, 交互式 shell, 客户端, 文件位置 (+5 more)

### Community 23 - "Community 23"
Cohesion: 0.25
Nodes (7): env.sh script, CMOCKA_PREFIX, FLUXIPC_TOOLS, LD_LIBRARY_PATH, MULL_FRONTEND, MULL_RUNNER, PATH

### Community 24 - "Community 24"
Cohesion: 0.33
Nodes (7): fluxipc_endpoint_t, fluxipc_leaf_cb, fluxipc_node_leaves(), fluxipc_register_all(), walk_leaves(), test_node_leaves_without_ctx(), test_register_all_table()

### Community 25 - "Community 25"
Cohesion: 0.50
Nodes (5): fluxipc_request_hook_fn, fluxipc_set_request_hooks(), main(), register_device(), register_endpoints()

### Community 26 - "Community 26"
Cohesion: 0.40
Nodes (5): Build, Install, Prerequisites, Quick Start, Run the Demo

### Community 27 - "Community 27"
Cohesion: 0.40
Nodes (5): Current quality gates, Integration tests (`tests/integration/`), Python client tests, Testing & Quality, Unit suites (cmocka, `tests/unit/`)

### Community 28 - "Community 28"
Cohesion: 0.40
Nodes (5): Python 客户端测试, 单元套件（cmocka，`tests/unit/`）, 当前质量门禁, 测试与质量, 集成测试（`tests/integration/`）

### Community 29 - "Community 29"
Cohesion: 0.40
Nodes (5): 依赖, 安装, 快速开始, 构建, 运行演示

### Community 30 - "Community 30"
Cohesion: 0.40
Nodes (5): fluxipc_ctx_t, fluxipc_http_request_t, fluxipc_http_response_t, ctx_alloc(), fluxipc_dispatch_http()

### Community 31 - "Community 31"
Cohesion: 0.50
Nodes (4): 1. Register endpoints, 3. Handler signatures, Lower-level node API, Server-Side Usage

### Community 32 - "Community 32"
Cohesion: 0.50
Nodes (4): 1. 注册端点, 3. 处理器签名, 更底层的节点 API, 服务端用法

### Community 34 - "Community 34"
Cohesion: 0.67
Nodes (3): CLI, Interactive shell, Local (Unix Socket) Interface

## Knowledge Gaps
- **107 isolated node(s):** `fluxipc_conn_kind_t`, `fluxipc_endpoint_t`, `cJSON`, `fluxipc_http_request_t`, `fluxipc_http_response_t` (+102 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **3 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `fluxipc_node_leaf()` connect `Community 17` to `Community 0`, `Community 2`, `Community 12`, `Community 18`, `Community 25`?**
  _High betweenness centrality (0.099) - this node is a cross-community bridge._
- **Why does `fluxipc_tree_root()` connect `Community 17` to `Community 0`, `Community 2`, `Community 18`, `Community 24`, `Community 25`?**
  _High betweenness centrality (0.063) - this node is a cross-community bridge._
- **Why does `register_endpoints()` connect `Community 25` to `Community 24`, `Community 17`, `Community 18`, `Community 5`?**
  _High betweenness centrality (0.040) - this node is a cross-community bridge._
- **Are the 51 inferred relationships involving `fluxipc_node_leaf()` (e.g. with `tree_leaf()` and `register_device()`) actually correct?**
  _`fluxipc_node_leaf()` has 51 INFERRED edges - model-reasoned connections that need verification._
- **Are the 52 inferred relationships involving `fluxipc_tree_root()` (e.g. with `register_endpoints()` and `test_call_success()`) actually correct?**
  _`fluxipc_tree_root()` has 52 INFERRED edges - model-reasoned connections that need verification._
- **Are the 34 inferred relationships involving `ut_conn_free()` (e.g. with `test_all_method_allow()` and `test_bad_request_line()`) actually correct?**
  _`ut_conn_free()` has 34 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `do_http()` (e.g. with `http_conn_process()` and `ut_conn_init()`) actually correct?**
  _`do_http()` has 2 INFERRED edges - model-reasoned connections that need verification._