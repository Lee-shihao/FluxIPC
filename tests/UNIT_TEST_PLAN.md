# FluxIPC 单元测试（UT）说明与实施方案

> 本文档描述 FluxIPC 当前实际使用的测试体系：**cmocka**（单元测试框架）、
> **Mull**（变异测试）、**gcovr**（覆盖率）、**ASan/UBSan**（内存与未定义行为）。
>
> 目标：真实约束每个模块的语义与错误分支——行/分支覆盖率达标，
> 并以**变异分数 > 70%** 作为正确性证据。

---

## 1. 结论指标（实测）

| 指标 | 门槛 | 实测 | 结果 |
|------|------|------|------|
| 单元用例 | — | **170 个**，全部通过（约 0.6s） | ✅ |
| 行覆盖率（src/） | ≥ 90% | **95.7%** | ✅ |
| 分支覆盖率（src/） | ≥ 78% | **78.2%** | ✅ |
| 变异分数 | **> 70%** | **75–76%**（`MUT_SCORE_MIN` 默认 70） | ✅ |
| ASan + UBSan | 无错误/无泄漏 | 干净 | ✅ |

逐文件覆盖率：

| 文件 | 行 | 分支 |
|------|----|------|
| `fluxipc_util.c` | 100.0% | 91.9% |
| `fluxipc_tree.c` | 100.0% | 87.2% |
| `fluxipc_core.c` | 98.9% | 81.2% |
| `fluxipc_http.c` | 98.2% | 78.8% |
| `fluxipc_conn.c` | 97.3% | 78.6% |
| `fluxipc_sock.c` | 96.8% | 74.6% |
| `fluxipc_interactive.c` | 92.0% | 74.6% |

---

## 2. 工具链

全部工具解包到仓库内 `.tools/`，**不需要 root**：

| 工具 | 版本 | 用途 |
|------|------|------|
| [cmocka](https://cmocka.org/) | 1.1.5 | 单元测试框架（fixture / 断言 / 分组） |
| [Mull](https://mull.readthedocs.io/) | 0.34.1 (`mull-14`) | 基于 clang-14 插件的变异测试 |
| gcovr | 5.0 | 覆盖率报告与阈值门禁 |
| lcov | 1.15 | 备用覆盖率工具（`genhtml`） |
| AddressSanitizer / UBSan | gcc 11 / clang 14 | 内存错误、未定义行为、泄漏检测 |

安装（一次性）：

```bash
make tools            # 等价于 tests/tools/bootstrap.sh
```

`bootstrap.sh` 通过 `apt-get download` 拉取 `mull-14`、`libcmocka-dev`、
`libcmocka0`、`lcov` 四个 `.deb`，用 `dpkg-deb -x` 解包到 `.tools/`。

---

## 3. 目录结构

```
tests/
  unit/
    ut_main.c          cmocka 入口（顺带把扫描目录隔离到 /tmp）
    ut_common.h/.c     共享：server fixture、fake conn、输出捕获、pump 线程
    test_protocol.c    IPC 12 字节线协议布局
    test_util.c        url_decode / parse_query / find_content_length
    test_tree.c        命名空间树
    test_core.c        生命周期 / 注册 / manifest / dispatch / hooks
    test_sock.c        sock_conn_process / 客户端 / 假服务器
    test_conn.c        连接池 / 读写状态机（socketpair）
    test_http.c        HTTP 解析 / 路由 / 响应 / OpenAPI
    test_interactive.c 白盒：include ../../src/fluxipc_interactive.c
    test_cli.c         白盒：include ../../cli/fluxipc_cli.c
  integration/main.c   端到端（fork example_server，IPC + HTTP）
  tools/
    bootstrap.sh        安装工具链
    env.sh              PATH / LD_LIBRARY_PATH
    coverage.sh         gcovr 报告 + 阈值门禁
    mutate.sh           Mull 构建 + 分数门禁
```

---

## 4. 测试策略

### 4.1 分层

| 层 | 手段 | 覆盖对象 |
|----|------|---------|
| L0 纯函数 | 直接调用 | util / tree / mask / manifest |
| L1 处理器白盒 | 内存 `fluxipc_conn_t` 直接驱动 `sock_conn_process` / `http_conn_process` | 协议解析、路由、响应构造（无网络、确定） |
| L2 组件 | 真实 Unix socket + 事件循环 pump 线程 | 客户端、连接状态机 |
| L3 端到端 | fork `example_server` | IPC + HTTP 全链路 |
| L4 健壮性 | ASan/UBSan、假服务器（坏 magic / 截断 / RST）、边界输入 | 错误分支 |

### 4.2 cmocka 用法

- 每个模块一个 `int ut_xxx(void)`，内部 `cmocka_run_group_tests()`。
- 需要 fresh server 的用例用 `cmocka_unit_test_setup_teardown(fn, setup, teardown)`，
  保证断言失败也会执行 teardown（避免全局 `g_ctx` 污染后续用例）。
- 客户端阻塞调用需要事件循环：`ut_server_pump_start()` 在辅助线程里跑
  `fluxipc_poll()`。
- 校验打印型函数/响应：`ut_capture_begin()/ut_capture_end()` 重定向 fd 1/2。
- 校验 HTTP 响应字节：直接读 fake `conn.wbuf` / `conn.wlen`（注意不要假设 NUL 结尾）。

### 4.3 白盒可见性（零侵入为主）

- `sock_conn_process` / `http_conn_process` 已导出，配合栈上 fake 连接即可。
- `fluxipc_interactive.c`、`cli/fluxipc_cli.c` 由测试 TU 直接 `#include`，从而可
  直接调用 static 函数；Makefile 的 UT 链接列表**排除** `src/fluxipc_interactive.c`
  以避免重复符号。
- 仅新增一个功能开关：`conn.c` 在 `-DFLUXIPC_TESTING` 下导出
  `fluxipc_ut_conn()/fluxipc_ut_conn_alloc()/fluxipc_ut_conn_read/write()`，
  用于精确驱动连接状态机。

### 4.4 测试隔离接缝

`load_all_endpoints()` 读取环境变量 `FLUXIPC_SCAN_DIR` 覆盖扫描目录（默认仍为
`/run/user/<uid>`）。UT 入口把它指向 `/tmp/fluxipc-ut-scan`，于是：
- 用例不会误连到其它进程/残留的 server；
- 并行变异测试的不同 worker 互不干扰；
- socket 路径同样从扫描根派生，保持自洽。

---

## 5. 命令

```bash
make tools        # 一次性安装 cmocka / Mull / lcov 到 .tools/
make ut           # cmocka 单元测试
make it           # 端到端集成测试（fork example_server）
make test         # ut + it
make cov          # 插桩构建 + 跑 ut/it + gcovr 报告，行/分支阈值门禁
make mutate       # Mull 变异测试 + 分数门禁
make ut-asan      # AddressSanitizer + UBSan（含泄漏检测）
```

可调参数：

```bash
COV_LINE_MIN=90 COV_BRANCH_MIN=78
MUT_SCORE_MIN=70 MUT_WORKERS=4
```

产物：`build/coverage/`（HTML/XML/summary）、`build/mutation/`。

---

## 6. 变异测试（Mull）

- 编译每个 TU **独立** clang 调用（`-grecord-command-line`），否则 Mull 无法
  解析多输入命令行。
- 启用 `cxx_default + cxx_logical + cxx_remove_negation`。
- `excludePaths` 排除 `tests/`、`cli/`。
- **Oracle 增强**：变异构建链接 ASan+UBSan（`detect_leaks=0`，因为 Mull runtime
  自身分配会被误报为泄漏）。越界/UB/崩溃即视为 killed。
- 分数 = killed / (killed + survived)，由 `--mutation-score-threshold` 直接门禁。

已知无谓/难以杀死的存活项多为等价变异（例如 `while (line < end)` 的 `<→<=`，
末次迭代无可观察差异；`sizeof(x)-1` 之类的常量折叠）。这些不影响功能正确性，
保留在报告中供人工审阅。

---

## 7. 过程中发现并修复的真实缺陷

1. **`build_candidates()` 栈释放后使用**（`src/fluxipc_interactive.c`）：
   `partial` 指向两个分支内各自声明的局部 `buf`，离开作用域后仍被
   `strlen(partial)` 读取。ASan 报 `stack-use-after-scope`。修复：把 `buf`
   提升到函数作用域，使 `partial` 在整个函数内有效。

这正是「真实测试语义而非为过用例而过」的体现——该缺陷在常规测试下不会暴露，
只有 ASan + 补全（Tab）路径的组合才能触发。

---

## 8. 后续可提升项

- 将 interactive 剩余 ~200 个存活变异中属于错误处理/内存分配的路径，用
  malloc 故障注入（`-Wl,--wrap=malloc`）进一步杀死。
- 为 `util` / HTTP parser 增加 libFuzzer 目标（固定种子 property test 已在计划中）。
- 把 `make cov` / `make mutate` 接入 CI，并在 PR 上只跑受影响文件的变异子集。
