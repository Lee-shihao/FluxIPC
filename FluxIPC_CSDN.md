# FluxIPC：轻量级 Linux IPC 框架 — 像操作文件一样调用进程间函数，还能接入 AI Agent

> 通过 UNIX 路径命名空间 + 共享内存发现 + Unix Domain Socket 通信，让 IPC 调用像访问文件一样自然。
> 内置 MCP（Model Context Protocol）服务，一键将所有 IPC 接口暴露给 Claude、Cursor 等 AI 客户端调用。

---

## 前言

在嵌入式 Linux 或系统服务开发中，进程间通信（IPC）一直是绕不开的话题。无论是 D-Bus、gRPC、还是手搓 Unix Socket，都或多或少面临以下痛点：

- **接口定义繁琐**：需要写 IDL、生成桩代码、维护接口文档
- **服务发现困难**：客户端需要硬编码服务地址，或者引入额外的名字服务
- **调试不便**：没有一个统一的命令行工具来快速测试接口
- **AI 集成门槛高**：想让 AI Agent 调用你的系统接口，需要额外开发 MCP Server 或 REST API

**FluxIPC** 就是为了解决这些问题而生的。

**GitHub 仓库：[https://github.com/Lee-shihao/FluxIPC](https://github.com/Lee-shihao/FluxIPC)**（如果觉得不错，欢迎点个 Star ⭐）

---

## 什么是 FluxIPC？

FluxIPC 是一个**轻量级的 Linux IPC 库**，核心设计理念只有四条：

1. **命名空间树** — 所有 IPC 端点按照 UNIX 文件路径的方式组织（`/module/sensor/status`、`/devices/stub/primary/value`）
2. **共享内存注册表** — 服务端通过 POSIX 共享内存暴露端点信息，客户端自动发现
3. **零配置调用** — 客户端通过符号链接直接调用，无需知道服务端地址
4. **原生 MCP 支持** — 内置 HTTP MCP Server，一键将所有 IPC 接口暴露为 AI 可调用的 Tools

用一句话概括：**用文件路径的方式，调用另一个进程的函数，还可以直接接入 AI Agent**。

---

## 架构概览

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Server 进程                                                             │
│                                                                          │
│  ELF section "fluxipc_registry" ──►  namespace tree                      │
│  FLUXIPC_STATIC(...)                     /                               │
│                                        ├─ module/                        │
│  fluxipc_register(...)                 │   └─ sensor/                    │
│  (dynamic, runtime)                    │       ├─ status   ←── handler   │
│                                        │       └─ calibrate              │
│                                        └─ devices/                       │
│                                            └─ stub/xxx/                  │
│                                                ├─ name   ←── handler     │
│                                                └─ value  ←── handler     │
│                                                                          │
│  ┌──────────────────────┐ ┌──────────────────────────────────────────┐   │
│  │  Unix socket         │ │  POSIX 共享内存                          │   │
│  │  /run/<prog>-fluxipc/│ │  /dev/shm/fluxipc.<prog>                 │   │
│  │  <prog>.sock         │ │                                          │   │
│  └───────┬──────────────┘ └──────────────────────────────────────────┘   │
│          │                                                                │
│          │  MCP HTTP Server (:32100)                                      │
│          │  ┌─────────────────────────────────────────┐                  │
│          │  │  POST /mcp   ← JSON-RPC 2.0             │                  │
│          │  │  ・ tools/list    → 32 endpoints as tools│                  │
│          │  │  ・ tools/call    → invoke any handler   │                  │
│          │  │  ・ initialize / ping                    │                  │
│          │  └─────────────────────────────────────────┘                  │
└──────────┼──────────────────────────────────────────────────────────────┘
           │
           │  符号链接: /run/<prog>-fluxipc/<path> → 可执行文件
           │
┌──────────▼──────────────────────┐  ┌────────────────────────────────────┐
│  CLI / Client 进程              │  │  交互式 Shell                      │
│                                 │  │                                    │
│  argv[0] 通过符号链接解析       │  │  加载所有 /dev/shm/fluxipc.*       │
│  → 推导出 prog, socket, path    │  │  → 构建 Tab 补全表                 │
│  → 通过 socket 发送请求         │  │  → readline + 命令补全             │
│  → 打印响应                     │  │  → 内联用法提示                    │
│                                 │  │  → 内联 range 语法                 │
└─────────────────────────────────┘  └────────────────────────────────────┘
```

整个架构由四个核心组件构成：

| 组件 | 路径 | 用途 |
|------|------|------|
| Unix Domain Socket | `/run/<prog>-fluxipc/<prog>.sock` | 实际的数据通信通道 |
| POSIX 共享内存 | `/dev/shm/fluxipc.<prog>` | 端点注册表，包含路径、用法、ID |
| 符号链接 | `/run/<prog>-fluxipc/<path>` | 客户端发现和调用入口 |
| MCP HTTP Server | `0.0.0.0:32100` | 将 IPC 端点暴露为 AI 可调用的 MCP Tools |

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
│  ├─ version                  # 版本信息
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
├─ demo/                       # 演示工具
│  ├─ echo                     # 参数回显
│  ├─ counter                  # 原子计数器
│  └─ arithmetic/              # 算术运算
│     ├─ add                   # 加法
│     └─ mul                   # 乘法
└─ devices/                    # 设备实例（动态注册）
   ├─ stub/primary/            # 虚拟打桩设备
   │  ├─ name / value / reset / info
   └─ io/led/                  # LED 设备
      └─ ...
```

内部节点（如 `/module/sensor`）仅作为命名空间容器；只有叶子节点才绑定处理函数。移除叶子节点时会自动剪枝空的祖先节点。

### 4. MCP Server — 一键接入 AI Agent

FluxIPC 内置了 MCP（Model Context Protocol）HTTP Server，启动后自动将所有已注册的 IPC 端点暴露为 MCP Tools。Claude Desktop、Cursor、Windsurf 等 AI 客户端可以直接发现并调用这些接口。

**服务端只需在初始化时传入 MCP 端口号：**

```c
fluxipc_server_init("myapp", 32100);  // 第二个参数为 0 则禁用 MCP
```

**AI 客户端配置（Claude Desktop / Cursor / Windsurf）：**

```json
{
  "mcpServers": {
    "fluxipc": {
      "url": "http://localhost:32100/mcp"
    }
  }
}
```

配置完成后，AI 就能自动发现并调用你的系统接口了——查询传感器数据、控制电机转速、修改配置参数，全部通过自然语言完成。

**curl 手动调用：**

```bash
# 列出所有可用的 Tools
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'

# 调用一个 Tool
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc":"2.0","id":1,"method":"tools/call",
    "params":{"name":"module_sensor_status","arguments":{"args":[]}}
  }'
```

**Python 客户端调用：**

```python
from fluxipc import FluxIPC

ipc = FluxIPC("localhost", 32100)

# 列出所有工具
for tool in ipc.list_tools():
    print(tool["name"], "–", tool["description"])

# 调用 IPC 接口
print(ipc.call("/system/info"))
print(ipc.call("/demo/arithmetic/add", "7", "3"))
print(ipc.call("/module/sensor/status"))
```

> Python 客户端零依赖，仅使用标准库，文件位于 `python/fluxipc.py`。

### 5. 交互式 Shell — 带 Tab 补全的 IPC 调试器

FluxIPC 提供了一个功能完备的交互式 Shell：

```bash
$ fluxipc-cli --interactive

FluxIPC interactive shell
  Tab              – complete path
  cd <path>        – change namespace
  ls               – show endpoints under current namespace
  help [path]      – show built-in commands or endpoint details
  reload           – refresh registry from shared memory
  watch [s] <path> – call endpoint every N seconds
  exit             – quit

fluxipc> ls
  system_info                         1       Show server process info
  system_uptime                       2       Show server uptime
  module_sensor_status                10      Query current sensor readings
  module_motor_speed                  13      [<rpm>]  Get/set motor speed
  demo_echo                           5       <args...>  Echo back all arguments
  ...

fluxipc> /module/s<TAB>                  # 自动补全为 /module/sensor/
fluxipc> /module/sensor/status           # 执行调用
temperature=23.5 humidity=58.2 sample_rate=10 ok=true

fluxipc> cd /config                     # 切换工作目录
fluxipc@/config> log_level 3            # 相对路径调用
log_level=3
```

**内联 Range 语法** — 一键参数扫描，笛卡尔积自动展开：

```bash
fluxipc> /demo/arithmetic/add 0:100:10 10:50:10
# 自动调用 11×5 = 55 次，每次组合不同的参数值
```

**`!` 前缀执行系统命令：**

```bash
fluxipc> !ping 192.168.1.1
fluxipc> !cat /proc/loadavg
```

Shell 特性一览：

| 功能 | 说明 |
|------|------|
| Tab 补全 | 从共享内存加载所有端点，支持路径层级补全 |
| 内联用法提示 | 输入路径后 Tab 补全时自动显示参数说明 |
| ls 命令 | 列出当前命名空间下所有端点及用途 |
| cd 命令 | 切换工作路径，支持 `cd /`、`cd ..`、相对路径 |
| help 命令 | 查看内置命令说明或单个端点详情 |
| reload 命令 | 重新扫描共享内存，热更新端点列表 |
| watch 命令 | 周期性调用端点，适合监控场景 |
| range 语法 | `start:end:step` 内联展开，自动生成参数组合 |
| ! 系统命令 | 不离开 Shell 即可执行系统命令 |

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

# 安装到系统（可选）
sudo make install
```

### 运行示例服务

```bash
# 终端 1 — 启动示例服务（自动开启 MCP :32100）
./example_server

# 终端 2 — 任意方式调用
fluxipc-cli -s example_server /system/info
fluxipc-cli -s example_server /demo/echo Hello World

# 或进入交互式 Shell
fluxipc-cli -i example_server

# 或通过 curl 调用 MCP 接口
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
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
    if (rc >= 0) return rc;

    // 4. 初始化服务器
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    rc = fluxipc_server_init("myapp", 32100);
    if (rc < 0) {
        fprintf(stderr, "init failed: %d\n", rc);
        return 1;
    }

    // 5. 事件循环（供 AI 客户端/curl/Python 调用）

    printf("Server started. Try:\n");
    printf("  fluxipc-cli -s myapp /greet/hello FluxIPC\n");
    printf("  curl http://localhost:32100/mcp -H 'Content-Type: application/json' "
           "-d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}'\n");

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

# 或者通过 HTTP
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"greet_hello","arguments":{"args":["FluxIPC"]}}}'
# 输出: {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"Hello, FluxIPC!\n"}],"isError":false}}
```

---

## 五种客户端调用方式对比

| 调用方式 | 适用场景 | 示例 |
|----------|----------|------|
| **CLI 直接调用** | Shell 脚本、命令行调试 | `fluxipc-cli -s myapp /path args` |
| **符号链接调用** | 系统命令集成、零配置 | `/run/myapp-fluxipc/path args` |
| **交互式 Shell** | 开发调试、手动探索 | `fluxipc-cli --interactive` |
| **HTTP / MCP** | AI Agent、跨网络、跨语言 | `curl -X POST localhost:32100/mcp` |
| **Python 客户端** | Python 应用集成 | `ipc.call("/path", "arg")` |

---

## 源码结构

```
FluxIPC/
├── include/
│   ├── fluxipc.h              # 公共 API 头文件
│   ├── fluxipc_internal.h     # 内部数据结构
│   └── (MCP 声明在 fluxipc_internal.h，不对外暴露)
├── src/
│   ├── fluxipc_core.c         # 库入口、生命周期管理
│   ├── fluxipc_tree.c         # 命名空间树（插入/查找/删除/剪枝）
│   ├── fluxipc_shm.c          # POSIX 共享内存注册表
│   ├── fluxipc_sock.c         # Unix Socket 服务端/客户端通信
│   ├── fluxipc_symlink.c      # 符号链接管理
│   ├── fluxipc_interactive.c  # 交互式 Shell（readline）
│   └── fluxipc_mcp.c          # MCP HTTP/JSON-RPC Server
├── cli/
│   └── fluxipc_cli.c          # CLI 工具入口
├── tests/
│   └── example_server.c       # 完整示例服务（含多子系统演示）
├── python/
│   ├── fluxipc.py             # Python MCP 客户端（零依赖）
│   └── test_mcp.py            # 集成测试套件
├── packaging/
│   └── install.sh             # 打包安装脚本
├── Makefile                   # 构建系统
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
| **AI Agent 集成** | 内置 MCP Server，一键接入 Claude/Cursor 等 AI 客户端 |
| **快速原型开发** | 几行代码就能暴露 IPC 接口，无需 IDL |

---

## 与其他 IPC 方案对比

| 特性 | FluxIPC | D-Bus | Unix Socket (裸写) | gRPC |
|------|---------|-------|---------------------|------|
| 接口定义 | 路径字符串 | XML 描述 | 自定义协议 | Protobuf |
| 服务发现 | 共享内存自动发现 | 总线守护进程 | 手动配置 | 额外服务 |
| CLI 调试 | 内置交互式 Shell | dbus-send | netcat/自写 | grpcurl |
| AI Agent 集成 | **内置 MCP Server** | 需额外开发 | 需额外开发 | 需额外开发 |
| HTTP 访问 | ✅ 原生支持 | ❌ | ❌ | ✅ (grpc-gateway) |
| Python 客户端 | ✅ 零依赖 | 需 pydbus | 需手写 | 需 grpcio |
| 依赖 | libreadline | libdbus + daemon | 无 | 重量级 |
| 学习成本 | 低 | 中 | 高（需自己造轮子） | 中高 |
| 动态注册 | ✅ | ✅ | 需自己实现 | ✅ |

---

## 写在最后

FluxIPC 是一个**小而美**的项目——它的核心理念很简单：**让 Linux 进程间通信变得跟操作文件系统一样直观，同时让 AI Agent 能零摩擦地调用系统能力**。

无论是嵌入式设备的传感器管理、机器人系统的电机控制，还是想让 Claude 帮你查系统状态、调配置参数，FluxIPC 都能在几行代码内搞定。

> **GitHub 仓库：[https://github.com/Lee-shihao/FluxIPC](https://github.com/Lee-shihao/FluxIPC)**
>
> 如果这个项目对你有帮助，欢迎 **Star ⭐** 支持一下作者！

```bash
git clone https://github.com/Lee-shihao/FluxIPC.git
```

---

*本文基于 FluxIPC v2.1.0 源码分析撰写。*
