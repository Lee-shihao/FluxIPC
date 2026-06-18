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
        self._host         = host
        self._port         = port
        self._timeout      = timeout
        self._session_id   = None   # set after initialize
        self._proto_ver    = "2025-03-26"  # will be negotiated
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
        headers = (
            f"POST /mcp HTTP/1.1\r\n"
            f"Host: {self._host}:{self._port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Accept: application/json, text/event-stream\r\n"
            f"Content-Length: {len(body)}\r\n"
        )
        if self._session_id:
            headers += f"Mcp-Session-Id: {self._session_id}\r\n"
        if self._proto_ver:
            headers += f"Mcp-Protocol-Version: {self._proto_ver}\r\n"
        headers += "Connection: close\r\n\r\n"

        with self._connect() as s:
            s.sendall(headers.encode() + body)
            resp = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                resp += chunk

        return self._parse_http_response(resp)

    def _parse_http_response(self, raw: bytes) -> bytes:
        """Extract body from a raw HTTP/1.1 response.

        Returns the body bytes, which may be empty for 202 responses.
        Also extracts Mcp-Session-Id from response headers for session
        management.
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

        headers_text = raw[:sep].decode(errors="replace")
        body         = raw[sep + 4:]

        status_line = headers_text.splitlines()[0]
        if not status_line.startswith("HTTP/1.1 2"):
            raise ConnectionError(f"HTTP error: {status_line}")

        # Extract Mcp-Session-Id from response headers (case-insensitive)
        for line in headers_text.splitlines():
            if line.lower().startswith("mcp-session-id:"):
                sid = line.split(":", 1)[1].strip()
                if sid:
                    self._session_id = sid
                    break

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
        result = self._call_rpc(
            "initialize",
            protocolVersion=self._proto_ver,
            capabilities={},
            clientInfo={"name": "fluxipc.py", "version": "1.0.0"},
        )
        # Accept the server's protocol version
        if result and "protocolVersion" in result:
            self._proto_ver = result["protocolVersion"]
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
