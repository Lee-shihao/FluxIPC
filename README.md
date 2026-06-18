# FluxIPC

A lightweight, namespace-tree IPC library for Linux.

Endpoints are organised as a UNIX-style path hierarchy (`/devices/stub/xxx/name`, `/module/sensor/status`), registered at compile-time or runtime, and discovered via POSIX shared memory. Built-in MCP (Model Context Protocol) server exposes all endpoints as AI-callable tools over HTTP.

---

## Quick Start

### Prerequisites

- Linux (requires `/dev/shm`, `/run`, Unix Domain Socket)
- GCC
- GNU Readline

```bash
# Debian / Ubuntu
sudo apt install libreadline-dev

# Fedora / RHEL
sudo dnf install readline-devel
```

### Build

```bash
git clone https://github.com/Lee-shihao/FluxIPC.git
cd FluxIPC
make -j$(nproc)
```

Individual targets:

```bash
make lib           # shared library only (libfluxipc.so.1.0.0)
make static        # static library only (libfluxipc.a)
make cli           # CLI tool only (fluxipc-cli)
make examples      # example server (example_server)
```

### Install

```bash
sudo make install                   # defaults to PREFIX=/usr
sudo make install PREFIX=/usr/local
```

### Uninstall

```bash
sudo make uninstall
sudo make uninstall PREFIX=/usr/local
```

### Run the Demo

```bash
# Terminal 1 — start the example server
./example_server

# Terminal 2 — interact with it (any method below)
```

---

## Server-Side Usage

### Compile-time Registration

Use `FLUXIPC_STATIC` to declare endpoints that are automatically discovered at startup:

```c
#include <fluxipc/fluxipc.h>

static int ping_handler(void *data, int argc, char **argv,
                        void *out, size_t cap, size_t *len) {
    const char *name = (argc > 1) ? argv[1] : "World";
    *len = snprintf(out, cap, "Hello, %s!\n", name);
    return 0;
}
FLUXIPC_STATIC("/greet/hello", "<name>  Say hello", 0, ping_handler, NULL);
```

### Runtime Registration

Use `fluxipc_register()` to add endpoints dynamically:

```c
fluxipc_node_t *node = fluxipc_register(
    "/my/app/dynamic", "Dynamic endpoint", 0, my_handler, ctx);
```

### Server Lifecycle

```c
#include <fluxipc/fluxipc.h>

int main(int argc, char **argv) {
    // 1. Check if invoked as symlink client (handle and return)
    int rc = fluxipc_client_dispatch(argc, argv);
    if (rc >= 0) return rc;

    // 2. Init server (registers static entries, creates socket + shm)
    //    Pass 0 for mcp_port to disable MCP, or a port number to enable it
    fluxipc_server_init("myapp", 32100);

    // 3. Register dynamic endpoints
    fluxipc_register("/devices/led/value", "[v]  Set LED brightness",
                     0, led_handler, &led_ctx);

    // 4. Event loop (polls both IPC socket and MCP connections)
    while (g_running) fluxipc_poll(NULL);

    // 5. Cleanup (stops MCP automatically)
    fluxipc_destroy();
}
```

Compile with:

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs fluxipc) -Wl,--export-dynamic
```

> `--export-dynamic` (or `-rdynamic`) is required so the linker retains `fluxipc_registry` section symbols.

### Handler Signature

```c
typedef int (*fluxipc_handler_fn)(void   *data,    // opaque context
                                  int     argc,    // argv[0] = path
                                  char  **argv,    // argument vector
                                  void   *out_buf, // response buffer
                                  size_t  out_cap, // buffer capacity
                                  size_t *out_len); // bytes written
// Returns 0 on success, negative errno on error
```

---

## Client Call Methods

### Method 1 — CLI Direct Call

```bash
fluxipc-cli --server <prog> <path> [args...]
```

Examples:

```bash
fluxipc-cli --server example_server /system/info
fluxipc-cli --server example_server /demo/arithmetic/add 7 3
fluxipc-cli --server example_server /module/sensor/calibrate 1.25
fluxipc-cli -s example_server /demo/echo hello world          # short form
```

### Method 2 — Symlink Invocation

After server starts, symlinks are created under `/run/<prog>-fluxipc/`. Invoke them directly:

```bash
/run/example_server-fluxipc/system/info
/run/example_server-fluxipc/demo/arithmetic/add 7 3
/run/example_server-fluxipc/demo/echo hello world
```

Or create a convenience alias anywhere in `$PATH`:

```bash
ln -s /run/example_server-fluxipc/system/info /usr/local/bin/myapp-info
myapp-info
```

### Method 3 — Interactive CLI Shell

```bash
# Discover all running FluxIPC servers
fluxipc-cli --interactive

# Focus on one server
fluxipc-cli --interactive example_server
```

Shell features:

| Command | Description |
|---------|-------------|
| `ls` | List all endpoints under current namespace |
| `cd <path>` | Change working namespace (`cd /` to return to root) |
| `help [path]` | Show built-in commands, or details for one endpoint |
| `reload` | Re-scan shared memory, hot-reload endpoint list |
| `watch [s] <path>` | Call endpoint every N seconds (default 1) |
| `exit` / `quit` | Exit the shell |
| `Tab` | Path completion (loaded from shared memory) |

Inline range syntax (Cartesian-product sweep):

```bash
fluxipc> /demo/arithmetic/add 1:3 10:30:10
# Calls add with (1,10) (1,20) (1,30) (2,10) (2,20) (2,30) (3,10) (3,20) (3,30)
```

Shell commands (`!` prefix execs system commands):

```bash
fluxipc> !ping 192.168.1.1
fluxipc> !cat /proc/cpuinfo
```

### Method 4 — HTTP / MCP (curl, AI Clients)

The server exposes all endpoints as MCP tools over HTTP on port 32100.

**List tools:**

```bash
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

**Call a tool:**

```bash
curl -s http://localhost:32100/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc":"2.0","id":1,"method":"tools/call",
    "params":{"name":"demo_arithmetic_add","arguments":{"args":["7","3"]}}
  }'
```

**Tool naming rule:** path `/demo/arithmetic/add` → MCP tool name `demo_arithmetic_add` (slashes become underscores).

**Compatible AI clients:**
- Claude Desktop / Cursor / Windsurf / OpenClaw / Hermes
- Any MCP-compliant host (spec 2024-11-05 or 2025-03-26)

Configure your AI client's `mcpServers`:

```json
{
  "mcpServers": {
    "fluxipc": {
      "url": "http://localhost:32100/mcp"
    }
  }
}
```

### Method 5 — Python Client

Zero-dependency Python client (stdlib only):

```python
from fluxipc import FluxIPC

# Connect to a running server
ipc = FluxIPC("localhost", 32100)

# List all tools
for tool in ipc.list_tools():
    print(tool["name"], "–", tool.get("description", ""))

# Call endpoints (path notation, leading slash optional)
print(ipc.call("/system/info"))
print(ipc.call("/demo/arithmetic/add", "7", "3"))
print(ipc.call("/module/sensor/status"))

# Context manager
with FluxIPC("192.168.1.100", 32100) as ipc:
    print(ipc.call("/system/uptime"))
```

Run the integration test suite:

```bash
python3 python/test_mcp.py                           # localhost:32100
python3 python/test_mcp.py --host 192.168.1.100      # remote host
python3 python/test_mcp.py --port 3002               # custom port
python3 python/test_mcp.py --host 10.0.0.5 -p 8080
```

---

## List Registered Endpoints

```bash
fluxipc-cli --list example_server
fluxipc-cli -l example_server
```

Output:

```
PATH                                         ID      USAGE
------------------------------------------  ------  -----------------------------
/system/info                                1       Show server process info
/system/uptime                              2       Show server uptime
/module/sensor/status                       10      Query current sensor readings
...
```

---

## File Locations

| Path | Purpose |
|------|---------|
| `/run/<prog>-fluxipc/<prog>.sock` | Unix domain socket (IPC transport) |
| `/dev/shm/fluxipc.<prog>` | POSIX shared memory registry |
| `/run/<prog>-fluxipc/<path>` | Client discovery symlinks |
| `/run/user/<uid>/<prog>-fluxipc/` | Per-user runtime directory |
| `/etc/tmpfiles.d/fluxipc.conf` | Ensures runtime dirs survive reboot |

---

## API Reference

### Server API

| Function | Description |
|----------|-------------|
| `fluxipc_server_init(prog, mcp_port)` | Init server: socket, shm, static entries, optional MCP (0=off) |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | Register endpoint at runtime |
| `fluxipc_unregister(node)` | Remove a dynamic endpoint |
| `fluxipc_poll(NULL)` | Dispatch pending requests (call in a loop) |
| `fluxipc_stop()` | Signal the poll loop to exit |
| `fluxipc_destroy()` | Release all resources (stops MCP automatically) |
| `fluxipc_usage(prog)` | Print all endpoints to stdout |

### Client API

| Function | Description |
|----------|-------------|
| `fluxipc_client_dispatch(argc, argv)` | Resolve argv[0] symlink, execute IPC call, return 0 |
| `fluxipc_client_resolve(argv0, ...)` | Parse symlink into prog/sock/shm parameters |
| `fluxipc_call(sock, path, argc, argv, out, cap, len)` | Low-level single IPC call |
| `fluxipc_list_endpoints(prog, cb, data)` | Enumerate active endpoints via callback |
| `fluxipc_interactive_init(prog)` | Enter readline REPL |

### Compile-time Macro

```c
FLUXIPC_STATIC(path, usage, slot_sz, handler_fn, ctx)
```

Places an `fluxipc_entry_t` in the `fluxipc_registry` ELF section.

---

## Building as a Dependency

```bash
gcc $(pkg-config --cflags fluxipc) myapp.c -o myapp \
    $(pkg-config --libs fluxipc) -Wl,--export-dynamic
```

---

## Source Layout

```
FluxIPC/
├── include/
│   ├── fluxipc.h              # Public API
│   ├── fluxipc_internal.h     # Internal data structures
│   └── (MCP declarations in fluxipc_internal.h, not public)
├── src/
│   ├── fluxipc_core.c         # Library entry, lifecycle
│   ├── fluxipc_tree.c         # Namespace tree (insert/find/remove/prune)
│   ├── fluxipc_shm.c          # POSIX shared memory registry
│   ├── fluxipc_sock.c         # Unix socket transport
│   ├── fluxipc_symlink.c      # Symlink management
│   ├── fluxipc_interactive.c  # Interactive REPL shell
│   └── fluxipc_mcp.c          # MCP HTTP/JSON-RPC server
├── cli/
│   └── fluxipc_cli.c          # CLI entry point
├── tests/
│   └── example_server.c       # Full demo with multiple subsystems
├── python/
│   ├── fluxipc.py             # Python MCP client
│   └── test_mcp.py            # Integration test suite
├── packaging/
│   └── install.sh             # Install/uninstall helper
├── Makefile
└── README.md
```

---

## Design Notes

- **Namespace tree** — internal nodes are containers; only leaf nodes carry handlers. Removing a leaf prunes empty ancestors automatically.
- **Shared memory** — one fixed-size `fluxipc_shm_t` per server process, containing a process-shared `pthread_rwlock_t` so concurrent readers are safe without external locking.
- **Symlink resolution** — `argv[0]` under `/run/<prog>-fluxipc/...` encodes both the server identity and the IPC path, enabling zero-config client invocation.
- **No daemon required** — the server embeds the IPC machinery as a library; `fluxipc_poll()` is called from the application's own event loop.
- **MCP server** — hand-rolled HTTP + JSON parser (zero external deps), speaks both Streamable HTTP (2025-03-26) and legacy SSE (2024-11-05) MCP transports on a single TCP port.
