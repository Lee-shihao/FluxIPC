#!/usr/bin/env python3
"""
test_mcp.py – end-to-end integration test for FluxIPC MCP server

Connects to an already-running FluxIPC MCP server and exercises every
MCP method through the Python client.

Usage:
    python3 python/test_mcp.py                           # default: localhost:32100
    python3 python/test_mcp.py --host 192.168.1.100      # remote host
    python3 python/test_mcp.py --port 3002               # custom port
    python3 python/test_mcp.py --host 10.0.0.5 -p 8080   # remote host + port
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from fluxipc import FluxIPC, FluxIPCError

# ── helpers ──────────────────────────────────────────────────────────────── #

PASS = "\033[32m✓\033[0m"
FAIL = "\033[31m✗\033[0m"

_failures = 0

def ok(label: str, value: str = "") -> None:
    print(f"  {PASS}  {label}" + (f"  → {value!r}" if value else ""))

def fail(label: str, reason: str) -> None:
    global _failures
    _failures += 1
    print(f"  {FAIL}  {label}  ✗  {reason}")

def check(label: str, expr: bool, detail: str = "") -> None:
    if expr:
        ok(label, detail)
    else:
        fail(label, detail or "assertion failed")

# ── test cases ───────────────────────────────────────────────────────────── #

def test_initialize(ipc: FluxIPC) -> None:
    print("\n── initialize (done in constructor) ──")
    ok("connection established")
    ok("MCP handshake complete")

def test_ping(ipc: FluxIPC) -> None:
    print("\n── ping ──")
    check("ping returns True", ipc.ping() is True)

def test_tools_list(ipc: FluxIPC) -> list[dict]:
    print("\n── tools/list ──")
    tools = ipc.list_tools()
    check("tools is a list",    isinstance(tools, list))
    check("at least 10 tools",  len(tools) >= 10, f"got {len(tools)}")
    # spot-check expected names
    names = {t["name"] for t in tools}
    for expected in ("system_info", "system_uptime", "demo_echo",
                     "demo_arithmetic_add", "module_sensor_status"):
        check(f"  has tool '{expected}'", expected in names)
    # verify schema shape
    for t in tools[:3]:
        check(f"  '{t['name']}' has inputSchema",
              "inputSchema" in t)
    return tools

def test_tools_call_basic(ipc: FluxIPC) -> None:
    print("\n── tools/call – basic ──")

    r = ipc.call("/system/info")
    check("system/info contains 'pid='", "pid=" in r, r)

    r = ipc.call("/system/uptime")
    check("system/uptime contains 'uptime_seconds='", "uptime_seconds=" in r, r)

    r = ipc.call("/system/version")
    check("system/version contains 'version='", "version=" in r, r)

def test_tools_call_args(ipc: FluxIPC) -> None:
    print("\n── tools/call – with arguments ──")

    r = ipc.call("/demo/arithmetic/add", "7", "3")
    check("add 7+3 = 10", "10" in r, r)

    r = ipc.call("/demo/arithmetic/mul", "6", "7")
    check("mul 6×7 = 42", "42" in r, r)

    r = ipc.call("/demo/echo", "hello", "world")
    check("echo returns both args", "hello" in r and "world" in r, r)

def test_tools_call_stateful(ipc: FluxIPC) -> None:
    print("\n── tools/call – stateful / side-effects ──")

    # counter should increment
    c1 = ipc.call("/demo/counter")
    c2 = ipc.call("/demo/counter")
    v1 = int(c1.split("=")[1])
    v2 = int(c2.split("=")[1])
    check("counter increments", v2 == v1 + 1, f"{v1} → {v2}")

    # sensor calibrate with arg
    r = ipc.call("/module/sensor/calibrate", "1.25")
    check("calibrate accepts offset arg", "1.25" in r, r)

    # dynamic device
    r = ipc.call("/devices/stub/primary/info")
    check("dynamic device info", "stub-primary" in r, r)

def test_tools_call_errors(ipc: FluxIPC) -> None:
    print("\n── tools/call – error handling ──")

    try:
        ipc.call("/nonexistent/path")
        fail("unknown tool", "expected FluxIPCError, got nothing")
    except FluxIPCError as e:
        check("unknown tool raises FluxIPCError", True, str(e))

    # motor/stop disables motor, then speed should return error
    ipc.call("/module/motor/stop")
    r = ipc.call("/module/motor/speed")
    check("disabled motor returns error text", "error" in r, r)

def test_path_formats(ipc: FluxIPC) -> None:
    print("\n── path format tolerance ──")

    # leading slash optional
    r1 = ipc.call("/system/info")
    r2 = ipc.call("system/info")     # no leading slash
    check("leading slash optional", r1 == r2, r2[:30])

# ── main ─────────────────────────────────────────────────────────────────── #

def main() -> int:
    parser = argparse.ArgumentParser(
        description="FluxIPC MCP client integration test",
    )
    parser.add_argument(
        "--host", default="localhost",
        help="Server hostname or IP address (default: localhost)",
    )
    parser.add_argument(
        "--port", "-p", type=int, default=32100,
        help="Server TCP port (default: 32100)",
    )
    parser.add_argument(
        "--timeout", type=float, default=10.0,
        help="Socket timeout in seconds (default: 10)",
    )
    args = parser.parse_args()

    print(f"Connecting to FluxIPC MCP server at {args.host}:{args.port} …")
    try:
        ipc = FluxIPC(args.host, args.port, timeout=args.timeout)
    except (ConnectionRefusedError, OSError) as exc:
        print(f"\n\033[31mERROR:\033[0m Cannot connect to {args.host}:{args.port} — {exc}")
        print("       Make sure the FluxIPC MCP server is already running.")
        return 1

    try:
        test_initialize(ipc)
        test_ping(ipc)
        test_tools_list(ipc)
        test_tools_call_basic(ipc)
        test_tools_call_args(ipc)
        test_tools_call_stateful(ipc)
        test_tools_call_errors(ipc)
        test_path_formats(ipc)
    except Exception as exc:
        print(f"\nFATAL: {exc}")
        import traceback; traceback.print_exc()
        global _failures
        _failures += 1
    finally:
        ipc.close()

    print(f"\n{'─'*40}")
    if _failures == 0:
        print(f"\033[32mAll tests passed.\033[0m")
    else:
        print(f"\033[31m{_failures} test(s) FAILED.\033[0m")
    return _failures


if __name__ == "__main__":
    sys.exit(main())
