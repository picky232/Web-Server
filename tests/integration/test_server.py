#!/usr/bin/env python3
"""Integration tests for the C web server.

The tests start the real server binary (WEBSERVER_BIN, default build/webserver)
on 127.0.0.1 with --port 0, read the chosen port from its log, and talk to it
over raw sockets so that byte-stream behaviour (split headers, pipelining,
partial bodies, early close) can be controlled exactly. A Python backend
fixture (backend.py) plays the reverse-proxy upstream.

Server logs are written to build/test-logs/<SANITIZER or default>/<name>.log.
Set SANITIZER=asan|tsan to also fail on sanitizer reports in those logs.

Run:  make integration      (or)   WEBSERVER_BIN=build/webserver python3 tests/integration/test_server.py
"""
import concurrent.futures
import hashlib
import http.client
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
from backend import big_body, start_backend  # noqa: E402

BIN = os.environ.get("WEBSERVER_BIN", str(ROOT / "build" / "webserver"))
SANITIZER = os.environ.get("SANITIZER", "")
LOG_DIR = ROOT / "build" / "test-logs" / (SANITIZER or "default")
SLOW = 3 if SANITIZER else 1  # sanitizer builds are slower: stretch timing limits

SANITIZER_PATTERNS = [
    rb"ERROR: AddressSanitizer",
    rb"ERROR: LeakSanitizer",
    rb"runtime error:",
    rb"WARNING: ThreadSanitizer",
]


# ---------------------------------------------------------------------------
# server process management
# ---------------------------------------------------------------------------
class Server:
    """Runs build/webserver as a child process and captures its stderr log."""

    def __init__(self, name, args=(), root=None):
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        self.name = name
        self.log_path = LOG_DIR / f"{name}.log"
        self._log = open(self.log_path, "wb")
        self.port = None
        cmd = [BIN, "--bind", "127.0.0.1", "--port", "0",
               "--root", str(root or ROOT / "www"), *map(str, args)]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        ready = threading.Event()

        def pump():
            for line in iter(self.proc.stderr.readline, b""):
                self._log.write(line)
                self._log.flush()
                if self.port is None:
                    m = re.search(rb"listening on http://[\d.]+:(\d+)", line)
                    if m:
                        self.port = int(m.group(1))
                        ready.set()
            ready.set()

        self._pump = threading.Thread(target=pump, daemon=True)
        self._pump.start()
        if not ready.wait(10 * SLOW) or self.port is None:
            self.proc.kill()
            raise RuntimeError(f"server {name} did not start; see {self.log_path}")

    @property
    def pid(self):
        return self.proc.pid

    def alive(self):
        return self.proc.poll() is None

    def fd_count(self):
        return len(os.listdir(f"/proc/{self.pid}/fd"))

    def thread_count(self):
        return len(os.listdir(f"/proc/{self.pid}/task"))

    def stop(self, sig=signal.SIGINT, timeout=10):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            rc = self.proc.wait(timeout * SLOW)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            rc = "timeout"
        self._pump.join(5)
        self._log.close()
        self.proc.stderr.close()
        return rc

    def log_bytes(self):
        return self.log_path.read_bytes()

    def sanitizer_findings(self):
        data = self.log_bytes()
        return [p.decode() for p in SANITIZER_PATTERNS if re.search(p, data)]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_until(pred, timeout, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


# ---------------------------------------------------------------------------
# minimal raw HTTP client (so we control every byte on the wire)
# ---------------------------------------------------------------------------
class Resp:
    def __init__(self, status, reason, headers, body, raw_head):
        self.status = status
        self.reason = reason
        self.headers = headers
        self.body = body
        self.raw_head = raw_head

    def h(self, name):
        return self.headers.get(name.lower())


class Conn:
    """A client connection with a receive buffer; can read several responses."""

    def __init__(self, port, timeout=5 * SLOW, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect(("127.0.0.1", port))
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        d = self.sock.recv(65536)
        if not d:
            raise EOFError("connection closed by server")
        self.buf += d

    def read_response(self, method="GET"):
        while True:
            while b"\r\n\r\n" not in self.buf:
                self._fill()
            head, self.buf = self.buf.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            parts = lines[0].split(" ", 2)
            status = int(parts[1])
            reason = parts[2] if len(parts) > 2 else ""
            headers = {}
            for line in lines[1:]:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
            if 100 <= status < 200:
                continue  # interim response (100 Continue)
            break
        body = b""
        if method == "HEAD" or status in (204, 304):
            pass
        elif "content-length" in headers:
            n = int(headers["content-length"])
            while len(self.buf) < n:
                self._fill()
            body, self.buf = self.buf[:n], self.buf[n:]
        else:  # body delimited by connection close
            try:
                while True:
                    self._fill()
            except EOFError:
                pass
            body, self.buf = self.buf, b""
        return Resp(status, reason, headers, body, lines[0])

    def read_until_eof(self):
        data = self.buf
        self.buf = b""
        while True:
            d = self.sock.recv(65536)
            if not d:
                return data
            data += d

    def is_closed_by_peer(self, wait=1.0):
        """True if the server closed the connection (EOF) within `wait` seconds."""
        self.sock.settimeout(wait)
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False
        except ConnectionResetError:
            return True

    def close(self):
        self.sock.close()


def build(method, path, headers=None, body=b"", version="HTTP/1.1", host=True):
    lines = [f"{method} {path} {version}"]
    hdrs = dict(headers or {})
    if host and version == "HTTP/1.1" and not any(k.lower() == "host" for k in hdrs):
        lines.append("Host: test.local")
    if body and not any(k.lower() == "content-length" for k in hdrs):
        hdrs["Content-Length"] = str(len(body))
    for k, v in hdrs.items():
        lines.append(f"{k}: {v}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def request(port, raw, method="GET", timeout=5 * SLOW):
    c = Conn(port, timeout=timeout)
    try:
        c.send(raw)
        return c.read_response(method)
    finally:
        c.close()


def get(port, path, headers=None):
    return request(port, build("GET", path, headers))


def sha(b):
    return hashlib.sha256(b).hexdigest()


class ServerTestCase(unittest.TestCase):
    """Base class: subclasses create self.srv in setUpClass; checked on teardown."""

    servers = []

    @classmethod
    def start(cls, name, args=(), root=None):
        s = Server(name, args, root)
        cls.servers.append(s)
        return s

    @classmethod
    def tearDownClass(cls):
        problems = []
        for s in cls.servers:
            rc = s.stop()
            if rc != 0:
                problems.append(f"{s.name}: exit code {rc} (log {s.log_path})")
            found = s.sanitizer_findings()
            if found:
                problems.append(f"{s.name}: sanitizer report {found} (log {s.log_path})")
        cls.servers = []
        if problems:
            raise AssertionError("; ".join(problems))


# ---------------------------------------------------------------------------
# 1. static files, HTTP parsing, errors, keep-alive
# ---------------------------------------------------------------------------
class StaticFileTests(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.servers = []
        cls.tmp = tempfile.TemporaryDirectory()
        base = Path(cls.tmp.name)
        root = base / "root"
        outside = base / "outside"
        root.mkdir()
        outside.mkdir()
        (outside / "secret.txt").write_text("TOP SECRET outside docroot\n")
        cls.files = {
            "index.html": b"<!doctype html><title>t</title><h1>index</h1>\n",
            "style.css": b"body { color: red; }\n",
            "app.js": b"console.log('hi');\n",
            "logo.png": (ROOT / "www" / "images" / "logo.png").read_bytes(),
            "photo.jpg": b"\xff\xd8\xff\xe0" + os.urandom(5000) + b"\xff\xd9",
            "binary.bin": bytes(range(256)) * 1000,
            "big.bin": os.urandom(4 * 1024 * 1024),
            "hello world.txt": b"spaces are fine\n",
            "sub/index.html": b"<h1>sub index</h1>\n",
            "sub/page.html": b"<h1>sub page</h1>\n",
            ".hidden.txt": b"hidden\n",
            "noperm.txt": b"no permission\n",
        }
        for rel, data in cls.files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        (root / "emptydir").mkdir()
        os.chmod(root / "noperm.txt", 0)
        os.symlink(outside / "secret.txt", root / "link_out.txt")
        os.symlink(outside, root / "linkdir_out")
        os.symlink(root / "style.css", root / "link_in.css")
        cls.root = root
        cls.srv = cls.start("static", ["--threads", "4", "--read-timeout-ms", 2000,
                                       "--keepalive-ms", 1500, "--max-body", 65536], root)
        cls.port = cls.srv.port

    @classmethod
    def tearDownClass(cls):
        try:
            super().tearDownClass()
        finally:
            cls.tmp.cleanup()

    # ---- basic GET / MIME / binary ----
    def test_get_root_serves_index(self):
        r = get(self.port, "/")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.h("content-type"), "text/html; charset=utf-8")
        self.assertEqual(int(r.h("content-length")), len(r.body))
        self.assertEqual(r.body, self.files["index.html"])
        self.assertTrue(r.raw_head.startswith("HTTP/1.1 200 OK"))
        self.assertIsNotNone(r.h("date"))

    def test_mime_types_and_bodies(self):
        cases = {
            "/index.html": "text/html; charset=utf-8",
            "/style.css": "text/css; charset=utf-8",
            "/app.js": "text/javascript; charset=utf-8",
            "/logo.png": "image/png",
            "/photo.jpg": "image/jpeg",
            "/binary.bin": "application/octet-stream",
        }
        for path, ctype in cases.items():
            with self.subTest(path=path):
                r = get(self.port, path)
                self.assertEqual(r.status, 200)
                self.assertEqual(r.h("content-type"), ctype)
                self.assertEqual(r.body, self.files[path[1:]])
                self.assertEqual(int(r.h("content-length")), len(self.files[path[1:]]))

    def test_large_binary_integrity(self):
        r = get(self.port, "/big.bin")
        self.assertEqual(r.status, 200)
        self.assertEqual(sha(r.body), sha(self.files["big.bin"]))

    def test_percent_encoded_name_and_query(self):
        self.assertEqual(get(self.port, "/hello%20world.txt").body, self.files["hello world.txt"])
        self.assertEqual(get(self.port, "/index.html?v=1&x=../../etc").status, 200)

    def test_head_has_length_but_no_body(self):
        c = Conn(self.port)
        c.send(build("HEAD", "/style.css"))
        r = c.read_response("HEAD")
        self.assertEqual(r.status, 200)
        self.assertEqual(int(r.h("content-length")), len(self.files["style.css"]))
        # the next request on the same connection must parse cleanly (no stray body bytes)
        c.send(build("GET", "/style.css"))
        r2 = c.read_response()
        self.assertEqual(r2.body, self.files["style.css"])
        c.close()

    # ---- status codes ----
    def test_404_403_and_directories(self):
        self.assertEqual(get(self.port, "/missing.html").status, 404)
        self.assertEqual(get(self.port, "/.hidden.txt").status, 403)
        if os.geteuid() != 0:
            self.assertEqual(get(self.port, "/noperm.txt").status, 403)
        r = get(self.port, "/sub")
        self.assertEqual(r.status, 301)
        self.assertEqual(r.h("location"), "/sub/")
        r = get(self.port, "/sub?x=1")
        self.assertEqual(r.h("location"), "/sub/?x=1")
        self.assertEqual(get(self.port, "/sub/").body, self.files["sub/index.html"])
        self.assertEqual(get(self.port, "/sub/page.html").body, self.files["sub/page.html"])
        self.assertEqual(get(self.port, "/emptydir/").status, 404)

    def test_method_handling(self):
        r = request(self.port, build("POST", "/index.html", body=b"x=1"))
        self.assertEqual(r.status, 405)
        self.assertEqual(r.h("allow"), "GET, HEAD")
        self.assertEqual(request(self.port, build("PUT", "/index.html")).status, 405)
        self.assertEqual(request(self.port, build("DELETE", "/index.html")).status, 405)
        self.assertEqual(request(self.port, build("BREW", "/")).status, 501)

    def test_malformed_requests(self):
        cases = [
            (b"garbage\r\n\r\n", 400),
            (b"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505),
            (b"GET / HTTP/1.1x\r\nHost: a\r\n\r\n", 400),
            (b"get / HTTP/1.1\r\nHost: a\r\n\r\n", 501),
            (b"GET / HTTP/1.1\r\n\r\n", 400),                                   # no Host
            (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: a\r\nBad Header: x\r\n\r\n", 400),
            (b"GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", 400),
            (b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", 400),
            (b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -5\r\n\r\n", 400),
            (b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", 400),
            (b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
            (b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n", 400),
            (b"GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", 400),
            (b"GET /a\x01b HTTP/1.1\r\nHost: a\r\n\r\n", 400),
        ]
        for raw, expected in cases:
            with self.subTest(raw=raw[:40]):
                c = Conn(self.port)
                c.send(raw)
                r = c.read_response()
                self.assertEqual(r.status, expected)
                self.assertEqual(r.h("connection"), "close")
                self.assertEqual(int(r.h("content-length")), len(r.body))
                self.assertTrue(c.is_closed_by_peer(2 * SLOW), "server must close after error")
                c.close()

    def test_size_limits(self):
        # URI longer than 2048 → 414
        self.assertEqual(get(self.port, "/" + "a" * 3000).status, 414)
        # request line longer than the 8 KiB head buffer → 414
        c = Conn(self.port)
        c.send(b"GET /" + b"a" * 9000)
        self.assertEqual(c.read_response().status, 414)
        c.close()
        # many headers → 431
        hdrs = {f"X-H{i}": "v" for i in range(70)}
        self.assertEqual(get(self.port, "/", hdrs).status, 431)
        # header block bigger than 8 KiB → 431
        self.assertEqual(get(self.port, "/", {"X-Big": "b" * 9000}).status, 431)
        # body bigger than --max-body (64 KiB): rejected before the body is sent
        c = Conn(self.port)
        c.send(b"POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 100000\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 413)
        c.close()

    # ---- keep-alive / pipelining / HTTP/1.0 ----
    def test_keep_alive_and_connection_close(self):
        c = Conn(self.port)
        for _ in range(3):
            c.send(build("GET", "/style.css"))
            r = c.read_response()
            self.assertEqual(r.status, 200)
            self.assertEqual(r.h("connection"), "keep-alive")
        c.send(build("GET", "/style.css", {"Connection": "close"}))
        r = c.read_response()
        self.assertEqual(r.h("connection"), "close")
        self.assertTrue(c.is_closed_by_peer())
        c.close()

    def test_pipelined_requests_in_one_packet(self):
        c = Conn(self.port)
        c.send(build("GET", "/style.css") + build("GET", "/app.js") +
               build("GET", "/missing") + build("GET", "/index.html"))
        self.assertEqual(c.read_response().body, self.files["style.css"])
        self.assertEqual(c.read_response().body, self.files["app.js"])
        self.assertEqual(c.read_response().status, 404)
        self.assertEqual(c.read_response().body, self.files["index.html"])
        c.close()

    def test_http10_defaults_to_close(self):
        c = Conn(self.port)
        c.send(b"GET /style.css HTTP/1.0\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.h("connection"), "close")
        self.assertTrue(c.is_closed_by_peer())
        c.close()
        c = Conn(self.port)
        c.send(b"GET /style.css HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        self.assertEqual(c.read_response().h("connection"), "keep-alive")
        c.close()

    def test_keepalive_idle_timeout_closes(self):
        c = Conn(self.port)
        c.send(build("GET", "/style.css"))
        c.read_response()
        t0 = time.time()
        self.assertTrue(c.is_closed_by_peer(5 * SLOW))  # --keepalive-ms 1500
        self.assertLess(time.time() - t0, 4 * SLOW)
        c.close()

    # ---- path safety ----
    def test_traversal_is_blocked(self):
        for path in ["/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd",
                     "/..%2f..%2fetc%2fpasswd", "/sub/../../etc/passwd",
                     "/%2E%2E/", "/.%2e/outside/secret.txt", "/sub/%2e%2e/%2e%2e/x"]:
            with self.subTest(path=path):
                r = get(self.port, path)
                self.assertEqual(r.status, 403)
                self.assertNotIn(b"root:", r.body)
        self.assertEqual(get(self.port, "/%00").status, 400)
        self.assertEqual(get(self.port, "/%zz").status, 400)
        # symlinks: inside → ok, pointing outside → 403
        self.assertEqual(get(self.port, "/link_in.css").body, self.files["style.css"])
        r = get(self.port, "/link_out.txt")
        self.assertEqual(r.status, 403)
        self.assertNotIn(b"SECRET", r.body)
        self.assertEqual(get(self.port, "/linkdir_out/secret.txt").status, 403)

    # ---- byte-stream behaviour ----
    def test_header_split_into_three_sends(self):
        raw = build("GET", "/style.css")
        c = Conn(self.port)
        for part in (raw[:7], raw[7:25], raw[25:]):
            c.send(part)
            time.sleep(0.2)
        self.assertEqual(c.read_response().body, self.files["style.css"])
        c.close()

    def test_split_inside_terminator_and_byte_by_byte(self):
        raw = build("GET", "/app.js")
        c = Conn(self.port)
        c.send(raw[:-1])  # "...\r\n\r"
        time.sleep(0.2)
        c.send(raw[-1:])  # "\n"
        self.assertEqual(c.read_response().body, self.files["app.js"])
        c.close()
        c = Conn(self.port)
        c.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for b in raw:
            c.send(bytes([b]))
            time.sleep(0.002)
        self.assertEqual(c.read_response().body, self.files["app.js"])
        c.close()

    def test_slow_reader_gets_complete_file(self):
        """Server-side partial send(): tiny receive window + slow reads."""
        c = Conn(self.port, rcvbuf=4096)
        c.send(build("GET", "/big.bin"))
        data = b""
        while b"\r\n\r\n" not in data:
            data += c.sock.recv(4096)
        head, body = data.split(b"\r\n\r\n", 1)
        n = int(re.search(rb"Content-Length: (\d+)", head).group(1))
        chunks = 0
        while len(body) < n:
            d = c.sock.recv(4096)
            self.assertTrue(d)
            body += d
            chunks += 1
            if chunks % 64 == 0:
                time.sleep(0.005)
        c.close()
        self.assertEqual(sha(body), sha(self.files["big.bin"]))

    def test_partial_request_times_out_with_408(self):
        c = Conn(self.port, timeout=10 * SLOW)
        c.send(b"GET / HTTP/1.1\r\nHo")
        t0 = time.time()
        r = c.read_response()
        elapsed = time.time() - t0
        self.assertEqual(r.status, 408)
        self.assertGreater(elapsed, 1.5)            # --read-timeout-ms 2000
        self.assertLess(elapsed, 5 * SLOW)
        c.close()

    def test_silent_connection_is_closed_without_response(self):
        c = Conn(self.port, timeout=10 * SLOW)
        self.assertEqual(c.read_until_eof(), b"")
        c.close()

    def test_client_disconnects_mid_request(self):
        for raw in [b"GET /index.html HTTP/1.1\r\nHost: a\r\n",
                    b"POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 1000\r\n\r\nonly-some"]:
            c = Conn(self.port)
            c.send(raw)
            time.sleep(0.05)
            c.close()
        time.sleep(0.2)
        self.assertTrue(self.srv.alive())
        self.assertEqual(get(self.port, "/style.css").status, 200)

    def test_client_reset_during_download_no_sigpipe(self):
        for _ in range(20):
            c = Conn(self.port, rcvbuf=4096)
            c.send(build("GET", "/big.bin"))
            c.sock.recv(1024)
            # SO_LINGER 0 → close() sends RST: server's next send() gets EPIPE/ECONNRESET
            c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            c.close()
        time.sleep(0.5)
        self.assertTrue(self.srv.alive(), "server died (SIGPIPE?)")
        self.assertEqual(get(self.port, "/style.css").status, 200)

    # ---- resource accounting ----
    def test_no_fd_leak_and_fixed_threads(self):
        threads_before = self.srv.thread_count()
        get(self.port, "/style.css")
        self.assertTrue(wait_until(lambda: self.srv.fd_count() <= 4 + 3, 5 * SLOW))
        baseline = self.srv.fd_count()
        for i in range(100):
            get(self.port, ["/style.css", "/missing", "/../x", "/big.bin"][i % 4])
            request(self.port, b"bogus\r\n\r\n")
        for _ in range(10):
            c = Conn(self.port)
            c.send(b"GET / HTTP/1.1\r\n")
            c.close()
        self.assertTrue(wait_until(lambda: self.srv.fd_count() == baseline, 6 * SLOW),
                        f"fd count {self.srv.fd_count()} != baseline {baseline}")
        self.assertEqual(self.srv.thread_count(), threads_before)


# ---------------------------------------------------------------------------
# 2. reverse proxy
# ---------------------------------------------------------------------------
class ProxyTests(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.servers = []
        cls.backend, cls.bport = start_backend(name="A")
        cls.backend_b, cls.bport_b = start_backend(name="B")
        cls.srv = cls.start("proxy", ["--backend", f"127.0.0.1:{cls.bport}",
                                      "--proxy-timeout-ms", 1000 * SLOW,
                                      "--max-body", 4 * 1024 * 1024])
        cls.port = cls.srv.port

    @classmethod
    def tearDownClass(cls):
        try:
            super().tearDownClass()
        finally:
            cls.backend.shutdown()
            cls.backend_b.shutdown()

    def echo(self, r):
        self.assertEqual(r.status, 200, r.body[:200])
        return json.loads(r.body)

    def test_get_hello(self):
        r = get(self.port, "/api/hello")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.h("x-backend"), "A")
        self.assertEqual(json.loads(r.body)["message"], "hello from backend")
        self.assertEqual(r.h("connection"), "keep-alive")

    def test_post_echo_body_and_forwarded_headers(self):
        body = os.urandom(100_000)
        r = request(self.port, build("POST", "/api/echo?x=1", {"Content-Type": "application/octet-stream"}, body))
        e = self.echo(r)
        self.assertEqual(e["method"], "POST")
        self.assertEqual(e["path"], "/api/echo?x=1")
        self.assertEqual(e["body_len"], len(body))
        self.assertEqual(e["body_sha256"], sha(body))
        self.assertEqual(e["headers"]["Host"], f"127.0.0.1:{self.bport}")
        self.assertEqual(e["headers"]["X-Forwarded-Host"], "test.local")
        self.assertEqual(e["headers"]["X-Forwarded-For"], "127.0.0.1")
        self.assertEqual(e["headers"]["Connection"], "close")

    def test_post_body_arrives_in_parts(self):
        body = b"part-one|part-two|part-three"
        raw = build("POST", "/api/echo", body=body)
        head_len = raw.index(b"\r\n\r\n") + 4
        c = Conn(self.port)
        c.send(raw[:head_len - 2])
        time.sleep(0.15)
        c.send(raw[head_len - 2:head_len + 5])
        time.sleep(0.15)
        c.send(raw[head_len + 5:head_len + 15])
        time.sleep(0.15)
        c.send(raw[head_len + 15:])
        e = self.echo(c.read_response())
        c.close()
        self.assertEqual(e["body"], body.decode())

    def test_expect_100_continue(self):
        body = b"x" * 5000
        c = Conn(self.port)
        c.send(b"POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5000\r\n"
               b"Expect: 100-continue\r\n\r\n")
        interim = b""
        while b"\r\n\r\n" not in interim:
            interim += c.sock.recv(1024)
        self.assertTrue(interim.startswith(b"HTTP/1.1 100 Continue"))
        c.send(body)
        e = self.echo(c.read_response())
        c.close()
        self.assertEqual(e["body_len"], 5000)
        self.assertNotIn("Expect", e["headers"])

    def test_hop_by_hop_and_spoofed_headers_removed(self):
        r = get(self.port, "/api/echo", {
            "Connection": "keep-alive, X-Secret",
            "X-Secret": "should-not-pass",
            "Keep-Alive": "timeout=5",
            "Proxy-Connection": "keep-alive",
            "TE": "trailers",
            "Upgrade": "websocket",
            "X-Forwarded-For": "6.6.6.6",
            "X-Normal": "yes",
        })
        hdrs = self.echo(r)["headers"]
        for gone in ["X-Secret", "Keep-Alive", "Proxy-Connection", "TE", "Upgrade"]:
            self.assertNotIn(gone, hdrs)
        self.assertEqual(hdrs["X-Normal"], "yes")
        self.assertEqual(hdrs["X-Forwarded-For"], "127.0.0.1")

    def test_keep_alive_through_proxy(self):
        c = Conn(self.port)
        for path in ["/api/hello", "/api/echo", "/style.css", "/api/hello"]:
            c.send(build("GET", path))
            r = c.read_response()
            self.assertEqual(r.status, 200)
            self.assertEqual(r.h("connection"), "keep-alive")
        c.close()

    def test_head_and_status_passthrough(self):
        c = Conn(self.port)
        c.send(build("HEAD", "/api/hello"))
        r = c.read_response("HEAD")
        self.assertEqual(r.status, 200)
        self.assertGreater(int(r.h("content-length")), 0)
        c.send(build("GET", "/api/status/204"))
        self.assertEqual(c.read_response().status, 204)
        c.send(build("GET", "/api/status/404"))
        r = c.read_response()
        self.assertEqual(r.status, 404)
        self.assertEqual(json.loads(r.body)["status"], 404)
        c.close()

    def test_large_response_streamed(self):
        n = 3_000_000
        r = get(self.port, f"/api/big?n={n}")
        self.assertEqual(r.status, 200)
        self.assertEqual(sha(r.body), sha(big_body(n)))

    def test_chunked_response_relayed_and_closed(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5 * SLOW)
        conn.request("GET", "/api/chunked")
        r = conn.getresponse()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.getheader("Transfer-Encoding"), "chunked")
        self.assertEqual(r.getheader("Connection"), "close")
        self.assertEqual(r.read(), b"hello chunked world")
        conn.close()

    def test_eof_delimited_response(self):
        c = Conn(self.port)
        c.send(build("GET", "/api/eof"))
        r = c.read_response()
        self.assertEqual(r.body, b"body delimited by connection close")
        self.assertEqual(r.h("connection"), "close")
        c.close()

    def test_backend_timeout_504(self):
        t0 = time.time()
        r = get(self.port, "/api/slow?ms=4000")
        elapsed = time.time() - t0
        self.assertEqual(r.status, 504)
        self.assertLess(elapsed, 3 * SLOW)

    def test_backend_hangup_502(self):
        self.assertEqual(get(self.port, "/api/hangup").status, 502)

    def test_backend_truncated_body_closes_client(self):
        c = Conn(self.port)
        c.send(build("GET", "/api/partial"))
        data = c.read_until_eof()
        c.close()
        head, body = data.split(b"\r\n\r\n", 1)
        self.assertIn(b"Content-Length: 1000", head)
        self.assertEqual(body, b"0123456789")  # truncated, then connection closed

    def test_prefix_escape_and_open_proxy_attempts(self):
        self.assertEqual(get(self.port, "/api/../index.html").status, 403)
        self.assertEqual(get(self.port, "/api/%2e%2e/index.html").status, 403)
        self.assertEqual(get(self.port, "/apix").status, 404)          # not proxied
        self.assertIsNone(get(self.port, "/apix").h("x-backend"))
        r = request(self.port, b"GET http://example.com/api/hello HTTP/1.1\r\nHost: example.com\r\n\r\n")
        self.assertEqual(r.status, 400)
        self.assertEqual(request(self.port, build("PUT", "/api/echo")).status, 405)
        r = request(self.port, b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n")
        self.assertEqual(r.status, 400)

    def test_backend_down_502_and_round_robin(self):
        dead = free_port()
        s = self.start("proxy_dead", ["--backend", f"127.0.0.1:{dead}", "--proxy-timeout-ms", 1000])
        t0 = time.time()
        self.assertEqual(get(s.port, "/api/hello").status, 502)
        self.assertLess(time.time() - t0, 1.5 * SLOW)

        rr = self.start("proxy_rr", ["--backend", f"127.0.0.1:{self.bport}",
                                     "--backend", f"127.0.0.1:{self.bport_b}"])
        seen = [get(rr.port, "/api/hello").h("x-backend") for _ in range(6)]
        self.assertEqual(seen.count("A"), 3)
        self.assertEqual(seen.count("B"), 3)
        self.assertNotEqual(seen[0], seen[1])

    def test_no_fd_leak_after_proxy_errors(self):
        get(self.port, "/api/hello")
        self.assertTrue(wait_until(lambda: self.srv.fd_count() <= 7, 6 * SLOW))
        baseline = self.srv.fd_count()
        for _ in range(20):
            get(self.port, "/api/hello")
            get(self.port, "/api/hangup")
            request(self.port, build("POST", "/api/echo", body=b"abc"))
        get(self.port, "/api/slow?ms=3000")  # 504 path
        self.assertTrue(wait_until(lambda: self.srv.fd_count() == baseline, 8 * SLOW),
                        f"fd count {self.srv.fd_count()} != baseline {baseline}")


# ---------------------------------------------------------------------------
# 3. concurrency, queue saturation, shutdown
# ---------------------------------------------------------------------------
class ConcurrencyTests(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.servers = []
        cls.backend, cls.bport = start_backend(name="A")
        cls.srv = cls.start("concurrency", ["--threads", "4", "--queue", "128",
                                            "--backend", f"127.0.0.1:{cls.bport}",
                                            "--read-timeout-ms", 3000 * SLOW])
        cls.port = cls.srv.port
        cls.css = (ROOT / "www" / "style.css").read_bytes()

    @classmethod
    def tearDownClass(cls):
        try:
            super().tearDownClass()
        finally:
            cls.backend.shutdown()

    def test_many_concurrent_clients(self):
        def worker(i):
            out = []
            for j in range(10):
                path = "/style.css" if (i + j) % 2 else "/api/hello"
                r = get(self.port, path)
                out.append((r.status, path, r.body))
            return out

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
            results = [x for f in [ex.submit(worker, i) for i in range(64)] for x in f.result()]
        self.assertEqual(len(results), 640)
        for status, path, body in results:
            self.assertEqual(status, 200)
            if path == "/style.css":
                self.assertEqual(body, self.css)

    def test_slow_clients_do_not_block_others(self):
        slow = []
        for _ in range(3):  # 3 slowloris clients on 4 workers
            c = Conn(self.port, timeout=10 * SLOW)
            c.send(b"GET / HTTP/1.1\r\nHost: a\r\nX-Slow: ")
            slow.append(c)
        time.sleep(0.2)
        t0 = time.time()
        for _ in range(20):
            self.assertEqual(get(self.port, "/style.css").status, 200)
        elapsed = time.time() - t0
        self.assertLess(elapsed, 2.0 * SLOW, f"20 requests took {elapsed:.2f}s")
        for c in slow:
            c.close()

    def test_idle_keepalive_yields_worker_when_queue_waits(self):
        s = self.start("one_worker", ["--threads", "1", "--keepalive-ms", 10000])
        idle = Conn(s.port, timeout=15)
        idle.send(build("GET", "/style.css"))
        idle.read_response()   # worker now waits for this keep-alive client's next request
        t0 = time.time()
        r = get(s.port, "/style.css")
        self.assertEqual(r.status, 200)
        self.assertLess(time.time() - t0, 1.5 * SLOW)  # not 10 s
        self.assertTrue(idle.is_closed_by_peer(1))
        idle.close()

    def test_queue_full_returns_503(self):
        s = self.start("tiny_queue", ["--threads", "1", "--queue", "1",
                                      "--read-timeout-ms", 4000 * SLOW])
        busy = Conn(s.port, timeout=15)
        busy.send(b"GET / HTTP/1.1\r\nHost: a\r\n")   # occupies the only worker
        time.sleep(0.3)
        queued = Conn(s.port, timeout=15)               # waits in the 1-slot queue
        time.sleep(0.3)
        rejected = Conn(s.port, timeout=5)
        r = rejected.read_response()
        self.assertEqual(r.status, 503)
        self.assertEqual(r.h("retry-after"), "1")
        rejected.close()
        busy.close()
        queued.close()
        self.assertTrue(wait_until(lambda: get(s.port, "/style.css").status == 200, 5 * SLOW))

    def test_graceful_shutdown_finishes_in_flight_request(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(sig=sig):
                s = Server(f"shutdown_{sig.name}", ["--backend", f"127.0.0.1:{self.bport}",
                                                    "--proxy-timeout-ms", 5000])
                idle = Conn(s.port)
                idle.send(build("GET", "/style.css"))
                idle.read_response()                    # idle keep-alive connection
                inflight = Conn(s.port, timeout=10 * SLOW)
                inflight.send(build("GET", "/api/slow?ms=1000"))
                time.sleep(0.3)
                t0 = time.time()
                s.proc.send_signal(sig)
                r = inflight.read_response()            # still answered
                self.assertEqual(r.status, 200)
                # keep-alive was decided when the request started (before the signal);
                # the guarantee is that the server closes the connection right after.
                self.assertTrue(inflight.is_closed_by_peer(2 * SLOW))
                rc = s.stop(sig, timeout=10)
                self.assertEqual(rc, 0)
                self.assertLess(time.time() - t0, 5 * SLOW)
                self.assertTrue(idle.is_closed_by_peer(1))
                self.assertIn(b"shutdown complete", s.log_bytes())
                self.assertEqual(s.sanitizer_findings(), [])
                idle.close()
                inflight.close()
                with self.assertRaises(OSError):
                    socket.create_connection(("127.0.0.1", s.port), timeout=1).close()

    def test_restart_on_same_port_uses_reuseaddr(self):
        port = free_port()
        s1 = Server("reuse1", ["--port", port])  # later --port overrides the default 0
        get(s1.port, "/style.css")
        request(s1.port, build("GET", "/style.css", {"Connection": "close"}))  # server-side close → TIME_WAIT
        self.assertEqual(s1.stop(), 0)
        s2 = Server("reuse2", ["--port", port])
        self.assertEqual(s2.port, port)
        self.assertEqual(get(s2.port, "/style.css").status, 200)
        self.assertEqual(s2.stop(), 0)


if __name__ == "__main__":
    if not Path(BIN).exists():
        sys.exit(f"server binary not found: {BIN} (run make first)")
    print(f"binary={BIN} sanitizer={SANITIZER or 'none'} logs={LOG_DIR}", flush=True)
    unittest.main(verbosity=2)
