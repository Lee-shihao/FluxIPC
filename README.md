# FluxIPC

FluxIPC is a lightweight **namespace-tree IPC library for Linux**. A process
embeds the library, registers callable leaves under a path hierarchy, and
serves them over a Unix domain socket. There is no daemon, no HTTP server and
no JSON layer: a call is positional `argv` in, raw bytes out.

The library creates **no threads of its own**. The host application decides
whether to run the event loop in a dedicated thread, integrate it into an
existing scheduler, or drive it manually.

## Features

- Hierarchical endpoint namespace (`/system/info`, `/devices/io/led/value`, …)
- Path-based registration with automatic container creation
- Classic `fluxipc_register(path, usage, slot_sz, handler, data)` compatibility
- Compile-time endpoint registration with `FLUXIPC_STATIC`
- Runtime add/remove; empty ancestors are pruned automatically
- Compact 12-byte wire protocol over a Unix socket
- Non-blocking, buffered, one-request-per-connection server
- Host-owned event loop:
  - `fluxipc_run()` for a thread you create
  - `fluxipc_poll_timeout(ms)` for your own scheduler
  - `fluxipc_wakeup()` / `fluxipc_stop()` from any thread
- Thread-safe registration/removal and client calls
- CLI, interactive shell, unit/integration tests and benchmark

## Quick Start

### Dependencies

- GCC or Clang
- GNU make
- `libreadline` (interactive shell)
- `libpthread`, `librt` (usually part of libc)

```bash
# Debian / Ubuntu
sudo apt install build-essential libreadline-dev

# Fedora / RHEL
sudo dnf install gcc make readline-devel
```

### Build

```bash
make -j$(nproc)
```

Targets:

```bash
make lib        # shared library: libfluxipc.so.1.0.0
make static     # static library: libfluxipc.a
make cli        # fluxipc-cli
make examples   # example_server
```

### Install

```bash
sudo make install              # PREFIX=/usr
sudo make install PREFIX=/usr/local
```

### Run the demo

```bash
# Terminal 1
./example_server

# Terminal 2
fluxipc-cli example_server
fluxipc-cli example_server -c /system/info
fluxipc-cli example_server -c /demo/arithmetic/add 7 3
fluxipc-cli example_server -i
```

## Server Usage

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
    fluxipc_run();          /* returns after fluxipc_stop() */
    return NULL;
}

int main(void)
{
    int rc = fluxipc_server_init("myapp");
    if (rc < 0) return 1;

    fluxipc_register_method("/demo/add",
        &FLUXIPC_METHOD(add_ipc, "Add two integers", NULL));

    /* Option A: run the loop in a thread you own. */
    pthread_t th;
    pthread_create(&th, NULL, serve, NULL);

    /* ... host application work ... */

    fluxipc_stop();
    pthread_join(th, NULL);
    fluxipc_destroy();
    return 0;
}
```

A handler receives:

- `argc` / `argv`: `argv[0]` is the endpoint path, followed by call arguments
- `out` / `cap`: output buffer (64 KiB in the socket server)
- `out_len`: number of bytes written

Return `0` on success or a negative errno. A missing endpoint yields
`-ENOENT`.

### Registration styles

```c
/* Path-based; intermediate containers are created automatically. */
fluxipc_register_method("/module/sensor/status",
    &FLUXIPC_METHOD(status_ipc, "Read the sensor", NULL));

/* Per-leaf data pointer. */
fluxipc_register_method("/devices/led/value",
    &FLUXIPC_METHOD_D(value_ipc, &led_ctx, "Get/set LED value", NULL));

/* Classic main-branch signature (slot_sz is accepted and ignored). */
fluxipc_register("/legacy/ping", "Ping", 0, ping_ipc, NULL);

/* Static table. */
static const fluxipc_endpoint_t table[] = {
    { "/a", FLUXIPC_METHOD(a_ipc, "A", NULL) },
    { "/b", FLUXIPC_METHOD(b_ipc, "B", NULL) },
};
fluxipc_register_all(table, sizeof(table) / sizeof(table[0]));

/* Compile-time registration (applied during fluxipc_server_init). */
FLUXIPC_STATIC("/system/compiled", "Compiled endpoint", 0, compiled_ipc, NULL);
```

Registering the same path again merges metadata: still-empty `summary`,
`description` and `data` are filled; an existing handler is never overwritten.

### Runtime mutation

```c
fluxipc_node_t *root = fluxipc_tree_root();
fluxipc_node_t *dir  = fluxipc_node_mkdir(root, "dynamic");
fluxipc_node_leaf(dir, "leaf", &FLUXIPC_METHOD(fn, "dynamic leaf", NULL));

fluxipc_node_remove(dir);   // removes the subtree and prunes empty ancestors
```

`fluxipc_node_find()`, `fluxipc_node_path()`, `fluxipc_node_name()`,
`fluxipc_node_method()` and `fluxipc_node_is_leaf()` are available for
inspection. Handles returned by lookup functions are borrowed; do not use one
concurrently with removal.

## Threading / Scheduling

```c
/* Dedicated host thread. */
static void *serve(void *arg) { fluxipc_run(); return NULL; }
pthread_create(&th, NULL, serve, NULL);

/* Or integrate with your own scheduler. */
for (;;) {
    int rc = fluxipc_poll_timeout(100);   /* ms; <0 = infinite, 0 = poll */
    if (rc < 0) break;
    /* do other periodic work */
}

fluxipc_wakeup();   /* interrupt a blocking poll */
fluxipc_stop();     /* request loop exit */
pthread_join(th, NULL);
fluxipc_destroy();  /* only after the poll thread has exited */
```

Guarantees:

- The library never creates a thread.
- Registration/removal may run while the poll thread is running.
- Exactly one thread may poll at a time; a concurrent poll returns `-EBUSY`.
- `fluxipc_call()` and `fluxipc_list_endpoints()` are thread-safe clients.
- Handlers execute on the polling thread.
- Hooks may be replaced safely from any thread; a request keeps the pair that
  was current when it started.

## Client API

```c
char out[4096];
size_t len = 0;
char *argv[] = { "/demo/add", "7", "3" };

int rc = fluxipc_call(
    "/run/user/1000/myapp-fluxipc/myapp.sock",
    "/demo/add", 3, argv, out, sizeof(out), &len);
```

`fluxipc_call()` opens a fresh connection per call, so independent threads can
call concurrently.

Endpoint discovery:

```c
static void on_leaf(const char *path, uint32_t id,
                    const char *usage, void *user)
{
    printf("%u\t%s\t%s\n", id, path, usage);
}

fluxipc_list_endpoints("myapp", on_leaf, NULL);
```

The `LIST` operation returns a compact text manifest
(`<id>\t<path>\t<summary>\n`) directly over the Unix socket.

## CLI

```
fluxipc-cli <server>                          list endpoints
fluxipc-cli <server> -l, --list               list endpoints
fluxipc-cli <server> -c, --call <path> [args] call one endpoint
fluxipc-cli <server> -i, --interactive        REPL with tab completion
```

The REPL supports `ls`, `cd`, `help`, `watch`, `reload`, relative paths and
inline ranges such as `1:10`.

## Testing & Quality

```bash
make ut          # cmocka unit suites
make it          # end-to-end tests against example_server
make test        # both
make test-mem    # ASan/UBSan + LeakSanitizer
make bench       # p50/p95/p99, ops/s, RSS
```

Benchmark knobs:

```bash
BENCH_IPC_ITERS=20000 BENCH_THREADS=8 make bench
BENCH_MIN_IPC_OPS=10000 BENCH_MAX_P99_MS=1.0 make bench
```

## File Locations

| Item | Path |
| --- | --- |
| Public header | `include/fluxipc.h` |
| Internal header | `include/fluxipc_internal.h` |
| Shared library | `libfluxipc.so.1.0.0` |
| Static library | `libfluxipc.a` |
| Runtime dir | `/run/user/<uid>/<prog>-fluxipc/` |
| Socket | `/run/user/<uid>/<prog>-fluxipc/<prog>.sock` |
| Install header | `$(PREFIX)/include/fluxipc/fluxipc.h` |

## Source Layout

```
include/
  fluxipc.h              public API
  fluxipc_internal.h     wire protocol + internal types
src/
  fluxipc_core.c         lifecycle, registration, manifest, dispatch, loop
  fluxipc_tree.c         namespace tree
  fluxipc_sock.c         Unix-socket transport + client
  fluxipc_conn.c         non-blocking connection pool / select loop
  fluxipc_interactive.c  REPL
cli/
  fluxipc_cli.c          CLI entry point
tests/
  example_server.c       IPC-only demo with host-owned thread
  unit/                  cmocka suites
  integration/           end-to-end IPC tests
  bench/                 latency / throughput baseline
```

## Design Notes

- **Single poller, many registrars.** The connection pool is driven by one
  thread; the tree is mutex-protected so registration can happen anywhere.
- **One request per connection.** This keeps the state machine small and makes
  client-side parallelism trivial.
- **Bounded resources.** 32 concurrent connections, 512 KiB per-request cap,
  64 KiB handler output, 30 s idle timeout.
- **No allocation on the hot path.** The server reuses one IPC output buffer;
  connection buffers grow only as required.
- **Text manifest.** Listing stays dependency-free while remaining easy to
  inspect and parse.

## Roadmap

- Access control / peer credential checks
- Optional worker-pool dispatch for blocking handlers
- Subscription/event push alongside request/response
- Fuzz targets for the wire parser
