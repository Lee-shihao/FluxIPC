# FluxIPC

A lightweight, namespace-tree IPC library for Linux.  
Endpoints are organised as a UNIX-style path hierarchy (`/devices/stub/xxx/name`, `/module/sensor/status`), registered at compile-time or at runtime, and discovered via POSIX shared memory.  The CLI client gains full tab-completion from the shared-memory registry.

---

## Architecture overview

```
┌───────────────────────────────────────────────────────────────────────┐
│  Server process                                                       │
│                                                                       │
│  ELF section "fluxipc_registry"  ──►  namespace tree                  │
│  FLUXIPC_STATIC(...)                     /                            │
│                                        ├─ module/                     │
│  fluxipc_register(...)                 │   └─ sensor/                 │
│  (dynamic, runtime)                    │       ├─ status  ←── handler │
│                                        │       └─ calibrate           │
│                                        └─ devices/                    │
│                                            └─ stub/xxx/               │
│                                                ├─ name   ←── handler  │
│                                                └─ value  ←── handler  │
│                                                                       │
│  ┌─────────────────────┐┌──────────────────────────────────────────┐  │
│  │  Unix socket        ││  POSIX shared memory /fluxipc.<progname> │  │
│  │  /run/<prog>-fluxipc││                                          │  │
│  │  <prog>.sock        ││  fluxipc_shm_t {                         │  │
│  └──────┬──────────────┘│    magic, version, prog_name, sock_path  │  │
│         │               │    entry_count, next_id, rwlock          │  │
│         │               │    entries[512] { path, usage, id, ... } │  │
│         │               │  }                                       │  │
└─────────┼───────────────┴──────────────────────────────────────────┘  │
│         │                                                             │
│         │  Symlinks: /run/<prog>-fluxipc/<path> → binary              │
│         │                                                             │
┌─────────▼──────────────────────┐  ┌───────────────────────────────────┐
│  CLI / client process          │  │  Interactive shell                │
│                                │  │                                   │
│  argv[0] resolved via symlink  │  │  Loads all /dev/shm/fluxipc.*     │
│  → derives prog, socket, path  │  │  → builds completion table        │
│  → sends request over socket   │  │  → readline with tab-complete     │
│  → prints response             │  │  → inline usage hints             │
└────────────────────────────────┘  └───────────────────────────────────┘
```

---

## Wire protocol

All communication uses a streaming Unix domain socket.

```
Request:
  fluxipc_req_hdr_t  (fixed header)
  NUL-separated arg strings  (total_arg_len bytes)

Response:
  fluxipc_resp_hdr_t  (fixed header: status + data_len)
  data_len bytes of payload
```

---

## Quick start

### Build

```bash
sudo apt install libreadline-dev   # Debian/Ubuntu
# or: sudo dnf install readline-devel

make -j$(nproc)                    # all: shared lib, static lib, CLI, examples
```

Individual targets:

```bash
make lib          # shared library only (libfluxipc.so.1.0.0)
make static       # static library only (libfluxipc.a)
make cli          # CLI tool only (fluxipc-cli)
make examples     # example server/client
```

### Install (system-wide)

```bash
sudo make install                  # defaults to PREFIX=/usr
sudo make install PREFIX=/usr/local
```

### Uninstall

```bash
sudo make uninstall                # defaults to PREFIX=/usr
sudo make uninstall PREFIX=/usr/local
```

### Packaging helper

A convenience install/uninstall wrapper is available in `packaging/install.sh`.

```bash
./packaging/install.sh install [prefix]
./packaging/install.sh uninstall [prefix]
```

The helper builds the project, installs files, and writes `/etc/tmpfiles.d/fluxipc.conf`
so runtime directories can be recreated on reboot.

---

## Usage

### Server side

```c
#include <fluxipc/fluxipc.h>

/* ── Compile-time registration ── */
static int my_handler(void *data, int argc, char **argv,
                      void *out, size_t cap, size_t *len) {
    *len = snprintf(out, cap, "hello\n");
    return 0;
}
FLUXIPC_STATIC("/my/app/ping", "Respond with hello", 0, my_handler, NULL);

int main(void) {
    fluxipc_server_init("myapp");           // registers static entries, creates socket+shm

    /* ── Runtime registration ── */
    fluxipc_node_t *node = fluxipc_register(
        "/my/app/dynamic", "Dynamic endpoint", 0, my_handler, NULL);

    while (1) fluxipc_poll(NULL);           // dispatch loop

    fluxipc_unregister(node);
    fluxipc_destroy();
}
```

Compile with:
```bash
gcc server.c -o myapp -lfluxipc -Wl,--export-dynamic
```

`--export-dynamic` (or `-rdynamic`) is required so the linker retains the
`fluxipc_registry` section symbols.

### Client side – direct call

```bash
fluxipc-cli --server myapp /my/app/ping
fluxipc-cli --server myapp /my/app/dynamic arg1 arg2
```

### Client side – symlink invocation

After `fluxipc_server_init` / `fluxipc_register`, symlinks appear at:
```
/run/myapp-fluxipc/my/app/ping
/run/myapp-fluxipc/my/app/dynamic
```

Invoke directly (the symlink points to the binary; argv[0] encodes server+path):
```bash
/run/myapp-fluxipc/my/app/ping
/run/myapp-fluxipc/my/app/dynamic arg1 arg2
```

Or create your own convenience symlink anywhere in `$PATH`:
```bash
ln -s /run/myapp-fluxipc/my/app/ping /usr/local/bin/myapp-ping
myapp-ping
```

### Interactive shell

```bash
fluxipc-cli --interactive           # discovers all running servers
fluxipc-cli --interactive myapp     # focuses on one server
```

Shell features:
- **Tab completion** on IPC paths loaded from shared memory
- **Inline usage** printed when a path is entered without required arguments
- **`ls`** – show all known endpoint paths, IDs, and usage strings
- **`help <path>`** – show detailed info for one endpoint
- **`reload`** – re-scan all shared-memory regions
- **`exit` / `quit`** – exit the shell
- Full readline history (`~/.bash_history` compatible)

### List registered endpoints

```bash
fluxipc-cli --list myapp
```

---

## API reference

### Server

| Function | Description |
|---|---|
| `fluxipc_server_init(prog_name)` | Initialise server: socket, shm, static entries |
| `fluxipc_register(path, usage, slot_sz, handler, data)` | Dynamic registration |
| `fluxipc_unregister(node)` | Remove a dynamic endpoint |
| `fluxipc_poll(NULL)` | Dispatch pending requests (call in a loop) |
| `fluxipc_stop()` | Signal the poll loop to exit |
| `fluxipc_destroy()` | Release all resources |
| `fluxipc_usage(prog)` | Print all endpoints to stdout |

### Client

| Function | Description |
|---|---|
| `fluxipc_client_init(argv0, argc, argv)` | Resolve server from argv[0] symlink |
| `fluxipc_interactive_init(prog_name)` | Enter readline REPL |

### Compile-time macro

```c
FLUXIPC_STATIC(path, usage, slot_sz, handler_fn, ctx)
```

Places an `fluxipc_entry_t` in the `fluxipc_registry` ELF section.

---

## File locations

| Path | Purpose |
|---|---|
| `/run/<prog>-fluxipc/<prog>.sock` | Unix domain socket |
| `/dev/shm/fluxipc.<prog>` | POSIX shared memory registry |
| `/run/<prog>-fluxipc/<path>` | Client discovery symlinks |
| `/etc/tmpfiles.d/fluxipc.conf` | Ensures dirs survive reboot |

---

## Building as a dependency

After installation, use pkg-config to compile and link against FluxIPC:
```bash
gcc $(pkg-config --cflags fluxipc) server.c -o myserver $(pkg-config --libs fluxipc) -Wl,--export-dynamic
```

If you need the static library instead of the shared library, link directly against `libfluxipc.a` and include `-ldl` if required by your toolchain.

---

## Design notes

- **Namespace tree** – internal nodes are plain containers; only leaf nodes carry handlers.  Removing a leaf prunes empty ancestors automatically.
- **Shared memory** – one fixed-size `fluxipc_shm_t` per server process, containing a process-shared `pthread_rwlock_t` so concurrent readers (CLI, other processes) are safe without external locking.
- **Symlink resolution** – `argv[0]` under `/run/<prog>-fluxipc/...` encodes both the server identity and the IPC path, enabling zero-config client invocation.
- **No daemon required** – the server embeds the IPC machinery as a library; `fluxipc_poll()` is called from the application's own event loop.
