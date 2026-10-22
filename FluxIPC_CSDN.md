# FluxIPC：面向 Linux 的命名空间树 IPC 库（IPC-only 版）

FluxIPC 是一个可嵌入应用进程的 Linux IPC 库。它用一棵命名空间树组织端点：
中间节点是容器，叶子节点是可调用方法。客户端通过 Unix domain socket 调用
叶子，参数是位置参数 `argv`，返回值是原始字节。

当前版本刻意只保留 IPC：

- 没有 HTTP 服务器
- 没有 JSON 请求/响应
- 不依赖 cJSON
- 不创建任何内部线程

## 1. 一次调用长什么样

```c
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
```

注册：

```c
fluxipc_register_method("/demo/add",
    &FLUXIPC_METHOD(add_ipc, "两数相加"));
```

调用：

```bash
fluxipc-cli myapp -c /demo/add 7 3
# 10
```

`argv[0]` 是端点路径 `/demo/add`，后面才是 `7`、`3`。这是 main 分支一直以来的
约定。

## 2. 线程模型：库不拥有线程

FluxIPC 只提供事件循环原语，调度权完全交给宿主应用。

### 方式 A：应用创建线程运行

```c
static void *serve(void *arg)
{
    fluxipc_run();          /* 阻塞直到 fluxipc_stop() */
    return NULL;
}

int main(void)
{
    fluxipc_server_init("myapp");
    fluxipc_register_method("/demo/add", ...);

    pthread_t th;
    pthread_create(&th, NULL, serve, NULL);

    /* 主线程做应用自己的工作 */

    fluxipc_stop();
    pthread_join(th, NULL);
    fluxipc_destroy();
    return 0;
}
```

### 方式 B：接入已有调度器

```c
for (;;) {
    int rc = fluxipc_poll_timeout(100);   /* ms；<0 无限，0 非阻塞 */
    if (rc < 0) break;
    /* 其它周期任务 */
}
```

### 线程保证

- `fluxipc_run()` / `fluxipc_poll_timeout()`：同一时刻只允许一个线程调用；
  并发 poll 返回 `-EBUSY`。
- `fluxipc_wakeup()` / `fluxipc_stop()`：任意线程安全。
- `fluxipc_register_method()` / `fluxipc_node_remove()`：与 poll 线程并发安全
  （树锁保护）。
- 处理器在 poll 线程上执行。
- `fluxipc_call()` / `fluxipc_list_endpoints()`：每次调用独立连接，多线程安全。
- `fluxipc_destroy()`：必须在 poll 线程 join 之后调用。

## 3. 线协议

请求头 12 字节：

```c
typedef struct {
    uint32_t magic;
    uint16_t op;
    uint16_t path_len;
    uint32_t arg_len;
} __attribute__((packed)) fluxipc_req_hdr_t;
```

响应头 12 字节：

```c
typedef struct {
    uint32_t magic;
    int32_t  status;
    uint32_t data_len;
} __attribute__((packed)) fluxipc_resp_hdr_t;
```

两种操作：

- `FLUXIPC_OP_CALL`：调用叶子 IPC 处理器，返回原始字节。
- `FLUXIPC_OP_LIST`：返回文本清单，每行
  `<id>\t<path>\t<summary>\n`。

请求体是路径加上 NUL 分隔的参数串：

```text
[12-byte header][path bytes][arg0\0arg1\0...]
```

## 4. 非阻塞连接层

`fluxipc_conn.c` 用单个 `select()` 驱动：

- Unix socket 监听 fd
- eventfd 唤醒 fd
- 固定 32 个连接槽

每个连接是一个状态机：

```text
accept → READING（累积请求） → process → WRITING（刷出响应） → close
```

设计约束：

- 每连接一次请求/响应
- 请求上限 512 KiB
- 空闲连接 30 秒超时
- 慢客户端不会阻塞服务端
- 连接槽用 `fd == -1` 表示空闲（避免把 fd 0 误判为占用）

## 5. 命名空间树

```c
fluxipc_node_t *root = fluxipc_tree_root();
fluxipc_node_t *a    = fluxipc_node_mkdir(root, "a");
fluxipc_node_leaf(a, "b", &FLUXIPC_METHOD(fn, "leaf b"));
```

路径注册会自动创建中间容器：

```c
fluxipc_register_method("/a/b/c", &method);
```

删除：

```c
fluxipc_node_remove(a);   /* 删除子树，并裁剪空祖先 */
```

查找复杂度是 O(depth × siblings)，对典型 IPC 命名空间（深度 ≤ 8、扇出 ≤ 64）
足够。

## 6. 兼容 main 分支

```c
fluxipc_register(path, usage, slot_sz, handler, data);
```

- `usage` 映射为 leaf summary
- `slot_sz` 保留但忽略（旧共享内存槽架构已移除）
- `fluxipc_unregister()` 是 `fluxipc_node_remove()` 的别名
- `FLUXIPC_STATIC(path, usage, slot_sz, handler, ctx)` 编译期注册仍然可用

重复注册同一路径会合并：只填充仍为空的元数据，不覆盖已有处理器。

## 7. 测试

```bash
make ut          # cmocka 单元测试
make it          # 端到端 IPC 测试
make test        # ut + it
make test-mem    # ASan/UBSan/LeakSanitizer
make bench       # p50/p95/p99、ops/s、RSS
```

测试覆盖：

- 12 字节线协议布局与畸形请求
- 树创建/查找/删除/裁剪
- 文本 manifest 的分隔与转义
- 宿主线程运行 `fluxipc_run()`、stop/wakeup、单 poller
- poll 线程运行期间并发注册
- 8 线程并发客户端
- 32 KiB 大参数
- 优雅退出

## 8. 小结

这版 FluxIPC 的目标很明确：

> 用最小、稳定、可嵌入的 Unix-socket IPC 内核，
> 把线程与调度的决定权交还给宿主应用。

没有 HTTP/JSON 的额外依赖和状态机，核心路径更短，线程边界也更清晰。
