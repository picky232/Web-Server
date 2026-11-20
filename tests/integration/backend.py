#!/usr/bin/env python3
"""Test/demo backend for the reverse proxy (Python standard library only).

This is NOT part of the C server. It plays the role of an application server
that the C web server forwards /api/* requests to.

Run standalone:
    python3 tests/integration/backend.py --port 9000 --name A

Endpoints (all under /api):
    GET  /api/hello            small JSON
    GET  /api/time             current server time (JSON)
    ANY  /api/echo             echoes method, path, headers, body length/sha256/body
    GET  /api/slow?ms=N        sleeps N ms before answering (timeout tests)
    GET  /api/big?n=N          N deterministic bytes with Content-Length
    GET  /api/chunked          Transfer-Encoding: chunked response
    GET  /api/eof              no Content-Length, body ends when connection closes
    GET  /api/hangup           closes the socket without any response
    GET  /api/partial          promises Content-Length 1000, sends 10 bytes, closes
    GET  /api/status/NNN       responds with status NNN
"""
import argparse
import hashlib
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


def big_body(n):
    pattern = bytes(range(256))
    reps = n // 256 + 1
    return (pattern * reps)[:n]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "TestBackend/1.0"
    backend_name = "A"

    def log_message(self, fmt, *args):  # keep test output quiet
        pass

    # ---- helpers -------------------------------------------------------
    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length > 0 else b""

    def _send(self, status, body, ctype="application/json", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Backend", self.backend_name)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, status, obj, extra=None):
        self._send(status, json.dumps(obj, ensure_ascii=False).encode(), extra=extra)

    # ---- routing -------------------------------------------------------
    def _dispatch(self):
        url = urlsplit(self.path)
        q = parse_qs(url.query)
        path = url.path
        body = self._read_body()

        if path == "/api/hello":
            self._json(200, {"backend": self.backend_name, "message": "hello from backend"})
        elif path == "/api/time":
            self._json(200, {"backend": self.backend_name,
                             "time": time.strftime("%Y-%m-%d %H:%M:%S")})
        elif path == "/api/echo":
            self._json(200, {
                "backend": self.backend_name,
                "method": self.command,
                "path": self.path,
                "headers": {k: v for k, v in self.headers.items()},
                "body_len": len(body),
                "body_sha256": hashlib.sha256(body).hexdigest(),
                "body": body.decode("utf-8", "replace") if len(body) <= 4096 else None,
            })
        elif path == "/api/slow":
            time.sleep(int(q.get("ms", ["3000"])[0]) / 1000.0)
            self._json(200, {"backend": self.backend_name, "slow": True})
        elif path == "/api/big":
            n = int(q.get("n", ["1000000"])[0])
            self._send(200, big_body(n), ctype="application/octet-stream")
        elif path == "/api/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"hello ", b"chunked ", b"world"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.write(b"0\r\n\r\n")
            self.close_connection = True
        elif path == "/api/eof":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"body delimited by connection close")
            self.close_connection = True
        elif path == "/api/hangup":
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        elif path == "/api/partial":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
                             b"Content-Type: text/plain\r\n\r\n0123456789")
            self.wfile.flush()
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        elif path.startswith("/api/status/"):
            code = int(path.rsplit("/", 1)[1])
            if code in (204, 304):
                self.send_response(code)
                self.send_header("X-Backend", self.backend_name)
                self.end_headers()
            else:
                self._json(code, {"backend": self.backend_name, "status": code})
        else:
            self._json(404, {"backend": self.backend_name, "error": "no such api"})

    def _safe_dispatch(self):
        try:
            self._dispatch()
        except (BrokenPipeError, ConnectionResetError):
            # the proxy gave up on us (e.g. 504 timeout test): expected, stay quiet
            self.close_connection = True

    do_GET = _safe_dispatch
    do_POST = _safe_dispatch
    do_HEAD = _safe_dispatch


# socketserver's default listen backlog is 5: bursts of proxy connections would
# overflow it and cost a 1 s SYN retransmit. Raise it for load tests.
ThreadingHTTPServer.request_queue_size = 128


def start_backend(port=0, name="A"):
    """Start a backend in a daemon thread. Returns (server, port)."""
    handler = type("NamedHandler", (Handler,), {"backend_name": name})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.daemon_threads = True
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server, server.server_address[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--name", default="A")
    args = ap.parse_args()
    handler = type("NamedHandler", (Handler,), {"backend_name": args.name})
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    server.daemon_threads = True
    print(f"backend {args.name} listening on http://127.0.0.1:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
