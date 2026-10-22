# FluxIPC

FluxIPC 是一个面向 Linux 的轻量级 **命名空间树 IPC 库**。应用把库嵌入自身进程，按路径层级注册可调用的叶子节点，并通过 Unix domain socket 对外提供服务。没有独立守护进程，没有 HTTP 服务，也没有 JSON 层：一次调用就是位置参数 `argv` 进、原始字节出。

库本身**不创建任何线程**。由宿主应用决定把事件循环放在自己创建的线程里运行、接入已有调度器，还是手动驱动。

## 特性

- 层级端点命名空间（`/system/info`、`/devices/io/led/value` …）
- 基于路径注册，自动创建中间容器
- 兼容经典 `fluxipc_register(path, usage, slot_sz, handler, data)`
- `FLUXIPC_STATIC` 编译期端点注册
- 运行时增删节点；空祖先自动裁剪
- 12 字节紧凑线协议，基于 Unix socket
- 非阻塞、带缓冲、每连接一次请求/响应
- 宿主自有事件循环：
  - `fluxipc_run()`：在应用创建的线程中运行
  - `fluxipc_poll_timeout(ms)`：接入应用自己的调度器
  - `fluxipc_wakeup()` / `fluxipc_stop()`：任意线程安全调用
- 注册/删除与客户端调用均为线程安全
- 提供 CLI、交互式 shell、单元/集成测试与基准测试

## 快速开始

### 依赖

- GCC 或 Clang
- GNU make
- `libreadline`（仅交互式 CLI 使用；核心库不链接它）
- `libpthread`、`librt`（通常由 libc 提供）

```bash
# Debian / Ubuntu
sudo apt install build-essential libreadline-dev

# Fedora / RHEL
sudo dnf install gcc make readline-devel
```

### 构建

```bash
make -j$(nproc)
```

目标：

```bash
make lib        # 动态库：libfluxipc.so.1.0.0
make static     # 静态库：libfluxipc.a
make cli        # fluxipc-cli
make examples   # example_server
```

### 安装

```bash
sudo make install              # PREFIX=/usr
sudo make install PREFIX=/usr/local
```

### 运行演示

```bash
# 终端 1
./example_server

# 终端 2
fluxipc-cli example_server
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
fluxipc-cli example_server -i
```

## 服务端用法

```c
#include <fluxipc.h>

static int add_ipc(void *data, int argc, char **argv,
                   void *out, size_t cap, size_t *len)
{
    (void)data;
    int a = argc > 1 ? atoi(argv[1]) : 0;
    int b = argc > 2 ? atoi(argv[2]) : 0;
    int n = snprintf(out, cap, "%d", a + b);
    if (len) *len = (n > 0) ? (size_t)n : 0;
    return 0;
}

static void *serve(void *arg)
{
    (void)arg;
    fluxipc_run();          /* 在 fluxipc_stop() 后返回 */
    return NULL;
}

int main(void)
{
    int rc = fluxipc_server_init("myapp");
    if (rc < 0) return 1;

    fluxipc_register_method("/demo/add",
        &FLUXIPC_METHOD(add_ipc, "两数相加"));

    /* 方式 A：在应用自己创建的线程中运行事件循环。 */
    pthread_t th;
    pthread_create(&th, NULL, serve, NULL);

    /* ... 宿主应用自己的工作 ... */

    fluxipc_stop();
    pthread_join(th, NULL);
    fluxipc_destroy();
    return 0;
}
```

处理器参数：

- `argc` / `argv`：`argv[0]` 是端点路径，后面是调用参数
- `out` / `cap`：输出缓冲区（socket 服务端为 64 KiB）
- `out_len`：实际写入字节数

成功返回 `0`，失败返回负 errno。端点不存在返回 `-ENOENT`。

### 注册方式

```c
/* 路径注册；中间容器自动创建。 */
fluxipc_register_method("/module/sensor/status",
    &FLUXIPC_METHOD(status_ipc, "读取传感器"));

/* 带每叶子数据指针。 */
fluxipc_register_method("/devices/led/value",
    &FLUXIPC_METHOD_D(value_ipc, &led_ctx, "读写 LED 值"));

/* 经典 main 分支签名（slot_sz 保留但忽略）。 */
fluxipc_register("/legacy/ping", "Ping", 0, ping_ipc, NULL);

/* 静态表。 */
static const fluxipc_endpoint_t table[] = {
    { "/a", FLUXIPC_METHOD(a_ipc, "A") },
    { "/b", FLUXIPC_METHOD(b_ipc, "B") },
};
fluxipc_register_all(table, sizeof(table) / sizeof(table[0]));

/* 编译期注册（在 fluxipc_server_init 时应用）。 */
FLUXIPC_STATIC("/system/compiled", "编译期端点", 0, compiled_ipc, NULL);
```

重复注册同一路径会合并元数据：仍为空的 `summary`、`data` 会被填充；已有处理器不会被覆盖。

### 运行时增删

```c
fluxipc_node_t *root = fluxipc_tree_root();
fluxipc_node_t *dir  = fluxipc_node_mkdir(root, "dynamic");
fluxipc_node_leaf(dir, "leaf", &FLUXIPC_METHOD(fn, "动态叶子"));

fluxipc_node_remove(dir);   // 删除子树并裁剪空祖先
```

`fluxipc_node_find()`、`fluxipc_node_path()`、`fluxipc_node_name()`、
`fluxipc_node_method()`、`fluxipc_node_is_leaf()` 可用于查询。查找返回的是借用句柄，不要与删除操作并发使用。

## 线程 / 调度

```c
/* 专用宿主线程。 */
static void *serve(void *arg) { fluxipc_run(); return NULL; }
pthread_create(&th, NULL, serve, NULL);

/* 或接入自己的调度器。 */
for (;;) {
    int rc = fluxipc_poll_timeout(100);   /* ms；<0 无限，0 非阻塞 */
    if (rc < 0) break;
    /* 做其他周期性工作 */
}

fluxipc_wakeup();   /* 唤醒阻塞中的 poll */
fluxipc_stop();     /* 请求事件循环退出 */
pthread_join(th, NULL);
fluxipc_destroy();  /* 必须在 poll 线程退出后调用 */
```

保证：

- 库不创建线程。
- poll 线程运行期间可以从其他线程注册/删除端点。
- 同一时刻只允许一个线程 poll；并发 poll 返回 `-EBUSY`。
- `fluxipc_call()` 与 `fluxipc_list_endpoints()` 是线程安全的客户端接口。
- 处理器运行在 poll 线程上。
- 请求钩子可从任意线程替换；正在执行的请求保持其开始时的钩子对。

## 客户端 API

```c
char out[4096];
size_t len = 0;
char *argv[] = { "/demo/add", "7", "3" };

int rc = fluxipc_call(
    "/run/user/1000/myapp-fluxipc/myapp.sock",
    "/demo/add", 3, argv, out, sizeof(out), &len);
```

`fluxipc_call()` 每次调用打开独立连接，因此多线程可并发调用。

端点发现：

```c
static void on_leaf(const char *path, uint32_t id,
                    const char *usage, void *user)
{
    printf("%u\t%s\t%s\n", id, path, usage);
}

fluxipc_list_endpoints("myapp", on_leaf, NULL);
```

`LIST` 操作直接通过 Unix socket 返回紧凑文本清单
（`<id>\t<path>\t<summary>\n`）。

## CLI

```
fluxipc-cli <server>                          列出端点
fluxipc-cli <server> -l, --list               列出端点
fluxipc-cli <server> -c, --call <path> [args] 调用端点
fluxipc-cli <server> -i, --interactive        交互式 shell（Tab 补全）
```

交互式 shell 支持 `ls`、`cd`、`help`、`watch`、`reload`、相对路径，以及
`1:10` 这样的区间语法。

## 测试与质量

```bash
make ut          # cmocka 单元测试
make it          # 针对 example_server 的端到端测试
make test        # 两者都跑
make test-mem    # ASan/UBSan + LeakSanitizer
make bench       # p50/p95/p99、ops/s、RSS
```

基准测试参数：

```bash
BENCH_IPC_ITERS=20000 BENCH_THREADS=8 make bench
BENCH_MIN_IPC_OPS=10000 BENCH_MAX_P99_MS=1.0 make bench
```

## 文件位置

| 项目 | 路径 |
| --- | --- |
| 公共头文件 | `include/fluxipc.h` |
| 内部头文件 | `include/fluxipc_internal.h` |
| 动态库 | `libfluxipc.so.1.0.0` |
| 静态库 | `libfluxipc.a` |
| 运行时目录 | `/run/user/<uid>/<prog>-fluxipc/` |
| Socket | `/run/user/<uid>/<prog>-fluxipc/<prog>.sock` |
| 安装头文件 | `$(PREFIX)/include/fluxipc/fluxipc.h` |

## 源码结构

```
include/
  fluxipc.h              公共 API
  fluxipc_internal.h     线协议 + 内部类型
src/
  fluxipc_core.c         生命周期、注册、清单、分发、事件循环
  fluxipc_tree.c         命名空间树
  fluxipc_sock.c         Unix socket 传输 + 客户端
  fluxipc_conn.c         非阻塞连接池 / select 循环
  fluxipc_interactive.c  交互式 REPL（仅链接进 fluxipc-cli）
cli/
  fluxipc_cli.c          CLI 入口
tests/
  example_server.c       仅 IPC 的演示（宿主线程运行事件循环）
  unit/                  cmocka 套件
  integration/           端到端 IPC 测试
  bench/                 延迟 / 吞吐基线
```

## 设计说明

- **单 poll 线程、多注册线程。** 连接池由一个线程驱动；树由互斥锁保护，因此注册可以发生在任意线程。
- **每连接一次请求。** 状态机保持简单，客户端并行调用也很自然。
- **资源有界。** 32 个并发连接、每请求 512 KiB 上限、处理器输出 64 KiB、30 秒空闲超时。
- **热路径少分配。** 服务端复用一份 IPC 输出缓冲；连接缓冲按需增长。
- **文本清单。** 列表接口零依赖，同时易于查看和解析。

## 路线图

- 访问控制 / 对端凭据校验
- 针对阻塞处理器的可选工作线程池
- 请求/响应之外的事件订阅推送
- 线协议解析器 fuzz 目标
