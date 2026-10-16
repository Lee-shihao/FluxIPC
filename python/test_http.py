#!/usr/bin/env python3
"""
test_http.py – end-to-end integration test for the FluxIPC HTTP server.

Requires a running server (default: the bundled example_server):

    ./example_server &
    python3 python/test_http.py
    python3 python/test_http.py --host 192.168.1.100 --port 32100
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fluxipc import FluxIPC, FluxIPCError  # noqa: E402

PASS = 0
FAIL = 0


def ok(msg):
    global PASS
    PASS += 1
    print(f"  \033[32m✓\033[0m {msg}")


def fail(msg):
    global FAIL
    FAIL += 1
    print(f"  \033[31m✗\033[0m {msg}")


def check(cond, msg):
    ok(msg) if cond else fail(msg)


def expect_error(fn, status, msg):
    try:
        fn()
        fail(f"{msg} (expected HTTP {status}, got success)")
    except FluxIPCError as e:
        check(e.status == status, f"{msg} (got HTTP {e.status})")


def main():
    ap = argparse.ArgumentParser(description="FluxIPC HTTP integration test")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=32100)
    args = ap.parse_args()

    c = FluxIPC(args.host, args.port)
    print(f"Testing FluxIPC server at {args.host}:{args.port}\n")

    # ── health & discovery ────────────────────────────────────────────────
    check(c.healthz().get("status") == "ok", "GET /healthz")

    root = c.list("/")
    check(root.get("kind") == "collection" and root.get("count", 0) > 10,
          f"GET / returns leaf manifest ({root.get('count')} leaves)")

    sub = c.list("/system", recursive=False)
    check(sub.get("count") == 4, "GET /system?recursive=0 lists 4 direct leaves")

    spec = c.openapi()
    check(spec.get("openapi", "").startswith("3.") and "/system/info" in spec.get("paths", {}),
          "GET /openapi.json returns OpenAPI 3.1 with /system/info")

    # ── reads ─────────────────────────────────────────────────────────────
    info = c.get("/system/info")
    check(info.get("program") == "example_server", "GET /system/info")

    status = c.get("/module/sensor/status")
    check("temperature" in status, "GET /module/sensor/status")

    add = c.get("/demo/arithmetic/add", a=2, b=3)
    check(add.get("result") == 5, "GET /demo/arithmetic/add?a=2&b=3")

    # ── writes ────────────────────────────────────────────────────────────
    c.put("/module/sensor/config", {"rate_hz": 20})
    cfg = c.get("/module/sensor/config")
    check(cfg.get("sample_rate") == 20, "PUT then GET /module/sensor/config")

    mul = c.post("/demo/arithmetic/mul", {"a": 4, "b": 6})
    check(mul.get("result") == 24, "POST /demo/arithmetic/mul {a,b}")

    # ── error semantics (RFC 7807) ────────────────────────────────────────
    expect_error(lambda: c.get("/does/not/exist"), 404, "GET missing path → 404")
    expect_error(lambda: c.get("/system/shutdown"), 405,
                 "GET POST-only endpoint → 405")
    expect_error(lambda: c.put("/devices/io/led/value", {"value": 999}), 422,
                 "out-of-range value → 422")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
