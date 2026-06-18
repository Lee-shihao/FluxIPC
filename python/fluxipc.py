"""
fluxipc.py – Python client for FluxIPC MCP server

Transport: Streamable HTTP (MCP spec 2025-03-26) over TCP.
Speaks standard HTTP/1.1 POST to /mcp for requests.
Zero dependencies beyond the Python standard library.

Quickstart
----------
    from fluxipc import FluxIPC

    ipc = FluxIPC("localhost", 32100)
    print(ipc.call("/system/info"))
    print(ipc.call("/demo/arithmetic/add", "7", "3"))

    for tool in ipc.list_tools():
        print(tool["name"], "–", tool.get("description", ""))

Context manager
---------------
    with FluxIPC("localhost", 32100) as ipc:
        result = ipc.call("/system/uptime")
"""

from __future__ import annotations

import itertools
import json
import socket
from typing import Any


class FluxIPCError(Exception):
    """Raised when the server returns a JSON-RPC error."""
    def __init__(self, code: int, message: str) -> None:
        super().__init__(f"[{code}] {message}")
        self.code    = code
        self.message = message


class FluxIPC:
    """MCP Streamable-HTTP client for a FluxIPC server.

    Parameters
    ----------
    host:    Hostname or IP address of the server.
    port:    TCP port (default 32100).
    timeout: Socket timeout in seconds (default 10).
    """

    _id_counter = itertools.count(1)

    def __init__(self, host: str = "localhost", port: int = 32100,
                 timeout: float = 10.0) -> None:
        self._host    = host
        self._port    = port
        self._timeout = timeout
        self._initialize()

    # ── connection management ────────────────────────────────────────────── #

    def _connect(self) -> socket.socket:
        """Open a fresh TCP connection for one HTTP request."""
        s = socket.create_connection((self._host, self._port),
                                     timeout=self._timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    def close(self) -> None:
        """No-op for API compatibility; connections are per-request."""

    def __enter__(self) -> "FluxIPC":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    # ── HTTP/1.1 transport ───────────────────────────────────────────────── #

    def _http_post(self, body: bytes) -> bytes:
        """POST body to /mcp, return the response body bytes."""
        req = (
            f"POST /mcp HTTP/1.1\r\n"
            f"Host: {self._host}:{self._port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Accept: application/json, text/event-stream\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode() + body

        with self._connect() as s:
            s.sendall(req)
            resp = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                resp += chunk

        return self._parse_http_response(resp)

    @staticmethod
    def _parse_http_response(raw: bytes) -> bytes:
        """Extract body from a raw HTTP/1.1 response.

        Returns the body bytes, which may be empty for 202 responses.
        """
        # Server closed connection before sending any bytes (notification ACK)
        if not raw:
            return b""

        sep = raw.find(b"\r\n\r\n")
        if sep < 0:
            # Incomplete response – treat as empty body if it looks like 2xx
            if raw.startswith(b"HTTP/1.1 2"):
                return b""
            raise ConnectionError("malformed HTTP response")

        headers = raw[:sep].decode(errors="replace")
        body    = raw[sep + 4:]

        status_line = headers.splitlines()[0]
        if not status_line.startswith("HTTP/1.1 2"):
            raise ConnectionError(f"HTTP error: {status_line}")

        return body

    # ── JSON-RPC layer ───────────────────────────────────────────────────── #

    def _call_rpc(self, method: str, **params: Any) -> Any:
        """Send one JSON-RPC request, return result (raises FluxIPCError on error)."""
        msg = {
            "jsonrpc": "2.0",
            "id":      next(self._id_counter),
            "method":  method,
            "params":  params,
        }
        body = json.dumps(msg, separators=(",", ":")).encode()
        raw  = self._http_post(body)

        # Handle SSE-wrapped response (Content-Type: text/event-stream)
        if raw.startswith(b"data:"):
            # Strip SSE framing: "data: <json>\n\n"
            raw = raw.removeprefix(b"data:").strip()

        resp = json.loads(raw)
        if "error" in resp:
            e = resp["error"]
            raise FluxIPCError(e.get("code", -1), e.get("message", "unknown error"))
        return resp.get("result")

    def _notify(self, method: str, **params: Any) -> None:
        """Send a JSON-RPC notification (no id, 202 response expected)."""
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        body = json.dumps(msg, separators=(",", ":")).encode()
        self._http_post(body)

    # ── MCP handshake ────────────────────────────────────────────────────── #

    def _initialize(self) -> None:
        self._call_rpc(
            "initialize",
            protocolVersion="2025-03-26",
            capabilities={},
            clientInfo={"name": "fluxipc.py", "version": "1.0.0"},
        )
        self._notify("notifications/initialized")

    # ── public API ───────────────────────────────────────────────────────── #

    def list_tools(self) -> list[dict]:
        """Return all tools exposed by this FluxIPC server."""
        result = self._call_rpc("tools/list")
        return result.get("tools", [])

    def call(self, path: str, *args: str) -> str:
        """Invoke a FluxIPC endpoint and return its text output.

        Parameters
        ----------
        path:  IPC path, e.g. "/system/info" or "/demo/arithmetic/add".
               Leading slash optional; slashes become underscores for MCP.
        *args: Positional string arguments forwarded to the handler.

        Returns
        -------
        Handler output as a string.

        Raises
        ------
        FluxIPCError on server-side error.
        """
        name   = path.lstrip("/").replace("/", "_")
        result = self._call_rpc("tools/call",
                                name=name,
                                arguments={"args": list(args)})
        content = result.get("content", [])
        return content[0]["text"] if content else ""

    def ping(self) -> bool:
        """Ping the server. Returns True on success."""
        self._call_rpc("ping")
        return True

    def __repr__(self) -> str:
        return f"FluxIPC({self._host!r}, {self._port})"
