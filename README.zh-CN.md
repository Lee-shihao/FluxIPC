# FluxIPC

**[English](README.md) | 简体中文**

一个面向 Linux 的轻量级、命名空间树结构的 IPC 框架，并内置标准 HTTP 接口。

端点存放在类 UNIX 的命名空间树中（`/devices/stub/primary/value`、
`/module/sensor/status`）。容器是命名空间（目录），叶子是可调用的方法。
每个叶子都可以通过**两套共享同一处理器的传输方式**访问：

- **HTTP/1.1** —— REST 风格、JSON 请求体、标准方法、状态码、`Allow`、CORS，
  以及 RFC 7807 错误体。
- **Unix Socket** —— 本地 argv 调用，供 CLI 与交互式 shell 使用。

没有编译期注册：树在运行时逐个节点构建，也不需要任何外部守护进程。

---

## 快速开始

### 依赖

- Linux（`/run`、Unix Domain Socket）
- GCC
- GNU Readline
- cJSON（`libcjson`）

```bash
# Debian / Ubuntu
sudo apt install libreadline-dev libcjson-dev

# Fedora / RHEL
sudo dnf install readline-devel cjson-devel
```

### 构建

```bash
make -j$(nproc)

make lib        # 仅共享库 (libfluxipc.so.1.0.0)
make static     # 仅静态库 (libfluxipc.a)
make cli        # 仅命令行工具 (fluxipc-cli)
make examples   # 示例服务端 (example_server)
make debug      # 打开详细 HTTP 日志 (HTTP_DEBUG=1)
```

### 安装

```bash
sudo make install                   # PREFIX=/usr
sudo make install PREFIX=/usr/local
sudo make uninstall
```

### 运行演示

```bash
# 终端 1
./example_server

# 终端 2 —— HTTP
curl -s http://localhost:32100/system/info
curl -s http://localhost:32100/                 # 叶子清单 (manifest)
curl -s http://localhost:32100/openapi.json

# 终端 2 —— 本地 socket / CLI
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
```

---

## 服务端用法

### 1. 注册端点

注册是动态的、**基于路径**的：辅助函数会自动创建沿途缺失的每一级容器。`fluxipc_register()` 保留 `main` 分支的经典签名 `(path, usage, slot_sz, handler, data)`，注册 IPC 端点；`fluxipc_register_method()` 接收完整的 `fluxipc_method_t`（IPC 和/或 JSON）；`fluxipc_register_json()` 补挂 JSON 端；`fluxipc_register_all()` 一次注册整张静态表。它们都会复制方法描述符（处理器、`summary`、`description`、`schema`），因此传入栈上临时对象或 `FLUXIPC_METHOD` 复合字面量都是安全的。

```c
#include <fluxipc.h>

/* IPC：位置参数 argv → 格式化文本（原始契约） */
static int hello_ipc(void *data, int argc, char **argv,
                     void *out, size_t cap, size_t *len)
{
    const char *name = (argc > 1) ? argv[1] : "World";
    int n = snprintf(out, cap, "Hello, %s!", name);
    *len = (n > 0) ? (size_t)n : 0;
    return 0;
}

/* JSON：已解析的请求 → cJSON 响应（HTTP + 本地调用） */
static int hello_json(void *data, const fluxipc_json_request_t *req,
                      fluxipc_json_response_t *resp)
{
    const char *name = fluxipc_json_str(req, "name", "World");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", "Hello");
    cJSON_AddStringToObject(o, "name", name);
    resp->json = o;              /* 所有权转移给服务端，由其序列化 */
    return 0;
}

int main(void)
{
    fluxipc_server_init("myapp", 32100);            /* 传 0 表示禁用 HTTP */

    /* 一次调用写清完整方法：元数据与两个处理器。 */
    fluxipc_register_method("/greet/hello", &FLUXIPC_METHOD(
        hello_ipc,          /* socket / CLI：原始字节 */
        hello_json,         /* JSON：HTTP + 本地调用 */
        "Say hello",
        FLUXIPC_SCHEMA(FLUXIPC_PROP_STR("name")),
        FLUXIPC_M_GET | FLUXIPC_M_POST));

    /* ……或 main 分支的经典写法（仅 IPC）。 */
    fluxipc_register("/greet/plain", "Say hello", 0, hello_ipc, NULL);

    while (fluxipc_poll(NULL) >= 0) { /* socket + HTTP */ }

    fluxipc_destroy();
}
```

端点较多时，用静态表更整洁：

```c
static const fluxipc_endpoint_t endpoints[] = {
    { "/system/info", FLUXIPC_METHOD(info_ipc, info_json,
                                      "Process info", NULL, FLUXIPC_M_GET) },
    { "/demo/add",    FLUXIPC_METHOD(add_ipc, add_json,
                                      "Add two integers", NULL,
                                      FLUXIPC_M_GET | FLUXIPC_M_POST) },
};

fluxipc_register_all(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
```

`FLUXIPC_METHOD_D(ipc, json, data, summary, schema, methods)` 与前者相同，只是额外设置每个叶子的 `data` 指针，用于有状态的处理器。

#### 兼容 `main` 分支

为 `main` 分支接口编写的代码可原样编译链接：

- `fluxipc_register(path, usage, slot_sz, handler, data)`——`usage` 映射为叶子的 `summary`；`slot_sz`（旧的共享内存槽大小）仅为源码兼容而保留，会被忽略；处理器的 `void *out_buf` 参数与公共 `fluxipc_handler_fn` 一致；
- `fluxipc_unregister(node)` 保留为 `fluxipc_node_remove(node)` 的别名；
- `FLUXIPC_STATIC(path, usage, slot_sz, handler, ctx)` 声明编译期端点。条目在 `main()` 之前由构造函数排队，由 `fluxipc_server_init()` 安装，因此**不需要 `-rdynamic`，也不需要链接器段**。

```c
static int hello_ipc(void *d, int argc, char **argv,
                     void *out, size_t cap, size_t *len) { /* ... */ }

FLUXIPC_STATIC("/greet/compiled", "say hello", 0, hello_ipc, NULL);
```

#### 分离注册（IPC 与 HTTP 各自独立）

对同一路径再次注册会**合并**到已有叶子上：只填充仍为空的字段，因此 IPC-only 的注册可以稍后用 `fluxipc_register_json()` 补上 JSON，反之亦然。方法掩码由首次挂上 JSON 处理器的那次调用决定；已存在的处理器、元数据或 `data` 不会被覆盖。

```c
/* 经典 / IPC-only 注册（方法里的 http 也可以传 NULL）。 */
fluxipc_register("/greet/hello", "Say hello", 0, hello_ipc, NULL);

/* 稍后补上 JSON 端（ipc 传 NULL）。 */
fluxipc_register_json("/greet/hello", &FLUXIPC_METHOD(
    NULL, hello_json, "Say hello", NULL, FLUXIPC_M_GET));
```

注册失败时是原子的：不会遗留创建到一半的容器，也不会剪除你自己创建的容器。

编译/链接：

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs fluxipc) -lcjson
```

#### 更底层的节点 API

注册辅助函数只是对节点原语的轻量封装；当需要在运行时动态修改树时，仍可使用这些原语：

| 函数 | 说明 |
|------|------|
| `fluxipc_tree_root()` | 根节点（在 `fluxipc_server_init` 之后可用） |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | main 分支经典注册（IPC） |
| `fluxipc_register_method(path, &method)` | 注册完整方法；重复时合并 |
| `fluxipc_register_json(path, &method)` | 注册 JSON 端（合并，`ipc` 通常为 NULL） |
| `fluxipc_register_all(table, count)` | 注册静态端点表 |
| `fluxipc_node_mkdir(parent, name)` | 获取或创建容器子节点（幂等） |
| `fluxipc_node_leaf(parent, name, &method)` | 在 `parent` 下创建叶子方法 |
| `fluxipc_node_find(parent, name)` | 查找直接子节点 |
| `fluxipc_node_remove(node)` / `fluxipc_unregister(node)` | 删除叶子或整棵子树；自动剪枝空祖先 |
| `fluxipc_node_leaves(node, cb, user)` | **每个节点内置**：枚举其自身及以下的全部叶子 |

### 3. 处理器签名

一个叶子携带**两个相互独立的处理器**。至少注册一个；任一可以为 `NULL`
（此时对应传输会报告*未实现*）。

**IPC 处理器** —— 位置参数 argv，原始输出缓冲区（原始契约）：

```c
typedef int (*fluxipc_handler_fn)(void *data, int argc, char **argv,
                                  void *out_buf, size_t out_cap, size_t *out_len);
```

**JSON 处理器** —— 已解析请求，cJSON 响应：

```c
typedef int (*fluxipc_json_handler_fn)(void *data,
                                       const fluxipc_json_request_t *req,
                                       fluxipc_json_response_t *resp);
```

`fluxipc_json_request_t` 除标准请求字段外，还携带已解析的 JSON 请求体：

| 字段 | 含义 |
|------|------|
| `method` | 原始动词字符串，例如 `"GET"`（用于日志） |
| `verb` | 框架解析后的 `FLUXIPC_M_*` 位；业务代码使用下面的宏 |
| `path` | 解码后的命名空间路径，例如 `/system/info` |
| `query` / `query_count` | 查询参数 |
| `headers` / `header_count` | HTTP 头 |
| `content_type`, `body`, `body_len` | 原始请求体 |
| `json` | 已解析的请求体（`const cJSON *`），或 `NULL` |
| `data` | 该方法注册时的 `data` 指针 |

框架在传输入口只解析一次动词字符串。业务代码比较解析后的位，而不是调用
`strcmp(req->method, ...)`：

```c
if (FLUXIPC_VERB_IS_WRITE(req)) {          /* POST / PUT / PATCH */
    long rpm = fluxipc_json_int(req, "rpm", -1);
}

if (FLUXIPC_VERB_IS(req, FLUXIPC_M_DELETE)) {
    /* ... */
}
```

常用字段读取由框架提供，先读 JSON 请求体，再读 query 参数：

```c
const char *name = fluxipc_json_str(req, "name", "World");
long        n    = fluxipc_json_int(req, "n", 0);
double      d    = fluxipc_json_double(req, "ratio", 1.0);
int         ok   = fluxipc_json_bool(req, "enabled", 0);
int         has  = fluxipc_json_has(req, "name");
```

错误响应也由框架构造；传入 errno（或显式正状态码）和消息即可：

```c
return fluxipc_json_error(resp, -EINVAL, "level must be 0-5");
return fluxipc_json_error(resp, -EBUSY,  "motor is disabled");
return fluxipc_json_error(resp, 409,     "custom conflict");
```

`fluxipc_json_response_t` 包含 `status`（0 → 200）、拥有所有权的
`cJSON *json`，以及可选的 `content_type`。

默认情况下两种表示保持独立的返回风格：**IPC 返回原始字节，JSON 返回
cJSON。** `fluxipc_call_json()` 增加一个可选的 IPC JSON 表示模式：它调用叶子的
JSON 处理器并序列化其结果，因此 JSON 仍然只有唯一的生产者。

### JSON Schema 辅助宏

`fluxipc_method_t.schema` 是标准 JSON Schema 文本。它属于公共契约：会出现在
容器 manifest、`/openapi.json` 中，并用于 `CALL_JSON` 位置参数绑定。因此顶层的
`"type":"object"` 并不冗余——它明确声明请求体是对象，也让 OpenAPI 输出更精确。

为了避免转义字符串，可以使用辅助宏：

```c
FLUXIPC_SCHEMA(
    FLUXIPC_PROP_INT("a") "," FLUXIPC_PROP_INT("b"))

FLUXIPC_SCHEMA(FLUXIPC_PROP_STR("name"))
FLUXIPC_SCHEMA(FLUXIPC_PROP_NUMBER("offset"))
FLUXIPC_SCHEMA(FLUXIPC_PROP_ARRAY("args", "string"))
```

这些宏只是展开成普通 JSON Schema 文本；需要 `required`、描述、范围约束等完整
能力时，仍然可以直接传入手写 schema。

旧的 `fluxipc_http_request_t`、`fluxipc_http_response_t`、
`fluxipc_http_handler_fn` 和 `fluxipc_register_http` 名称仍作为兼容别名保留。

---

## HTTP 接口

树直接映射到 URL 路径。叶子是资源，容器是集合。

```bash
# 读取叶子
curl -s http://localhost:32100/system/info

# 写入叶子（带 JSON 请求体的 PUT/POST）
curl -s -X PUT -H 'Content-Type: application/json' \
     -d '{"rate_hz":20}' http://localhost:32100/module/sensor/config

# 查询参数
curl -s 'http://localhost:32100/demo/arithmetic/add?a=7&b=3'

# 列出容器下的全部叶子（每个节点内置）
curl -s http://localhost:32100/module
curl -s 'http://localhost:32100/module?recursive=0'   # 仅直接叶子

# 发现允许的方法
curl -s -X OPTIONS -i http://localhost:32100/system/info

# 面向机器的、覆盖所有端点的模式描述
curl -s http://localhost:32100/openapi.json
```

| 方法 | 作用于叶子 | 作用于容器 |
|------|-----------|-----------|
| `GET` | 调用处理器（读取） | 叶子清单 |
| `POST` | 调用处理器（动作） | `405` + `Allow` |
| `PUT` / `PATCH` / `DELETE` | 调用处理器 | `405` + `Allow` |
| `HEAD` | 同 `GET`，无响应体 | 同 `GET`，无响应体 |
| `OPTIONS` | `204` + `Allow` | `204` + `Allow` |

**遵循的标准：** `Content-Type: application/json; charset=utf-8`、
`Content-Length`、`Allow`、CORS、`application/problem+json` 错误体
（RFC 7807），以及 OpenAPI 3.1。

清单示例（`GET /system`）：

```json
{
  "path": "/system",
  "kind": "collection",
  "count": 4,
  "leaves": [
    {
      "path": "/system/info",
      "name": "info",
      "methods": ["GET"],
      "summary": "Show server process info"
    }
  ]
}
```

错误示例（对仅支持 POST 的叶子执行 `GET`）：

```json
{
  "type": "about:blank",
  "title": "Method Not Allowed",
  "status": 405,
  "detail": "method not allowed for this endpoint",
  "instance": "/system/shutdown"
}
```

### 用法 / 文档元数据

每个方法都带有一个 `summary`、可选的 `description`，以及可选的 JSON Schema
`schema`。它们会出现在：

- 容器的叶子清单中；
- 生成的 OpenAPI 文档中；
- 交互式 shell / `--list` 的输出中。

这里**没有自动生成的 `_usage` 兄弟叶子**：元数据属于叶子自身的表示，而不是
一个独立资源。这样可以保持命名空间干净、避免命名冲突，也无需额外注册。如果
你确实想要一个专门的资源，随时可以显式注册一个。

---

## 本地（Unix Socket）接口

同一个处理器，不同的传输方式。

### CLI

```bash
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
fluxipc-cli example_server --json /system/info
fluxipc-cli example_server --json /demo/arithmetic/add '{"a":7,"b":3}'
fluxipc-cli example_server --json /demo/arithmetic/add 7 3
fluxipc-cli example_server --json -X PUT /module/sensor/config '{"rate_hz":20}'
fluxipc-cli example_server --list
```

`--json` 模式下：单个 `{...}`/`[...]` 参数作为 JSON 请求体，`key=value`
参数作为 query 参数，裸参数则按 JSON Schema 的属性声明顺序进行位置绑定
（末尾的数组属性会收集剩余参数）。

### 交互式 shell

```bash
fluxipc-cli example_server --interactive
```

| 命令 | 说明 |
|------|------|
| `ls` | 列出当前命名空间下的叶子 |
| `cd <path>` | 切换命名空间 |
| `help [path]` | 内置帮助 / 端点详情 |
| `reload` | 重新扫描运行中的服务端并刷新注册表 |
| `watch [s] <path>` | 每隔 N 秒调用一次端点 |
| `Tab` | 路径补全 |

---

## Python 客户端

仅依赖标准库的 REST 客户端：

```python
from fluxipc import FluxIPC

ipc = FluxIPC("localhost", 32100)
print(ipc.get("/system/info"))
print(ipc.get("/demo/arithmetic/add", a=7, b=3))
ipc.put("/module/sensor/config", {"rate_hz": 20})
print(ipc.list("/module"))          # 叶子清单
print(ipc.openapi())                # OpenAPI 文档
```

集成测试：

```bash
./example_server &
python3 python/test_http.py
python3 python/test_http.py --host 192.168.1.100 --port 32100
```

---

## 测试与质量

整套测试工具链**本地**安装到 `.tools/`，无需 root：

```bash
make tools      # 下载并解包 Mull 0.34.1、cmocka 1.1.5、lcov 到 .tools/
```

| 命令 | 作用 |
|------|------|
| `make ut` | **cmocka** 单元套件 → `fluxipc-ut`（170 个用例） |
| `make it` | 端到端套件 → `fluxipc-it`（fork `example_server`） |
| `make test` | `ut` + `it` |
| `make cov` | 插桩构建 + 文本 & HTML 覆盖率报告，含行/分支门禁 |
| `make mutate` | **Mull** 变异测试，含变异分数门禁 |
| `make ut-asan` | AddressSanitizer + UBSan 构建（含泄漏检测） |

可调参数（环境变量）：

```bash
COV_LINE_MIN=90  COV_BRANCH_MIN=78  MUT_SCORE_MIN=70  MUT_WORKERS=4
```

### 当前质量门禁

| 门禁 | 目标 | 实测 |
|------|------|------|
| 行覆盖率（`src/`） | ≥ 90% | **95.8%** |
| 分支覆盖率（`src/`） | ≥ 78% | **79.1%** |
| 变异分数 | > 70% | **78%** |
| ASan + UBSan | 干净 | 干净 |

`make cov` 会打印两张文本表（行覆盖与分支覆盖，含未覆盖的行/分支号），同时
写出 `build/coverage/index.html`、`cobertura.xml` 与 `summary.json`。
`make mutate` 将报告写入 `build/mutation/`。

### 单元套件（cmocka，`tests/unit/`）

| 套件 | 覆盖内容 |
|------|---------|
| `protocol` | 固定 12 字节线协议头布局（客户端/服务端不可漂移） |
| `util` | URL 解码、查询串解析、不修改输入的 `Content-Length` 扫描 |
| `tree` | 命名空间树：创建/查找/删除、名称槽冲突、深路径 |
| `core` | 生命周期、注册、manifest、分发、请求钩子 |
| `sock` | `sock_conn_process`（伪连接）、客户端辅助函数、畸形/截断响应 |
| `conn` | 连接池 + 非阻塞读写状态机（socketpair） |
| `http` | 解析、路由、`Allow`、RFC 7807、OpenAPI、HEAD/OPTIONS/错误 |
| `interactive` | 路径 trie、范围展开、Tab 补全、REPL 命令 |
| `cli` | 参数模式与输出 |

部分模块采用白盒测试：传输处理器通过内存中的 `fluxipc_conn_t` 驱动；
`fluxipc_interactive.c` / `fluxipc_cli.c` 由各自的测试翻译单元 `#include` 进来，
以便直接触达 `static` 辅助函数。

### 集成测试（`tests/integration/`）

fork `example_server`，端到端地验证两种传输：IPC 调用（含参数与错误传播）、
`OP_LIST` 发现、HTTP 200/404/405/`problem+json`，以及“启动后立即连接”的回归。

### Python 客户端测试

仅依赖标准库的 REST 客户端由 `python/test_http.py` 针对运行中的服务端进行覆盖
（见上文）。

---

## 文件位置

| 路径 | 用途 |
|------|------|
| `/run/user/<uid>/<prog>-fluxipc/<prog>.sock` | Unix domain socket（数据 + 内省） |

---

## API 参考

### 服务端 / 树

| 函数 | 说明 |
|------|------|
| `fluxipc_server_init(prog, http_port)` | 初始化 socket 与可选 HTTP（0 = 关闭） |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | main 分支经典注册（IPC） |
| `fluxipc_register_method(path, method)` | 注册完整方法；重复时合并 |
| `fluxipc_register_json(path, method)` | 注册端点的 JSON 端 |
| `fluxipc_register_all(table, count)` | 注册静态 `fluxipc_endpoint_t` 表 |
| `FLUXIPC_STATIC(path, usage, slot, handler, data)` | 编译期端点（构造函数排队） |
| `fluxipc_tree_root()` | 根节点 |
| `fluxipc_node_mkdir(parent, name)` | 获取或创建容器 |
| `fluxipc_node_leaf(parent, name, method)` | 注册一个叶子 |
| `fluxipc_node_find(parent, name)` | 查找子节点 |
| `fluxipc_node_remove(node)` | 删除叶子/子树 |
| `fluxipc_node_leaves(node, cb, user)` | 枚举后代叶子 |
| `fluxipc_node_path/name/method/is_leaf` | 节点访问器 |
| `fluxipc_poll(NULL)` | 分发待处理的 socket + HTTP 请求 |
| `fluxipc_stop()` / `fluxipc_destroy()` | 关闭 |
| `fluxipc_usage(prog)` | 打印端点列表 |
| `fluxipc_set_request_hooks(pre, post, user)` | 观察请求 |

### 客户端

| 函数 | 说明 |
|------|------|
| `fluxipc_call(sock, path, argc, argv, out, cap, len)` | 底层 socket 调用（原始字节） |
| `fluxipc_call_json(sock, path, method, paramc, params, &out, &len)` | 走 IPC，返回 JSON 处理器的响应 |
| `fluxipc_list_endpoints(prog, cb, data)` | 通过服务端 socket 枚举端点 |
| `fluxipc_interactive_init(prog)` | 进入 readline REPL |

### JSON 处理器辅助

| 函数 / 宏 | 说明 |
|-----------|------|
| `fluxipc_json_str/int/double/bool(req, key, def)` | 先读 JSON 请求体，再读 query 参数 |
| `fluxipc_json_has(req, key)` | 判断请求体或 query 中是否存在字段 |
| `fluxipc_json_error(resp, code, msg)` | 构造 `{"error":msg,"code":code}` 并设置状态码 |
| `FLUXIPC_VERB_IS(req, mask)` | 用解析后的动词位比较 `FLUXIPC_M_*` |
| `FLUXIPC_VERB_IS_READ(req)` / `FLUXIPC_VERB_IS_WRITE(req)` | 常用 GET / 写操作判断 |

---

## 源码结构

```
FluxIPC/
├── include/
│   ├── fluxipc.h              # 公共 API（树、处理器、HTTP 客户端）
│   └── fluxipc_internal.h     # 内部数据结构
├── src/
│   ├── fluxipc_core.c         # 生命周期、注册、分发
│   ├── fluxipc_tree.c         # 命名空间树
│   ├── fluxipc_http.c         # 标准 HTTP/1.1 + REST + OpenAPI
│   ├── fluxipc_sock.c         # Unix socket 传输 + 协议
│   ├── fluxipc_conn.c         # 统一非阻塞事件循环
│   ├── fluxipc_util.c         # 纯解析辅助（URL/查询/长度）
│   └── fluxipc_interactive.c  # 交互式 REPL
├── cli/fluxipc_cli.c          # CLI 入口
├── tests/
│   ├── example_server.c       # 演示服务端（双处理器）
│   ├── unit/                  # cmocka 单元套件 (fluxipc-ut)
│   │   ├── test.h / ut_common.*   # 共享 fixture 与辅助
│   │   └── test_{protocol,util,tree,core,sock,conn,http,interactive,cli}.c
│   ├── integration/           # 端到端测试 (fluxipc-it)
│   ├── tools/                 # bootstrap.sh、coverage.sh、mutate.sh、env.sh
│   └── UNIT_TEST_PLAN.md      # 测试策略与指标
├── python/
│   ├── fluxipc.py             # Python REST 客户端
│   └── test_http.py           # Python HTTP 测试
├── mull.yml                   # Mull 变异测试配置
├── Makefile
├── README.md                  # English
└── README.zh-CN.md            # 简体中文
```

---

## 设计说明

- **一棵树、两种表示** —— HTTP 与 Unix socket 解析同一棵树；原始 IPC 调用
  `ipc`，结构化 JSON 调用（HTTP 或本地）调用 `json`。
- **注册是机制而非策略** —— 路径化辅助函数（`fluxipc_register_method`）会自动创建缺失的容器，节点原语仍然保留。为兼容 main 分支保留了经典 `fluxipc_register` 签名与 `FLUXIPC_STATIC`；`FLUXIPC_STATIC` 由构造函数排队，因此依旧没有 ELF section、也不需要 `-rdynamic`。
- **内置叶子枚举** —— 每个节点都能枚举其叶子；HTTP 通过 `GET /<container>`
  暴露，socket 通过协议的 `LIST` 操作暴露。
- **REST + OpenAPI** —— 标准方法、状态码、`Allow`，以及 RFC 7807 错误；
  `/openapi.json` 描述每个叶子。
- **统一非阻塞事件循环**（`fluxipc_conn.c`）—— 单个 `select()` 驱动两个监听
  套接字与所有连接，通过共享的缓冲状态机处理，因此任何对端都无法阻塞服务端。

---

## 路线图 / TODO

以下事项是**有意延后**的：它们只在特定部署形态下才成立，提前做会给库引入尚不
需要的策略与复杂度。每条都标注了“什么时候才值得做”的触发条件。

### 安全与访问控制
- [ ] **认证 + TLS** —— HTTP 无认证且绑定 `INADDR_ANY`；Unix socket 依赖操作系统权限。当 HTTP 端口暴露给非受信网络时再做（优先在反向代理处终止 TLS/认证，而不是做进库内）。
- [ ] **安全默认值** —— HTTP 默认只绑 `127.0.0.1`（地址可配置），并停止无条件发送 `Access-Control-Allow-Origin: *`。成本低，可提前做。
- [ ] **可拒绝请求的 gate 钩子** —— 现有 pre/post 钩子只读、无法拦截请求；在此之上才谈得上应用层认证。
- [ ] **限流 / 空闲超时** —— 慢速客户端可长期占用 32 个连接槽之一；需要请求限流与每连接空闲超时。

### 并发与控制权
- [ ] **独占控制（租约）** —— “此刻谁能操作该设备”，用 TTL 租约 + owner 标识（+ fencing token）表达，**刻意不**绑定在连接上。仅当多个独立客户端不能交错写时才需要；否则保持命令幂等 / 最后写入生效。
- [ ] **乐观并发** —— 对读-改-写端点用 `ETag` / `If-Match`，冲突返回 `409` / `412`；是租约的轻量替代。

### 性能
- [ ] **HTTP keep-alive / 长连接** —— 省去每请求一次 TCP 握手。仅在高请求频率或严格延迟预算下才值得；直连链路收益很小，且会带来空闲超时与连接池占用问题。
- [ ] **批处理** —— 一次请求携带多个操作；对频繁交互的客户端通常比 keep-alive 收益更大。
- [ ] **紧凑线协议** —— 高频设备控制用持久化的帧协议替代 HTTP 文本 + JSON。
- [ ] **削减每响应开销** —— 非浏览器链路去掉（无用的）CORS 头；热路径避免 cJSON。

### 扩展性
- [ ] **多线程 / 线程池** —— 事件循环是单线程，这正是 handler 无数据竞争的原因。只有出现多并发客户端、单核打满或阻塞 handler 时才加线程；优先“单 reactor + handler 工作线程池”或用 `SO_REUSEPORT` 的线程-核模型。这会重新引入 handler 层的共享状态加锁。

### 健壮性
- [ ] **节点 API 的 `ENAMETOOLONG`** —— `fluxipc_node_mkdir()` / `fluxipc_node_leaf()` 对超长 `full_path` 会静默截断；路径化辅助函数已经拒绝该情况。
