# FluxIPC 测试说明

> FluxIPC 现在只保留 Unix-socket IPC：没有 HTTP、没有 JSON、没有 cJSON 依赖。
> 本文档描述当前测试体系：cmocka 单元测试、端到端 IPC 集成测试、
> ASan/UBSan/LeakSanitizer、gcovr 覆盖率与 Mull 变异测试。

---

## 1. 测试入口

```bash
make ut          # cmocka 单元套件（纯，无需外部服务器）
make it          # fork example_server 的端到端 IPC 套件
make test        # ut + it
make test-mem    # 单元 + 集成 ASan/UBSan/LeakSanitizer
make it-asan     # 插桩服务端 + 集成套件
make bench       # 延迟/吞吐/RSS 基线
make cov         # 覆盖率报告与阈值门禁
make mutate      # Mull 变异测试与分数门禁
```

---

## 2. 目录结构

```
tests/
  unit/
    ut_main.c          cmocka 入口（隔离扫描目录到 /tmp）
    ut_common.h/.c     共享：server fixture、fake conn、输出捕获、pump 线程
    test_protocol.c    IPC 12 字节线协议布局
    test_tree.c        命名空间树 / 合并 / 销毁
    test_core.c        生命周期 / 注册 / 文本 manifest / dispatch / hooks /
                       宿主线程调度 / 单 poller 约束 / 并发注册
    test_sock.c        sock_conn_process / 客户端 / 假服务器
    test_conn.c        连接池 / 读写状态机（socketpair）
    test_interactive.c 白盒：include ../../src/fluxipc_interactive.c
    test_cli.c         白盒：include ../../cli/fluxipc_cli.c
  integration/main.c   端到端 IPC：调用、参数、列表、并发、大负载、优雅退出
  bench/bench.c        延迟/吞吐/RSS 基线（fluxipc-bench）
  tools/
    bootstrap.sh        安装工具链
    env.sh              PATH / LD_LIBRARY_PATH
    coverage.sh         gcovr 报告 + 阈值门禁
    mutate.sh           Mull 构建 + 分数门禁
```

---

## 3. 测试策略

| 层 | 手段 | 覆盖对象 |
|----|------|---------|
| L0 纯函数 | 直接调用 | 树、路径查找、manifest 文本构造 |
| L1 处理器白盒 | 内存 `fluxipc_conn_t` 驱动 `sock_conn_process` | 线协议解析、分发、响应构造 |
| L2 组件 | 真实 Unix socket + 事件循环 pump 线程 | 客户端、连接池、读写状态机 |
| L3 端到端 | fork `example_server` | 公共 IPC API、并发、大负载、优雅退出 |
| L4 健壮性 | ASan/UBSan/LSan、假服务器、畸形协议 | 错误分支 / 内存安全 |
| L5 线程模型 | 宿主线程运行 `fluxipc_run()`、并发注册、单 poller | 外部调度能力 |

### cmocka 约定

- 每个模块一个 `int ut_xxx(void)`，内部 `cmocka_run_group_tests()`。
- 需要 fresh server 的用例使用
  `cmocka_unit_test_setup_teardown(fn, setup, teardown)`，断言失败也会执行
  teardown，避免全局 `g_ctx` 污染后续用例。
- 阻塞客户端调用需要事件循环：`ut_server_pump_start()` 在辅助线程里跑
  `fluxipc_poll()`；`ut_server_pump_stop()` 用 `fluxipc_stop()` 唤醒并 join。
- 校验打印型函数：`ut_capture_begin()/ut_capture_end()` 重定向 fd 1/2。
- 校验响应字节：直接读 fake `conn.wbuf` / `conn.wlen`（不要假设 NUL 结尾）。

### 白盒可见性

- `sock_conn_process` 已导出，配合栈上 fake 连接即可测试协议。
- `fluxipc_interactive.c`、`cli/fluxipc_cli.c` 由测试 TU 直接 `#include`，
  可直接调用 static 函数；`fluxipc_interactive.c` 只属于 CLI，不在 `LIB_SRCS`
  中，因此不会与 UT 链接列表重复。
- `-DFLUXIPC_TESTING` 下 `conn.c` 额外导出
  `fluxipc_ut_conn()` / `fluxipc_ut_conn_alloc()` /
  `fluxipc_ut_conn_read()` / `fluxipc_ut_conn_write()`，用于精确驱动连接状态机。

### 测试隔离

`load_all_endpoints()` 读取环境变量 `FLUXIPC_SCAN_DIR` 覆盖扫描目录（默认
`/run/user/<uid>`）。UT 入口把它指向 `/tmp/fluxipc-ut-scan`，避免并行测试或
残留服务器互相干扰。

---

## 4. 关键覆盖点

### 线协议

- 12 字节请求/响应头大小与字段偏移
- `FLUXIPC_OP_CALL` / `FLUXIPC_OP_LIST`
- 错误 magic、超长 path、超长 arg、未知 op、path 内嵌 NUL
- 参数数量上限 `FLUXIPC_MAX_ARGS`

### 命名空间树

- 容器/叶子创建、重复名、叶子上再建子节点
- 路径注册自动建中间容器、重复斜杠、尾部斜杠回滚
- 删除叶子/子树并裁剪空祖先
- 字符串所有权与合并语义

### 文本 manifest

- `<id>\t<path>\t<summary>\n` 每行恰好两个分隔符
- 用户 summary 中的 tab/newline 被替换为空格
- 空子树返回空字符串而不是 NULL
- `fluxipc_list_endpoints()` 在线解析并回调

### 生命周期与线程

- 重复 init / init 参数校验
- `fluxipc_run()` 在宿主线程运行，`fluxipc_stop()` 正常退出
- `fluxipc_poll_timeout(0)` 非阻塞
- 第二个并发 poll 返回 `-EBUSY`
- poll 线程运行期间从其他线程注册 50 个端点并成功列出
- `fluxipc_wakeup()` 不停止循环
- socket 权限 0666、退出后 socket 被删除

### 端到端

- `/system/info`、`/system/version`
- 算术参数与返回
- 未知路径/容器返回 `-ENOENT`
- 32 KiB 单参数大负载
- 8 线程 × 100 次并发调用
- `/system/shutdown` 后服务端退出码为 0

---

## 5. 基准

`make bench` 覆盖：

- 顺序 IPC（`/system/info`）
- 顺序 IPC（算术端点）
- 4 线程并行 IPC

可用环境变量调整：

```bash
BENCH_IPC_ITERS=20000 BENCH_THREADS=8 BENCH_WARMUP=500 make bench
BENCH_MIN_IPC_OPS=10000 BENCH_MAX_P99_MS=1.0 make bench
```

输出包括 p50/p95/p99、平均延迟、ops/s、错误数和服务端 RSS。

---

## 6. 工具链

全部工具解包到仓库内 `.tools/`，不需要 root：

| 工具 | 用途 |
|------|------|
| cmocka | 单元测试框架 |
| Mull (`clang-14`) | 变异测试 |
| gcovr | 覆盖率报告与阈值门禁 |
| AddressSanitizer / UBSan / LSan | 内存/未定义行为/泄漏检测 |

```bash
make tools            # 安装工具链
```

覆盖率门槛：`COV_LINE_MIN`（默认 90）、`COV_BRANCH_MIN`（默认 75）。
变异分数门槛：`MUT_SCORE_MIN`（默认 70）。
