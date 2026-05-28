# FluxIPC：一个轻量级 Linux 进程间通信框架，用文件路径的方式调用函数

> 通过 UNIX 路径命名空间 + 共享内存发现 + Unix Domain Socket 通信，让 IPC 调用像访问文件一样自然。

---

## 前言

在嵌入式 Linux 或系统服务开发中，进程间通信（IPC）一直是绕不开的话题。无论是 D-Bus、gRPC、还是手搓 Unix Socket，都或多或少面临以下痛点：

- **接口定义繁琐**：需要写 IDL、生成桩代码、维护接口文档
- **服务发现困难**：客户端需要硬编码服务地址，或者引入额外的名字服务
- **调试不便**：没有一个统一的命令行工具来快速测试接口
- **学习成本高**：引入一个 IPC 框架往往需要理解复杂的概念和配置

**FluxIPC** 就是为了解决这些问题而生的。

**GitHub 仓库：[https://github.com/Lee-shihao/FluxIPC](https://github.com/Lee-shihao/FluxIPC)**（如果觉得不错，欢迎点个 Star ⭐）

---

## 什么是 FluxIPC？

FluxIPC 是一个**轻量级的 Linux IPC 库**，核心设计理念只有三条：

1. **命名空间树** — 所有 IPC 端点按照 UNIX 文件路径的方式组织（`/module/sensor/status`、`/devices/stub/primary/value`）
2. **共享内存注册表** — 服务端通过 POSIX 共享内存暴露端点信息，客户端自动发现
3. **零配置调用** — 客户端通过符号链接直接调用，无需知道服务端地址

用一句话概括：**用文件系统的方式，调用另一个进程的函数**。

---

## 架构概览

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Server 进程                                                            │
│                                                                         │
│  ELF section "fluxipc_registry" ──►  namespace tree                     │
│  FLUXIPC_STATIC(...)                     /                              │
│                                        ├─ module/                       │
│  fluxipc_register(...)                 │   └─ sensor/                   │
│  (dynamic, runtime)                    │       ├─ status   ←── handler  │
│                                        │       └─ calibrate             │
│                                        └─ devices/                      │
│                                            └─ stub/xxx/                 │
│                                                ├─ name   ←── handler    │
│                                                └─ value  ←── handler    │
│                                                                         │
│  ┌─────────────────────────┐ ┌──────────────────────────────────────┐   │
│  │  Unix socket            │ │  POSIX 共享内存                      │   │
│  │  /run/<prog>-fluxipc/   │ │  /dev/shm/fluxipc.<prog>             │   │
│  │  <prog>.sock            │ │                                      │   │
│  └────────┬────────────────┘ └──────────────────────────────────────┘   │
└───────────┼─────────────────────────────────────────────────────────────┘
            │
            │  符号链接: /run/<prog>-fluxipc/<path> → 可执行文件
            │
┌───────────▼──────────────────────┐  ┌───────────────────────────────────┐
│  CLI / Client 进程               │  │  交互式 Shell                     │
│                                  │  │                                   │
│  argv[0] 通过符号链接解析        │  │  加载所有 /dev/shm/fluxipc.*      │
│  → 推导出 prog, socket, path     │  │  → 构建 Tab 补全表               │
│  → 通过 socket 发送请求          │  │  → readline + 命令补全            │
│  → 打印响应                      │  │  → 内联使用提示                   │
└──────────────────────────────────┘  └───────────────────────────────────┘
```

整个架构由三个核心组件构成：

| 组件 | 路径 | 用途 |
|------|------|------|
| Unix Domain Socket | `/run/<prog>-fluxipc/<prog>.sock` | 实际的数据通信通道 |
| POSIX 共享内存 | `/dev/shm/fluxipc.<prog>` | 端点注册表，包含路径、用法、ID |
| 符号链接 | `/run/<prog>-fluxipc/<path>` | 客户端发现和调用入口 |

---

## 设计亮点

### 1. 编译期 + 运行期双注册机制

FluxIPC 支持两种端点注册方式：

**编译期注册** — 通过 `FLUXIPC_STATIC` 宏，将端点的元数据写入 ELF 的 `fluxipc_registry` 段：

```c
static int ping_handler(void *data, int argc, char **argv,
                        void *out, size_t cap, size_t *len) {
    *len = snprintf(out, cap, "pong\n");
    return 0;
}
FLUXIPC_STATIC("/my/app/ping", "Respond with pong", 0, ping_handler, NULL);
```

**运行期注册** — 通过 `fluxipc_register()` API，在运行时动态添加端点：

```c
fluxipc_node_t *node = fluxipc_register(
    "/my/app/dynamic", "Dynamic endpoint", 0, my_handler, NULL);
```

这种设计非常适合嵌入式场景：固定的系统接口在编译期注册，而需要根据硬件配置动态生成的接口在运行期注册。

### 2. 符号链接 — 零配置客户端调用

这是 FluxIPC 最巧妙的设计。每个注册的端点都会在 `/run/<prog>-fluxipc/<path>` 创建一个符号链接，指向可执行文件本身。当通过符号链接启动程序时，`argv[0]` 中就编码了服务端身份和 IPC 路径：

```bash
# 直接通过符号链接调用 IPC 接口
/run/myapp-fluxipc/module/sensor/status

# 还可以创建自定义快捷方式
ln -s /run/myapp-fluxipc/module/sensor/status /usr/local/bin/sensor-status
sensor-status
```

无需配置服务地址，无需传递服务名，**像执行命令一样调用 IPC**。

### 3. 命名空间树 — 像文件系统一样组织接口

IPC 端点按路径层级组织，和文件系统有天然的对应关系：

```
/                              # 根节点（命名空间容器）
├─ system/                     # 系统相关
│  ├─ info                     # 进程信息
│  ├─ uptime                   # 运行时间
│  └─ shutdown                 # 关闭服务
├─ module/                     # 功能模块
│  ├─ sensor/                  # 传感器子系统
│  │  ├─ status                # 查询读数
│  │  ├─ calibrate             # 校准
│  │  └─ config                # 配置采样率
│  └─ motor/                   # 电机子系统
│     ├─ speed                 # 转速控制
│     ├─ position              # 位置查询
│     └─ stop                  # 急停
├─ config/                     # 配置管理
│  ├─ log_level                # 日志级别
│  └─ max_clients              # 最大客户端数
└─ devices/                    # 设备实例（动态注册）
   ├─ stub/primary/            # 虚拟打桩测试设备
   │  ├─ name
   │  ├─ value
   │  └─ reset
   └─ io/led/                  # LED 设备
      └─ ...
```

内部节点（如 `/module/sensor`）仅作为命名空间容器；只有叶子节点才绑定处理函数。移除叶子节点时会自动剪枝空的祖先节点。

### 4. 交互式 Shell — 带 Tab 补全的 IPC 调试器

FluxIPC 提供了一个功能完备的交互式 Shell：

```bash
$ fluxipc-cli --interactive

[fluxipc] />
[fluxipc] /> ls
  /system/info                             Show server process info
  /system/uptime                           Show server uptime
  /system/version                          Show server version and build date
  /module/sensor/status                    Query current sensor readings
  /module/motor/speed                      [<rpm>]  Get/set motor speed
  /config/log_level                        [<0-5>]  Get/set server log level
  /demo/echo                               <args...>  Echo back all arguments
  ...

[fluxipc] /> /module/s<TAB>        # 自动补全为 /module/sensor/
[fluxipc] /> /module/sensor/status  # 执行调用
temperature=23.5 humidity=58.2 sample_rate=10 ok=true

[fluxipc] /> cd /config            # 切换工作目录
[fluxipc] /config> log_level 3     # 支持相对路径调用
log_level=3
```

Shell 特性一览：
- **Tab 补全**：从共享内存加载所有端点的路径信息
- **内联用法提示**：输入路径但缺少必选参数时自动显示用法
- **`ls` 命令**：列出所有已注册的端点及用途说明
- **`cd` 命令**：切换当前工作路径，支持相对路径调用
- **`help <path>`**：查看单个端点的详细信息
- **`reload` 命令**：重新扫描共享内存，热更新端点列表

### 5. 通信协议 — 简洁高效的流式协议

所有通信走 Unix Domain Socket，协议格式极其简洁：

```
请求:
  fluxipc_req_hdr_t  (固定头部，包含 endpoint_id 和参数长度)
  NUL 分隔的参数字符串

响应:
  fluxipc_resp_hdr_t  (固定头部: status + data_len)
  data_len 字节的响应数据
```

没有复杂的序列化框架，没有 Protobuf 的依赖，纯粹的二进制定长头 + 变长数据，性能极佳。

---

## 快速上手

### 环境要求

- Linux 系统（依赖 `/dev/shm`、`/run`、Unix Socket）
- GCC 编译器
- GNU Readline 库

### 安装步骤

```bash
# 克隆仓库
git clone https://github.com/Lee-shihao/FluxIPC.git
cd FluxIPC

# 安装依赖（Debian/Ubuntu）
sudo apt install libreadline-dev

# 编译
make -j$(nproc)

# 安装到系统
sudo make install
```

### 编写第一个 IPC 服务

```c
#include <fluxipc/fluxipc.h>
#include <stdio.h>
#include <signal.h>

// 1. 定义处理函数
static int hello_handler(void *data, int argc, char **argv,
                         void *out, size_t cap, size_t *len) {
    const char *name = (argc > 1) ? argv[1] : "World";
    *len = snprintf(out, cap, "Hello, %s!\n", name);
    return 0;
}

// 2. 编译期注册
FLUXIPC_STATIC("/greet/hello", "<name>  Say hello", 0, hello_handler, NULL);

static volatile int quit = 0;
static void on_signal(int s) { (void)s; quit = 1; fluxipc_stop(); }

int main(int argc, char **argv) {
    // 3. 检查是否为符号链接调用（客户端模式）
    int rc = fluxipc_client_dispatch(argc, argv);
    if (rc >= 0) return rc;  // 如果是客户端调用，直接返回

    // 4. 初始化服务器
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    rc = fluxipc_server_init("myapp");
    if (rc < 0) {
        fprintf(stderr, "init failed: %d\n", rc);
        return 1;
    }

    printf("Server started. Try:\n");
    printf("  fluxipc-cli --server myapp /greet/hello\n");
    printf("  fluxipc-cli --interactive myapp\n");

    // 5. 事件循环
    while (!quit) fluxipc_poll(NULL);

    fluxipc_destroy();
    return 0;
}
```

编译运行：

```bash
# 编译（注意需要 --export-dynamic）
gcc -o myapp myapp.c $(pkg-config --cflags --libs fluxipc) -Wl,--export-dynamic

# 启动服务
./myapp

# 另一个终端中调用
fluxipc-cli --server myapp /greet/hello FluxIPC
# 输出: Hello, FluxIPC!

# 或者通过符号链接
/run/myapp-fluxipc/greet/hello FluxIPC
# 输出: Hello, FluxIPC!
```

---

## 源码结构

了解源码结构有助于深入学习或定制：

```
FluxIPC/
├── include/
│   ├── fluxipc.h           # 公共 API 头文件
│   └── fluxipc_internal.h  # 内部数据结构
├── src/
│   ├── fluxipc_core.c      # 库入口、生命周期管理
│   ├── fluxipc_tree.c      # 命名空间树（插入/查找/删除/剪枝）
│   ├── fluxipc_shm.c       # POSIX 共享内存注册表
│   ├── fluxipc_sock.c      # Unix Socket 服务端/客户端通信
│   ├── fluxipc_symlink.c   # 符号链接管理
│   └── fluxipc_interactive.c # 交互式 Shell（readline）
├── cli/
│   └── fluxipc_cli.c       # CLI 工具入口
├── tests/
│   └── example_server.c    # 完整的示例服务（含多子系统演示）
├── packaging/
│   └── install.sh          # 打包安装脚本
├── Makefile                # 构建系统
└── README.md
```

模块划分清晰，每个 `.c` 文件职责单一，非常适合阅读和学习。

---

## 适用场景

| 场景 | 说明 |
|------|------|
| **嵌入式 Linux 系统** | 轻量无依赖，适合资源受限的设备 |
| **系统守护进程** | 提供结构化的 IPC 接口，便于运维调试 |
| **多进程应用** | 用文件路径组织接口，结构清晰易维护 |
| **机器人/工控** | 传感器、电机、IO 设备的统一管理接口 |
| **快速原型开发** | 几行代码就能暴露 IPC 接口，无需 IDL |

---

## 与其他 IPC 方案对比

| 特性 | FluxIPC | D-Bus | Unix Socket (裸写) | gRPC |
|------|---------|-------|---------------------|------|
| 接口定义 | 路径字符串 | XML 描述 | 自定义协议 | Protobuf |
| 服务发现 | 共享内存自动发现 | 总线守护进程 | 手动配置 | 额外服务 |
| CLI 调试 | 内置交互式 Shell | dbus-send | netcat/自写 | grpcurl |
| 依赖 | libreadline | libdbus + daemon | 无 | 重量级 |
| 学习成本 | 低 | 中 | 高（需自己造轮子） | 中高 |
| 动态注册 | ✅ | ✅ | 需自己实现 | ✅ |

---

## 写在最后

FluxIPC 是一个**小而美**的项目——它没有宏大的目标，只专注于做好一件事：**让 Linux 进程间通信变得简单、直观、易调试**。

如果你正在开发一个需要多进程协作的 Linux 应用，或者厌倦了 D-Bus 的复杂配置，不妨试试 FluxIPC。

> **GitHub 仓库：[https://github.com/Lee-shihao/FluxIPC](https://github.com/Lee-shihao/FluxIPC)**
>
> 如果这个项目对你有帮助，欢迎 **Star ⭐** 支持一下作者！

```bash
git clone https://github.com/Lee-shihao/FluxIPC.git
```

---

*本文由 Claude Code 辅助撰写，基于 FluxIPC 项目源码分析。*
