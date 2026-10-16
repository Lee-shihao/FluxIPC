# FluxIPC

**English | [简体中文](README.zh-CN.md)**

A lightweight, namespace-tree IPC framework for Linux with a standard HTTP
interface.

Endpoints live in a UNIX-style namespace tree (`/devices/stub/primary/value`,
`/module/sensor/status`).  Containers are namespaces; leaves are callable
methods.  Every leaf is reachable through **two transports that share one
handler**:

- **HTTP/1.1** — REST-style, JSON bodies, standard methods, status codes,
  `Allow`, CORS, and RFC 7807 error bodies.
- **Unix socket** — local argv calls used by the CLI and the interactive shell.

There is no compile-time registration: the tree is built at runtime, node by
node.  No external daemon is required.

---

## Quick Start

### Prerequisites

- Linux (`/run`, Unix Domain Sockets)
- GCC
- GNU Readline
- cJSON (`libcjson`)

```bash
# Debian / Ubuntu
sudo apt install libreadline-dev libcjson-dev

# Fedora / RHEL
sudo dnf install readline-devel cjson-devel
```

### Build

```bash
make -j$(nproc)

make lib        # shared library only (libfluxipc.so.1.0.0)
make static     # static library only (libfluxipc.a)
make cli        # CLI tool only (fluxipc-cli)
make examples   # example server (example_server)
make debug      # verbose HTTP logging (HTTP_DEBUG=1)
```

### Install

```bash
sudo make install                   # PREFIX=/usr
sudo make install PREFIX=/usr/local
sudo make uninstall
```

### Run the Demo

```bash
# Terminal 1
./example_server

# Terminal 2 — HTTP
curl -s http://localhost:32100/system/info
curl -s http://localhost:32100/                 # leaf manifest
curl -s http://localhost:32100/openapi.json

# Terminal 2 — local socket / CLI
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
```

---

## Server-Side Usage

### 1. Register endpoints

Registration is dynamic and **path-based**: the helpers create every missing
intermediate container for you.  `fluxipc_register()` keeps the classic
`main`-branch signature `(path, usage, slot_sz, handler, data)` and registers
an IPC endpoint; `fluxipc_register_method()` takes a full `fluxipc_method_t`
(IPC and/or HTTP); `fluxipc_register_http()` attaches the HTTP side; and
`fluxipc_register_all()` registers a whole static table.  All of them copy the
method descriptor (handlers, `summary`, `description`, `schema`), so passing a
stack temporary or a `FLUXIPC_METHOD` compound literal is safe.

```c
#include <fluxipc.h>

/* IPC: positional argv → formatted text (the original contract) */
static int hello_ipc(void *data, int argc, char **argv,
                     void *out, size_t cap, size_t *len)
{
    const char *name = (argc > 1) ? argv[1] : "World";
    int n = snprintf(out, cap, "Hello, %s!", name);
    *len = (n > 0) ? (size_t)n : 0;
    return 0;
}

/* HTTP: parsed request → structured JSON */
static int hello_http(void *data, const fluxipc_http_request_t *req,
                      fluxipc_http_response_t *resp)
{
    const char *name = "World";
    const cJSON *v = req->json
        ? cJSON_GetObjectItemCaseSensitive(req->json, "name") : NULL;
    if (cJSON_IsString(v) && v->valuestring) name = v->valuestring;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", "Hello");
    cJSON_AddStringToObject(o, "name", name);
    resp->json = o;              /* server takes ownership + serialises */
    return 0;
}

int main(void)
{
    fluxipc_server_init("myapp", 32100);            /* 0 disables HTTP */

    /* Full method in one call: metadata + both handlers. */
    fluxipc_register_method("/greet/hello", &FLUXIPC_METHOD(
        hello_ipc,          /* socket / CLI: raw bytes */
        hello_http,         /* HTTP: JSON              */
        "Say hello",
        "{\"type\":\"object\",\"properties\":"
        "{\"name\":{\"type\":\"string\"}}}",
        FLUXIPC_M_GET | FLUXIPC_M_POST));

    /* ...or the classic main-branch form (IPC only). */
    fluxipc_register("/greet/plain", "Say hello", 0, hello_ipc, NULL);

    while (fluxipc_poll(NULL) >= 0) { /* socket + HTTP */ }

    fluxipc_destroy();
}
```

For a larger server, a static table is even tidier:

```c
static const fluxipc_endpoint_t endpoints[] = {
    { "/system/info", FLUXIPC_METHOD(info_ipc, info_http,
                                      "Process info", NULL, FLUXIPC_M_GET) },
    { "/demo/add",    FLUXIPC_METHOD(add_ipc, add_http,
                                      "Add two integers", NULL,
                                      FLUXIPC_M_GET | FLUXIPC_M_POST) },
};

fluxipc_register_all(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
```

`FLUXIPC_METHOD_D(ipc, http, data, summary, schema, methods)` is the same but
also sets the per-leaf `data` pointer for stateful handlers.

#### `main`-branch compatibility

Code written for the `main` branch's interfaces compiles and links unchanged:

- `fluxipc_register(path, usage, slot_sz, handler, data)` — `usage` becomes the
  leaf `summary`; `slot_sz` (the old shared-memory slot size) is accepted and
  ignored; the handler's `void *out_buf` parameter matches the public
  `fluxipc_handler_fn`;
- `fluxipc_unregister(node)` is kept as an alias for
  `fluxipc_node_remove(node)`;
- `FLUXIPC_STATIC(path, usage, slot_sz, handler, ctx)` declares a compile-time
  endpoint.  Entries are queued by a constructor before `main()` and installed
  by `fluxipc_server_init()`, so **no `-rdynamic` and no linker section are
  needed**.

```c
static int hello_ipc(void *d, int argc, char **argv,
                     void *out, size_t cap, size_t *len) { /* ... */ }

FLUXIPC_STATIC("/greet/compiled", "say hello", 0, hello_ipc, NULL);
```

#### Split registration (IPC and HTTP independently)

Registering the same path again **merges** into the existing leaf: only
still-empty fields are filled, so an IPC-only registration can be completed
later with `fluxipc_register_http()` — and vice versa.  The HTTP method mask is
owned by the call that first attaches an HTTP handler, and existing non-empty
handlers, metadata and `data` are never overwritten.

```c
/* Classic / IPC-only registration (http may also be NULL in a method). */
fluxipc_register("/greet/hello", "Say hello", 0, hello_ipc, NULL);

/* Add the HTTP side later (ipc is NULL). */
fluxipc_register_http("/greet/hello", &FLUXIPC_METHOD(
    NULL, hello_http, "Say hello", NULL, FLUXIPC_M_GET));
```

A failed registration is atomic: it never leaves half-created containers
behind and never prunes containers you created yourself.

Compile/link:

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs fluxipc) -lcjson
```

#### Lower-level node API

The registration helpers are thin conveniences over the node primitives, which
stay available when the tree must be mutated at runtime:

| Function | Description |
|----------|-------------|
| `fluxipc_tree_root()` | Root node (after `fluxipc_server_init`) |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | Classic main-branch registration (IPC) |
| `fluxipc_register_method(path, &method)` | Register a full method; merges on repeat |
| `fluxipc_register_http(path, &method)` | Register the HTTP side (merges, `ipc` usually NULL) |
| `fluxipc_register_all(table, count)` | Register a static endpoint table |
| `fluxipc_node_mkdir(parent, name)` | Get-or-create a container child (idempotent) |
| `fluxipc_node_leaf(parent, name, &method)` | Create a leaf method under `parent` |
| `fluxipc_node_find(parent, name)` | Look up a direct child |
| `fluxipc_node_remove(node)` / `fluxipc_unregister(node)` | Remove a leaf/subtree; prunes empty ancestors |
| `fluxipc_node_leaves(node, cb, user)` | **Built-in on every node**: enumerate all leaves at/below it |

### 3. Handler signatures

A leaf carries **two independent handlers**. Register at least one; either may
be `NULL` (the corresponding transport then reports *not implemented*).

**IPC handler** — positional argv, raw output buffer (the original contract):

```c
typedef int (*fluxipc_handler_fn)(void *data, int argc, char **argv,
                                  void *out_buf, size_t out_cap, size_t *out_len);
```

**HTTP handler** — parsed request, cJSON response:

```c
typedef int (*fluxipc_http_handler_fn)(void *data,
                                       const fluxipc_http_request_t *req,
                                       fluxipc_http_response_t *resp);
```

`fluxipc_http_request_t` carries the standard request fields plus the parsed
JSON body:

| Field | Meaning |
|-------|---------|
| `method` | `"GET"`, `"POST"`, … |
| `path` | decoded namespace path, e.g. `/system/info` |
| `query` / `query_count` | query parameters |
| `headers` / `header_count` | HTTP headers |
| `content_type`, `body`, `body_len` | raw body |
| `json` | parsed request body (`const cJSON *`), or `NULL` |
| `data` | the method's `data` pointer |

`fluxipc_http_response_t` has `status` (0 → 200), an owned `cJSON *json`, and
an optional `content_type`.

The two transports never share a return style: **IPC returns raw bytes, HTTP
returns JSON.**

---

## HTTP Interface

The tree maps directly onto URL paths.  Leaves are resources; containers are
collections.

```bash
# Read a leaf
curl -s http://localhost:32100/system/info

# Write a leaf (PUT/POST with a JSON body)
curl -s -X PUT -H 'Content-Type: application/json' \
     -d '{"rate_hz":20}' http://localhost:32100/module/sensor/config

# Query parameters
curl -s 'http://localhost:32100/demo/arithmetic/add?a=7&b=3'

# List all leaves under a container (built-in on every node)
curl -s http://localhost:32100/module
curl -s 'http://localhost:32100/module?recursive=0'   # direct leaves only

# Discover allowed methods
curl -s -X OPTIONS -i http://localhost:32100/system/info

# Machine-readable schema for every endpoint
curl -s http://localhost:32100/openapi.json
```

| Method | On a leaf | On a container |
|--------|-----------|----------------|
| `GET` | invoke handler (read) | leaf manifest |
| `POST` | invoke handler (action) | `405` + `Allow` |
| `PUT` / `PATCH` / `DELETE` | invoke handler | `405` + `Allow` |
| `HEAD` | like `GET`, no body | like `GET`, no body |
| `OPTIONS` | `204` + `Allow` | `204` + `Allow` |

**Standards used:** `Content-Type: application/json; charset=utf-8`,
`Content-Length`, `Allow`, CORS, `application/problem+json` errors
(RFC 7807), and OpenAPI 3.1.

Example manifest (`GET /system`):

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

Example error (`GET` on a POST-only leaf):

```json
{
  "type": "about:blank",
  "title": "Method Not Allowed",
  "status": 405,
  "detail": "method not allowed for this endpoint",
  "instance": "/system/shutdown"
}
```

### Usage / documentation metadata

Every method carries a `summary`, an optional `description`, and an optional
JSON Schema `schema`.  These are returned:

- in the container leaf manifest,
- in the generated OpenAPI document,
- in the interactive shell / `--list` output.

There are **no auto-generated `_usage` sibling leaves**: metadata belongs to the
leaf's representation, not to a separate resource.  This keeps the namespace
unpolluted, avoids name collisions, and needs no extra registrations.  If you
want a dedicated resource you can always register one explicitly.

---

## Local (Unix Socket) Interface

Same handler, different transport.

### CLI

```bash
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
fluxipc-cli example_server --list
```

### Interactive shell

```bash
fluxipc-cli example_server --interactive
```

| Command | Description |
|---------|-------------|
| `ls` | list leaves under the current namespace |
| `cd <path>` | change namespace |
| `help [path]` | built-in help / endpoint details |
| `reload` | re-scan running servers and refresh the registry |
| `watch [s] <path>` | call endpoint every N seconds |
| `Tab` | path completion |

---

## Python Client

Stdlib-only REST client:

```python
from fluxipc import FluxIPC

ipc = FluxIPC("localhost", 32100)
print(ipc.get("/system/info"))
print(ipc.get("/demo/arithmetic/add", a=7, b=3))
ipc.put("/module/sensor/config", {"rate_hz": 20})
print(ipc.list("/module"))          # leaf manifest
print(ipc.openapi())                # OpenAPI document
```

Integration tests:

```bash
./example_server &
python3 python/test_http.py
python3 python/test_http.py --host 192.168.1.100 --port 32100
```

---

## Testing & Quality

The whole test toolchain is installed **locally** into `.tools/` — no root
required:

```bash
make tools      # download+extract Mull 0.34.1, cmocka 1.1.5, lcov into .tools/
```

| Command | What it does |
|---------|--------------|
| `make ut` | **cmocka** unit suite → `fluxipc-ut` (170 cases) |
| `make it` | end-to-end suite → `fluxipc-it` (forks `example_server`) |
| `make test` | `ut` + `it` |
| `make cov` | instrumented build + text & HTML coverage report, line/branch gates |
| `make mutate` | **Mull** mutation testing with a mutation-score gate |
| `make ut-asan` | AddressSanitizer + UBSan build (leak-checked) |

Tunable knobs (env vars):

```bash
COV_LINE_MIN=90  COV_BRANCH_MIN=78  MUT_SCORE_MIN=70  MUT_WORKERS=4
```

### Current quality gates

| Gate | Target | Actual |
|------|--------|--------|
| Line coverage (`src/`) | ≥ 90% | **95.8%** |
| Branch coverage (`src/`) | ≥ 78% | **79.1%** |
| Mutation score | > 70% | **78%** |
| ASan + UBSan | clean | clean |

`make cov` prints two text tables (line and branch, with the uncovered
line/branch numbers) and also writes `build/coverage/index.html`,
`cobertura.xml` and `summary.json`.  `make mutate` writes its report to
`build/mutation/`.

### Unit suites (cmocka, `tests/unit/`)

| Suite | Covers |
|-------|--------|
| `protocol` | pinned 12-byte wire-header layout (client/server cannot drift) |
| `util` | URL decoding, query parsing, non-mutating `Content-Length` scan |
| `tree` | namespace tree: create/find/remove, name-slot conflicts, deep paths |
| `core` | lifecycle, registration, manifest, dispatch, request hooks |
| `sock` | `sock_conn_process` (fake conn), client helpers, malformed/truncated replies |
| `conn` | pool + non-blocking read/write state machine (socketpairs) |
| `http` | parser, routing, `Allow`, RFC 7807, OpenAPI, HEAD/OPTIONS/errors |
| `interactive` | path trie, range expansion, Tab completion, REPL commands |
| `cli` | argument modes and output |

Some modules are tested white-box: the transport processors are driven through
an in-memory `fluxipc_conn_t`, and `fluxipc_interactive.c` / `fluxipc_cli.c` are
`#include`d by their test translation units so `static` helpers can be reached.

### Integration tests (`tests/integration/`)

Forks `example_server` and exercises both transports end-to-end: IPC calls
(with arguments and error propagation), `OP_LIST` discovery, HTTP
200/404/405/`problem+json`, and the connect-immediately-after-start regression.

### Python client tests

The stdlib REST client is covered by `python/test_http.py` against a running
server (see below).

---

## File Locations

| Path | Purpose |
|------|---------|
| `/run/user/<uid>/<prog>-fluxipc/<prog>.sock` | Unix domain socket (data + introspection) |

---

## API Reference

### Server / tree

| Function | Description |
|----------|-------------|
| `fluxipc_server_init(prog, http_port)` | Init socket + optional HTTP (0 = off) |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | Classic main-branch registration (IPC) |
| `fluxipc_register_method(path, method)` | Register a full method; merges on repeat |
| `fluxipc_register_http(path, method)` | Register the HTTP side of an endpoint |
| `fluxipc_register_all(table, count)` | Register a static `fluxipc_endpoint_t` table |
| `FLUXIPC_STATIC(path, usage, slot, handler, data)` | Compile-time endpoint (constructor-queued) |
| `fluxipc_tree_root()` | Root node |
| `fluxipc_node_mkdir(parent, name)` | Get-or-create container |
| `fluxipc_node_leaf(parent, name, method)` | Register a leaf |
| `fluxipc_node_find(parent, name)` | Find a child |
| `fluxipc_node_remove(node)` | Remove leaf/subtree |
| `fluxipc_node_leaves(node, cb, user)` | Enumerate descendant leaves |
| `fluxipc_node_path/name/method/is_leaf` | Node accessors |
| `fluxipc_poll(NULL)` | Dispatch pending socket + HTTP requests |
| `fluxipc_stop()` / `fluxipc_destroy()` | Shutdown |
| `fluxipc_usage(prog)` | Print endpoints |
| `fluxipc_set_request_hooks(pre, post, user)` | Observe requests |

### Client

| Function | Description |
|----------|-------------|
| `fluxipc_call(sock, path, argc, argv, out, cap, len)` | Low-level socket call |
| `fluxipc_list_endpoints(prog, cb, data)` | Enumerate via the server socket |
| `fluxipc_interactive_init(prog)` | Enter the readline REPL |

---

## Source Layout

```
FluxIPC/
├── include/
│   ├── fluxipc.h              # Public API (tree, handlers, HTTP client)
│   └── fluxipc_internal.h     # Internal data structures
├── src/
│   ├── fluxipc_core.c         # Lifecycle, registration, dispatch
│   ├── fluxipc_tree.c         # Namespace tree
│   ├── fluxipc_http.c         # Standard HTTP/1.1 + REST + OpenAPI
│   ├── fluxipc_sock.c         # Unix socket transport + protocol
│   ├── fluxipc_conn.c         # Unified non-blocking event loop
│   ├── fluxipc_util.c         # Pure parsing helpers (URL/query/length)
│   └── fluxipc_interactive.c  # Interactive REPL
├── cli/fluxipc_cli.c          # CLI entry point
├── tests/
│   ├── example_server.c       # Demo server (dual handlers)
│   ├── unit/                  # cmocka unit suites (fluxipc-ut)
│   │   ├── test.h / ut_common.*   # shared fixtures & helpers
│   │   └── test_{protocol,util,tree,core,sock,conn,http,interactive,cli}.c
│   ├── integration/           # End-to-end tests (fluxipc-it)
│   ├── tools/                 # bootstrap.sh, coverage.sh, mutate.sh, env.sh
│   └── UNIT_TEST_PLAN.md      # test strategy & metrics
├── python/
│   ├── fluxipc.py             # Python REST client
│   └── test_http.py           # Python HTTP tests
├── mull.yml                   # Mull mutation-testing configuration
├── Makefile
├── README.md                  # English
└── README.zh-CN.md            # 简体中文
```

---

## Design Notes

- **One tree, two handlers, two transports** — HTTP and the Unix socket both
  resolve the same tree; each invokes the leaf's own `http` or `ipc` handler.
- **Node-based registration** — no ELF sections, no `-rdynamic`, no full-path
  strings; containers and leaves are added one segment at a time.
- **Built-in leaf listing** — every node can enumerate its leaves; HTTP exposes
  it as `GET /<container>`, the socket via the protocol's `LIST` op.
- **REST + OpenAPI** — standard methods, status codes, `Allow`, and RFC 7807
  errors; `/openapi.json` describes every leaf.
- **Unified non-blocking event loop** (`fluxipc_conn.c`) — one `select()` drives
  both listeners and every connection through a shared buffered state machine,
  so no peer can block the server.
