"""
fluxipc.py – Python client for the FluxIPC HTTP server.

Speaks plain HTTP/1.1 with JSON bodies against the REST interface exposed by
any FluxIPC server:

    GET    /<container>     -> manifest of leaves below it
    GET    /<leaf>          -> invoke (read)
    POST   /<leaf>          -> invoke (write / action)
    PUT/PATCH/DELETE ...
    GET    /openapi.json    -> OpenAPI 3.1 document

Only the standard library is required.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request


class FluxIPCError(Exception):
    """Raised for a non-2xx HTTP response (RFC 7807 problem+json)."""

    def __init__(self, status: int, detail, url: str):
        self.status = status
        self.detail = detail
        self.url = url
        super().__init__(f"HTTP {status} for {url}: {detail}")


class FluxIPC:
    """Minimal REST client for a FluxIPC server."""

    def __init__(self, host: str = "localhost", port: int = 32100,
                 timeout: float = 5.0):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout

    # ── low level ──────────────────────────────────────────────────────────

    def request(self, method: str, path: str, body=None,
                query: dict | None = None) -> dict | None:
        url = self.base + path
        if query:
            url += "?" + urllib.parse.urlencode(query)

        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = urllib.request.Request(url, data=data, headers=headers,
                                     method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read()
                if not raw:
                    return None
                return json.loads(raw.decode("utf-8"))
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                detail = json.loads(raw.decode("utf-8"))
            except Exception:
                detail = raw.decode("utf-8", "replace")
            raise FluxIPCError(e.code, detail, url) from None
        except urllib.error.URLError as e:
            raise FluxIPCError(0, str(e.reason), url) from None

    # ── convenience ────────────────────────────────────────────────────────

    def get(self, path: str, **query) -> dict | None:
        return self.request("GET", path, query=query or None)

    def post(self, path: str, body=None) -> dict | None:
        return self.request("POST", path, body=body if body is not None else {})

    def put(self, path: str, body=None) -> dict | None:
        return self.request("PUT", path, body=body if body is not None else {})

    def patch(self, path: str, body=None) -> dict | None:
        return self.request("PATCH", path, body=body if body is not None else {})

    def delete(self, path: str) -> dict | None:
        return self.request("DELETE", path)

    def options(self, path: str) -> dict | None:
        return self.request("OPTIONS", path)

    # ── discovery ──────────────────────────────────────────────────────────

    def list(self, path: str = "/", recursive: bool = True) -> dict:
        """Return the leaf manifest for a container (all nodes support this)."""
        q = None if recursive else {"recursive": "0"}
        return self.request("GET", path, query=q) or {}

    def openapi(self) -> dict:
        """Return the generated OpenAPI 3.1 document."""
        return self.request("GET", "/openapi.json") or {}

    def healthz(self) -> dict:
        return self.request("GET", "/healthz") or {}


if __name__ == "__main__":  # tiny CLI: python3 fluxipc.py [host] [port]
    import sys

    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 32100
    client = FluxIPC(host, port)
    print(json.dumps(client.list("/"), indent=2))
