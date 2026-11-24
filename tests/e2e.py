#!/usr/bin/env python3
"""기능 테스트: 실제 서버를 띄우고 소켓으로 요청을 보내 확인한다 (표준 라이브러리만).

    make e2e
    WEBSERVER=build/asan/webserver python3 tests/e2e.py [-k 이름]
"""
import hashlib
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

ROOT = Path(__file__).resolve().parent.parent
BIN = str(ROOT / os.environ.get("WEBSERVER", "build/webserver"))
H = b"Host: x\r\n"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """설정 파일을 만들어 서버를 띄운다"""

    def __init__(self, conf, **popen):
        self.dir = tempfile.mkdtemp(prefix="webserver-e2e-")
        self.port, self.admin = free_port(), free_port()
        path = os.path.join(self.dir, "server.conf")
        with open(path, "w") as f:
            f.write(f"listen 127.0.0.1:{self.port}\nadmin 127.0.0.1:{self.admin}\nloops 3\naccess_log {self.dir}/access.log\n" +
                    conf.replace("@PORT@", str(self.port)))
        self.err = open(os.path.join(self.dir, "stderr"), "w+")
        self.p = subprocess.Popen([BIN, "-c", path], stderr=self.err, **popen)
        end = time.time() + 10
        while time.time() < end:
            try:
                socket.create_connection(("127.0.0.1", self.port), 1).close()
                return
            except OSError:
                if self.p.poll() is not None:
                    break
                time.sleep(0.02)
        raise RuntimeError("서버가 뜨지 않음: " + self.stderr())

    def stderr(self):
        self.err.flush()
        return Path(self.err.name).read_text()

    def stop(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            self.p.send_signal(sig)
        code = self.p.wait(20)
        text = self.stderr()
        self.err.close()
        shutil.rmtree(self.dir, ignore_errors=True)
        if code != 0 or re.search(r"Sanitizer|runtime error", text):
            raise AssertionError(f"종료 코드 {code}\n{text[-3000:]}")

    # --- 요청 보내기 ---
    def raw(self, *parts, timeout=3, eof=False, delay=0.03, port=None):
        """바이트를 그대로 보내고 받은 것 전부와 끝난 모양('EOF', 'OPEN', 'RST')을 돌려준다"""
        s = socket.create_connection(("127.0.0.1", port or self.port))
        s.settimeout(timeout)
        out, end = b"", "OPEN"
        try:
            for i, x in enumerate(parts):
                if i:
                    time.sleep(delay)
                s.sendall(x)
            if eof:
                s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        try:
            while True:
                d = s.recv(1 << 16)
                if not d:
                    end = "EOF"
                    break
                out += d
        except socket.timeout:
            pass
        except ConnectionError:
            end = "RST"
        s.close()
        return out, end

    def get(self, path, method="GET", headers=(), body=b"", host="x", version="1.1", timeout=5):
        """요청 하나. Connection: close로 끝까지 받아 (상태, 헤더 dict, 본문)"""
        h = "".join(f"{k}: {v}\r\n" for k, v in headers)
        if body:
            h += f"Content-Length: {len(body)}\r\n"
        req = f"{method} {path} HTTP/{version}\r\nHost: {host}\r\nConnection: close\r\n{h}\r\n".encode() + body
        out, end = self.raw(req, timeout=timeout)
        return parse(out, method == "HEAD") + (end,)


def parse(raw, head=False):
    i = raw.index(b"\r\n\r\n")
    lines = raw[:i].decode("latin1").split("\r\n")
    headers = {}
    for x in lines[1:]:
        k, v = x.split(":", 1)
        headers.setdefault(k.lower(), v.strip())
    body = raw[i + 4:]
    if headers.get("transfer-encoding") == "chunked" and not head:
        body = unchunk(body)
    return int(lines[0].split()[1]), headers, body


def unchunk(b):
    out = b""
    while True:
        i = b.index(b"\r\n")
        n = int(b[:i].split(b";")[0], 16)
        out, b = out + b[i + 2:i + 2 + n], b[i + 2 + n + 2:]
        if n == 0:
            return out


def statuses(raw):
    return [int(x) for x in re.findall(rb"HTTP/1\.1 (\d{3})", raw)]


class Case(unittest.TestCase):
    conf = ""

    @classmethod
    def setUpClass(cls):
        cls.www = tempfile.mkdtemp(prefix="webserver-www-")
        cls.files(cls.www)
        cls.s = Server(cls.make_conf())

    @classmethod
    def files(cls, d):
        pass

    @classmethod
    def make_conf(cls):
        return f"route * / static {cls.www}\n"

    @classmethod
    def tearDownClass(cls):
        try:
            cls.s.stop()
        finally:
            shutil.rmtree(cls.www, ignore_errors=True)


class Parsing(Case):
    @classmethod
    def files(cls, d):
        Path(d, "index.html").write_text("hello")

    def test_bad_requests(self):
        for req, want in [
            (b"GET / HTTP/1.1\r\n\r\n", 400), (b"GET / HTTP/1.1\r\n" + H + H + b"\r\n", 400),
            (b"GET /\r\n" + H + b"\r\n", 400), (b"G@T / HTTP/1.1\r\n" + H + b"\r\n", 400),
            (b"BREW / HTTP/1.1\r\n" + H + b"\r\n", 501), (b"GET / HTTP/2.0\r\n" + H + b"\r\n", 505),
            (b"GET / HTTP/1.1\n" + H + b"\n", 400), (b"\x16\x03\x01\x02", 400),
            (b"GET /../x HTTP/1.1\r\n" + H + b"\r\n", 400), (b"GET /%2e%2e/x HTTP/1.1\r\n" + H + b"\r\n", 400),
            (b"GET /%00 HTTP/1.1\r\n" + H + b"\r\n", 400), (b"GET /" + b"a" * 20000, 414),
            (b"GET / HTTP/1.1\r\n" + H + b"X: " + b"a" * 20000, 431),
            (b"CONNECT a:443 HTTP/1.1\r\n" + H + b"\r\n", 501),
            (b"POST / HTTP/1.1\r\n" + H + b"Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
        ]:
            out, end = self.s.raw(req)
            self.assertEqual((statuses(out), end), ([want], "EOF"), req[:60])
            self.assertIn(b"Connection: close", out)

    def test_split_anywhere(self):
        req = b"GET / HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n"
        for i in range(1, len(req), 7):
            out, _ = self.s.raw(req[:i], req[i:], delay=0.01)
            self.assertEqual(statuses(out), [200])
        out, _ = self.s.raw(*[req[i:i + 1] for i in range(len(req))], delay=0.002)
        self.assertEqual(statuses(out), [200])

    def test_pipeline_order(self):
        one = b"GET / HTTP/1.1\r\n" + H + b"\r\n"
        out, end = self.s.raw(one * 60 + b"GET /none HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        self.assertEqual((statuses(out), end), ([200] * 60 + [404], "EOF"))

    def test_keep_alive_rules(self):
        self.assertEqual(self.s.raw(b"GET / HTTP/1.1\r\n" + H + b"\r\n", timeout=0.4)[1], "OPEN")
        self.assertEqual(self.s.raw(b"GET / HTTP/1.0\r\n\r\n", timeout=1)[1], "EOF")
        self.assertEqual(self.s.raw(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", timeout=0.4)[1], "OPEN")

    def test_absolute_form_and_options(self):
        self.assertEqual(statuses(self.s.raw(b"GET http://any:1/ HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")[0]), [200])
        self.assertEqual(statuses(self.s.raw(b"OPTIONS * HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")[0]), [204])

    def test_expect_never_after_final(self):
        # 예전 구현의 결함: 최종 응답 뒤에 100을 또 보냈다
        out, _ = self.s.raw(b"POST /.env HTTP/1.1\r\n" + H + b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n")
        self.assertEqual(statuses(out), [405])

    def test_half_close_still_answers(self):
        out, end = self.s.raw(b"GET / HTTP/1.1\r\n" + H + b"\r\n", eof=True)
        self.assertEqual((statuses(out), end), ([200], "EOF"))


class Static(Case):
    @classmethod
    def files(cls, d):
        p = Path(d)
        (p / "sub").mkdir()
        (p / "empty").mkdir()
        (p / "index.html").write_text("<h1>root</h1>")
        (p / "sub/index.html").write_text("sub")
        for n in ("a.css", "a.js", "a.png", "a.jpg", "한글 이름.txt", ".env"):
            (p / n).write_text(n)
        (p / "big.bin").write_bytes(os.urandom(5 << 20))
        os.symlink("/etc/hosts", p / "link")
        os.symlink("/etc", p / "etc")
        os.mkfifo(p / "fifo")
        (p / "secret").write_text("x")
        os.chmod(p / "secret", 0)

    def test_mime_and_body(self):
        for path, mime in [("/", "text/html"), ("/a.css", "text/css"), ("/a.js", "text/javascript"),
                           ("/a.png", "image/png"), ("/a.jpg", "image/jpeg"), ("/sub/", "text/html")]:
            st, h, body, _ = self.s.get(path)
            self.assertEqual(st, 200, path)
            self.assertTrue(h["content-type"].startswith(mime), path)
            self.assertEqual(int(h["content-length"]), len(body))
        st, _, body, _ = self.s.get("/%ED%95%9C%EA%B8%80%20%EC%9D%B4%EB%A6%84.txt?q=1")
        self.assertEqual((st, body.decode()), (200, "한글 이름.txt"))

    def test_big_file_is_exact(self):
        want = hashlib.sha256(Path(self.www, "big.bin").read_bytes()).hexdigest()
        st, _, body, _ = self.s.get("/big.bin")
        self.assertEqual((st, hashlib.sha256(body).hexdigest()), (200, want))

    def test_head(self):
        st, h, body, _ = self.s.get("/a.css", method="HEAD")
        self.assertEqual((st, h["content-length"], body), (200, "5", b""))

    def test_errors(self):
        for path, want in [("/none", 404), ("/sub/none/x", 404), ("/.env", 403), ("/link", 403), ("/etc/hosts", (403, 404)),
                           ("/fifo", 403), ("/empty/", 403), ("/secret", 403), ("/" + "n" * 300, 404)]:
            st = self.s.get(path)[0]
            self.assertIn(st, want if isinstance(want, tuple) else (want,), path)

    def test_directory_redirect_stays_here(self):
        # 예전 구현의 결함: //sub → Location: //sub/ (다른 호스트로 튐)
        for path in ("/sub", "//sub", "/sub?a=1"):
            st, h, _, _ = self.s.get(path)
            self.assertEqual((st, h["location"].split("?")[0]), (301, "/sub/"), path)

    def test_methods(self):
        st, h, _, _ = self.s.get("/a.css", method="POST", body=b"x")
        self.assertEqual((st, h["allow"]), (405, "GET, HEAD"))

    def test_conditional(self):
        _, h, _, _ = self.s.get("/a.css")
        for head in (("If-None-Match", h["etag"]), ("If-None-Match", '"zz", W/' + h["etag"]), ("If-None-Match", "*"),
                     ("If-Modified-Since", h["last-modified"])):
            st, h2, body, _ = self.s.get("/a.css", headers=[head])
            self.assertEqual((st, body, h2["etag"]), (304, b"", h["etag"]))
            self.assertNotIn("content-length", h2)
        self.assertEqual(self.s.get("/a.css", headers=[("If-None-Match", '"old"')])[0], 200)

    def slow_fetch(self, change):
        f = Path(self.www, "change.bin")
        f.write_bytes(b"A" * 8_000_000)
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.connect(("127.0.0.1", self.s.port))
        s.settimeout(5)
        s.sendall(b"GET /change.bin HTTP/1.1\r\n" + H + b"\r\n")
        out = s.recv(1024)
        time.sleep(0.3)
        change(str(f))
        end = "OPEN"
        try:
            while True:
                d = s.recv(1 << 20)
                if not d:
                    end = "EOF"
                    break
                out += d
        except socket.timeout:
            pass
        except ConnectionError:
            end = "RST"
        s.close()
        return len(out) - out.index(b"\r\n\r\n") - 4, end

    def test_file_shrinks_while_sending(self):
        # 예전 구현의 결함: 약속보다 적게 보내고 연결을 안 닫아 클라이언트가 무한 대기
        n, end = self.slow_fetch(lambda f: os.truncate(f, 1_000_000))
        self.assertLess(n, 8_000_000)
        self.assertIn(end, ("EOF", "RST"))

    def test_file_grows_while_sending(self):
        # 예전 구현의 결함: 늘어난 만큼 더 보내 다음 응답과 섞임
        def grow(f):
            with open(f, "ab") as g:
                g.write(b"HTTP/1.1 200 INJECTED\r\n" * 50000)
        self.assertEqual(self.slow_fetch(grow)[0], 8_000_000)

    def test_many_clients_get_their_own_file(self):
        for i in range(20):
            Path(self.www, f"f{i}").write_bytes(bytes([i]) * (200_000 + i))
        bad = []

        def one(i):
            for _ in range(5):
                st, _, body, _ = self.s.get(f"/f{i}", timeout=20)
                if st != 200 or body != bytes([i]) * (200_000 + i):
                    bad.append(i)
        ts = [threading.Thread(target=one, args=(i % 20,)) for i in range(40)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(bad, [])

# ---- 리버스 프록시 ----

class Backend:
    """바이트 단위로 조종하는 가짜 백엔드. script(sock, head, body)가 응답을 직접 쓴다"""

    def __init__(self, script, port=0, backlog=128):
        self.script, self.requests, self.connections = script, [], 0
        self.open = True
        self.ls = socket.socket()
        self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", port))
        self.ls.listen(backlog)
        self.port = self.ls.getsockname()[1]
        self.socks = []
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                s, _ = self.ls.accept()
            except OSError:
                return
            self.socks.append(s)
            threading.Thread(target=self.serve, args=(s,), daemon=True).start()

    def serve(self, s):
        buf = b""
        counted = False
        try:
            while True:
                while b"\r\n\r\n" not in buf:
                    d = s.recv(65536)
                    if not d:
                        return
                    buf += d
                if not counted:
                    counted = True
                    self.connections += 1
                i = buf.index(b"\r\n\r\n") + 4
                head, buf = buf[:i], buf[i:]
                if head.startswith(b"GET /health "):
                    s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
                    continue
                m = re.search(rb"(?i)content-length: (\d+)", head)
                n = int(m.group(1)) if m else 0
                if not getattr(self.script, "early", False):
                    while len(buf) < n:
                        d = s.recv(1 << 20)
                        if not d:
                            return
                        buf += d
                body, buf = buf[:n], buf[n:]
                self.requests.append((head, body))
                if self.script(s, head, body) is False:
                    return
        except OSError:
            pass
        finally:
            s.close()

    def stop(self):
        self.ls.close()
        for s in self.socks:
            try:
                s.close()
            except OSError:
                pass


def ok(body=b"ok", extra=b""):
    data = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n%s\r\n" % (len(body), extra) + body
    return lambda s, head, b: s.sendall(data)


def send(*parts, close=False, delay=0):
    def script(s, head, body):
        for x in parts:
            s.sendall(x)
            time.sleep(delay)
        return not close
    return script


class ProxyCase(unittest.TestCase):
    """테스트마다 백엔드와 서버를 새로 띄운다"""
    extra = ""

    def up(self, *scripts, pool="", extra="", route="route * / proxy p\n"):
        self.b = [x if isinstance(x, Backend) else Backend(x) for x in scripts]
        addrs = " ".join(f"127.0.0.1:{b.port}" for b in self.b)
        self.s = Server(f"pool p {pool} {addrs}\n{route}health_interval 200ms\n{self.extra}{extra}")
        return self.s

    def tearDown(self):
        for b in getattr(self, "b", []):
            b.stop()
        if hasattr(self, "s"):
            self.s.stop()


class ProxyRelay(ProxyCase):
    def test_request_is_rewritten(self):
        s = self.up(ok())
        s.raw(b"POST //a/./b?q=%20 HTTP/1.1\r\nHost: Ex.com:99\r\nConnection: X-Drop, keep-alive\r\nX-Drop: 1\r\nKeep-Alive: 5\r\n"
              b"X-Forwarded-For: 6.6.6.6\r\nTE: trailers\r\nUpgrade: h2c\r\nX-Keep: yes\r\nTransfer-Encoding: chunked\r\n\r\n"
              b"3\r\nabc\r\n0\r\n\r\n", timeout=0.5)
        head, body = self.b[0].requests[0]
        h = head.decode()
        self.assertTrue(h.startswith("POST /a/b?q=%20 HTTP/1.1\r\nHost: Ex.com:99\r\n"), h)
        self.assertEqual(body, b"abc")
        for gone in ("X-Drop", "Keep-Alive", "6.6.6.6", "TE:", "Upgrade", "chunked"):
            self.assertNotIn(gone, h)
        for there in ("X-Keep: yes", "X-Forwarded-For: 127.0.0.1", "Via: 1.1 jiho-webserver-", "Content-Length: 3"):
            self.assertIn(there, h)

    def test_response_framings(self):
        cases = {
            b"/len": b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
            b"/chunk": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n3;x=1\r\nllo\r\n0\r\nT: 1\r\n\r\n",
            # 예전 구현의 결함: 둘 다 오면 둘 다 넘겼다
            b"/both": b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
            b"/eof": b"HTTP/1.1 200 OK\r\n\r\nhello",
        }

        def script(sock, head, body):
            path = head.split()[1]
            sock.sendall(cases[path])
            return path != b"/eof"
        s = self.up(script)
        for path in cases:
            st, h, body, end = s.get(path.decode())
            self.assertEqual((st, body, end), (200, b"hello", "EOF"), path)
            self.assertEqual("content-length" in h, path == b"/len", path)
            # HTTP/1.0 클라이언트에게는 chunked를 보내면 안 된다
            out, _ = s.raw(b"GET " + path + b" HTTP/1.0\r\n\r\n")
            self.assertNotIn(b"chunked", out)
            self.assertTrue(out.endswith(b"\r\n\r\nhello"), out)

    def test_no_body_statuses_keep_the_connection_clean(self):
        def script(sock, head, body):
            p = head.split()[1]
            sock.sendall({b"/204": b"HTTP/1.1 204 No Content\r\n\r\n", b"/304": b"HTTP/1.1 304 Not Modified\r\nETag: \"a\"\r\n\r\n",
                          b"/head": b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"}.get(p) or b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        s = self.up(script)
        out, end = s.raw(b"GET /204 HTTP/1.1\r\n" + H + b"\r\n" + b"GET /304 HTTP/1.1\r\n" + H + b"\r\n" +
                         b"HEAD /head HTTP/1.1\r\n" + H + b"\r\n" + b"GET /z HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        self.assertEqual((statuses(out), end), ([204, 304, 200, 200], "EOF"))
        self.assertIn(b"Content-Length: 1000", out)
        self.assertEqual(self.b[0].connections, 2)  # HEAD 뒤에만 새 연결, 나머지는 다시 썼다

    def test_interim_responses(self):
        # 예전 구현의 결함: 103을 최종 응답으로 보고 진짜 200을 버렸다
        s = self.up(send(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n",
                         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", delay=0.05))
        out, _ = s.raw(b"GET / HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        self.assertEqual(statuses(out), [103, 200])
        self.assertTrue(out.endswith(b"ok"))
        self.assertEqual(statuses(s.raw(b"GET / HTTP/1.0\r\n\r\n")[0]), [200])

    def test_bad_responses_become_502(self):
        bad = [b"garbage\r\n\r\n", b"HTTP/1.1 200 OK\nContent-Length: 0\n\n", b"HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n",
               b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", b"HTTP/1.1 101 Switching\r\n\r\n",
               b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
               b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
               b"HTTP/1.1 200 OK\r\nX: " + b"a" * 30000 + b"\r\n\r\n", b"HTTP/1.1 200"]
        for data in bad:
            b = Backend(send(data, close=True))
            s = Server(f"pool p 127.0.0.1:{b.port}\nroute * / proxy p\n")
            try:
                st, _, _, end = s.get("/")
                self.assertEqual((st, end), (502, "EOF"), data[:50])
            finally:
                s.stop()
                b.stop()

    def test_truncated_bodies_cut_the_client(self):
        for data in (b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort",
                     b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nshort\r\n",
                     b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"):
            b = Backend(send(data, close=True))
            s = Server(f"pool p 127.0.0.1:{b.port}\nroute * / proxy p\n")
            try:
                out, end = s.raw(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
                self.assertIn(end, ("EOF", "RST"))
                self.assertNotIn(b"\r\n0\r\n\r\n", out)  # 정상으로 끝난 척하면 안 된다
                out, end = s.raw(b"GET / HTTP/1.0\r\n\r\n")
                if b"chunked" in data:  # 1.0에는 닫힘이 끝이라는 뜻이므로 RST로 알려야 한다
                    self.assertEqual(end, "RST")
            finally:
                s.stop()
                b.stop()

    def test_early_error_while_uploading(self):
        def script(sock, head, body):
            sock.sendall(b"HTTP/1.1 413 Content Too Large\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbig")
            return False
        script.early = True
        s = self.up(script)
        st, _, body, _ = s.get("/", method="POST", body=b"x" * 5_000_000)
        self.assertEqual((st, body), (413, b"big"))

    def test_reset_while_reading_until_close_is_not_a_complete_answer(self):
        def script(sock, head, body):
            sock.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial")
            time.sleep(0.2)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\1\0\0\0\0\0\0\0")
            return False
        s = self.up(script)
        out, end = s.raw(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
        self.assertNotIn(b"\r\n0\r\n\r\n", out)
        self.assertNotEqual(end, "OPEN")
        self.assertEqual(s.raw(b"GET / HTTP/1.0\r\n\r\n")[1], "RST")

    def test_late_body_of_head_does_not_reach_anyone(self):
        def script(sock, head, body):
            if head.startswith(b"HEAD"):
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n")
                time.sleep(0.02)
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nFAKE")
            else:
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nreal")
        s = self.up(script, extra="loops 1\n")
        out, _ = s.raw(b"HEAD /h HTTP/1.1\r\n" + H + b"\r\n" + (b"GET /a HTTP/1.1\r\n" + H + b"\r\n") * 3 + b"GET /z HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        self.assertEqual((out.count(b"real"), out.count(b"FAKE")), (4, 0))

    def test_leftover_bytes_do_not_reach_the_next_client(self):
        n = [0]

        def script(sock, head, body):
            n[0] += 1
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nEVIL")
        s = self.up(script)
        for _ in range(5):
            self.assertEqual(s.get("/")[2], b"ok")

    def test_big_relay_with_slow_client_uses_little_memory(self):
        size = 200 << 20

        def script(sock, head, body):
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % size)
            block = b"z" * (1 << 20)
            for _ in range(size >> 20):
                sock.sendall(block)
        s = self.up(script)
        c = socket.create_connection(("127.0.0.1", s.port))
        c.sendall(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
        c.recv(1000)
        time.sleep(1)  # 읽지 않고 버틴다
        rss = int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(s.p.pid)]))
        got = 0
        c.settimeout(10)
        while got < size - 2000:
            got += len(c.recv(1 << 20))
        c.close()
        if "asan" not in BIN and "tsan" not in BIN:
            self.assertLess(rss, 30_000, "KB")

    def test_concurrent_bodies_do_not_mix(self):
        s = self.up(lambda sock, head, body: sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body),
                    lambda sock, head, body: sock.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n" % len(body) + body + b"\r\n0\r\n\r\n"))
        bad = []

        def one(i):
            body = bytes([i]) * (1000 * i + 1)
            for _ in range(10):
                st, _, got, _ = s.get("/", method="POST", body=body, timeout=20)
                if st != 200 or got != body:
                    bad.append((i, st, len(got)))
        ts = [threading.Thread(target=one, args=(i,)) for i in range(1, 41)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(bad, [])

    def test_loop_is_detected(self):
        s = Server("pool me 127.0.0.1:@PORT@\nroute * / proxy me\n")
        try:
            self.assertEqual(s.get("/")[0], 508)
        finally:
            s.stop()


class ProxyFailure(ProxyCase):
    extra = "timeout_response 600ms\ntimeout_stall 600ms\n"

    def test_balances(self):
        for policy in ("least_conn", "round_robin"):
            s = self.up(ok(b"A"), ok(b"B"), pool=policy)
            got = [s.get("/")[2] for _ in range(10)]
            self.assertEqual((got.count(b"A"), got.count(b"B")), (5, 5), policy)
            self.tearDown()
            del self.s

    def test_least_conn_avoids_the_busy_one(self):
        s = self.up(lambda sock, h, b: (time.sleep(0.5), sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nslow"))[1], ok(b"fast"))
        ts = [threading.Thread(target=s.get, args=("/",)) for _ in range(2)]
        [t.start() for t in ts]
        time.sleep(0.1)
        got = [s.get("/")[2] for _ in range(10)]
        [t.join() for t in ts]
        self.assertGreaterEqual(got.count(b"fast"), 9)

    def test_dead_backend_is_invisible_to_clients(self):
        # 예전 구현의 결함: 2대 중 1대가 죽으면 절반이 502
        s = self.up(ok(b"A"), ok(b"B"))
        port = self.b[1].port
        self.b[1].stop()
        self.assertEqual([s.get("/")[0] for _ in range(20)], [200] * 20)
        self.assertEqual([s.get("/", method="POST", body=b"x")[0] for _ in range(20)], [200] * 20)
        self.b[1] = Backend(ok(b"B"), port=port)  # 되살아나면 저절로 돌아온다
        end = time.time() + 5
        while time.time() < end and s.get("/")[2] != b"B":
            time.sleep(0.1)
        self.assertLess(time.time(), end)

    def test_all_dead_is_502_not_hang(self):
        s = self.up(ok(), ok())
        for b in self.b:
            b.stop()
        t = time.time()
        self.assertEqual(s.get("/")[0], 502)
        self.assertLess(time.time() - t, 2)

    def test_hang_is_504_and_does_not_exclude(self):
        def script(sock, head, body):
            if head.startswith(b"GET /hang"):
                time.sleep(3)
                return False
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        s = self.up(script)
        self.assertEqual([s.get("/hang")[0] for _ in range(6)], [504] * 6)
        self.assertEqual(s.get("/")[0], 200)

    def test_header_drip_from_backend_is_504(self):
        def script(sock, head, body):
            for ch in b"HTTP/1.1 200 OK\r\nX: " + b"a" * 100:
                sock.sendall(bytes([ch]))
                time.sleep(0.1)
        s = self.up(script)
        t = time.time()
        self.assertEqual(s.get("/")[0], 504)
        self.assertLess(time.time() - t, 2)

    def test_posts_survive_a_backend_that_closes_silently(self):
        def script(sock, head, body):
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
            return False
        s = self.up(script, extra="loops 1\n")
        bad = []

        def one():
            for _ in range(30):
                if s.get("/", method="POST", body=b"x")[0] != 200:
                    bad.append(1)
        ts = [threading.Thread(target=one) for _ in range(8)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(bad, [])

    def test_stall_in_body_cuts(self):
        s = self.up(send(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc", b"", delay=2))
        t = time.time()
        out, end = s.raw(b"GET / HTTP/1.1\r\n" + H + b"\r\n", timeout=5)
        self.assertIn(end, ("EOF", "RST"))
        self.assertLess(time.time() - t, 2)

    def test_post_is_never_sent_twice(self):
        s = self.up(lambda *a: False, lambda *a: False)
        self.assertEqual(s.get("/", method="POST", body=b"pay")[0], 502)
        self.assertEqual(sum(len(b.requests) for b in self.b), 1)
        self.assertEqual(s.get("/")[0], 502)  # GET은 다른 백엔드로 한 번 더
        self.assertEqual(sum(len(b.requests) for b in self.b), 3)

    def test_get_moves_to_the_other_backend(self):
        s = self.up(lambda *a: False, ok(b"B"), pool="round_robin")
        self.assertEqual([s.get("/")[2] for _ in range(6)], [b"B"] * 6)

    def test_always_503_backend_gets_excluded(self):
        # 즉시 503만 주는 백엔드는 가장 한가해 보여서 least_conn이 몰아준다
        s = self.up(send(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n"), ok(b"B"))
        got = [s.get("/")[0] for _ in range(40)]
        self.assertLessEqual(got.count(503), 6)
        self.assertEqual(got[-20:], [200] * 20)

    def test_500_does_not_exclude(self):
        s = self.up(send(b"HTTP/1.1 500 x\r\nContent-Length: 0\r\n\r\n"), ok(b"B"), pool="round_robin")
        got = [s.get("/")[0] for _ in range(30)]
        self.assertEqual(got.count(500), 15)

    def test_in_flight_limit(self):
        gate = threading.Event()

        def script(sock, head, body):
            gate.wait(5)
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        s = self.up(script, pool="max=2", extra="timeout_response 10s\n")
        res = []
        ts = [threading.Thread(target=lambda: res.append(s.get("/", timeout=10)[0])) for _ in range(2)]
        [t.start() for t in ts]
        time.sleep(0.3)
        st, h, _, _ = s.get("/")
        self.assertEqual((st, h["retry-after"]), (503, "1"))
        gate.set()
        [t.join() for t in ts]
        self.assertEqual(res, [200, 200])
        self.assertEqual(s.get("/")[0], 200)

    def test_stale_pooled_connection(self):
        # 응답 직후 말없이 닫는 백엔드. 다시 쓰려다 실패해도 손님에게는 안 보여야 한다
        def script(sock, head, body):
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
            return False
        s = self.up(script)
        self.assertEqual([s.get("/")[0] for _ in range(30)], [200] * 30)

    def test_frozen_backend_is_found_by_health_check(self):
        # 프로세스가 멈춰도 handshake는 커널이 끝내므로 연결은 성공한다. 헬스 체크만 알아챌 수 있다
        ports = [free_port(), free_port()]
        ps = [subprocess.Popen([sys.executable, str(ROOT / "tests/backend.py"), str(p)], stdout=subprocess.DEVNULL) for p in ports]
        try:
            time.sleep(1)
            s = self.s = Server(f"pool p health=/health 127.0.0.1:{ports[0]} 127.0.0.1:{ports[1]}\nroute * / proxy p\nhealth_interval 200ms\n{self.extra}")
            self.b = []
            ps[1].send_signal(signal.SIGSTOP)
            time.sleep(4)
            t = time.time()
            self.assertEqual([s.get("/")[0] for _ in range(10)], [200] * 10)
            self.assertLess(time.time() - t, 1)
            ps[1].send_signal(signal.SIGCONT)
        finally:
            for p in ps:
                p.send_signal(signal.SIGCONT)
                p.kill()
                p.wait()

    def test_client_leaving_frees_the_backend(self):
        s = self.up(lambda sock, h, b: time.sleep(1.5) or sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), pool="max=1",
                    extra="timeout_response 10s\n")
        c = socket.create_connection(("127.0.0.1", s.port))
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\1\0\0\0\0\0\0\0")
        c.sendall(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
        time.sleep(0.2)
        self.assertEqual(s.get("/")[0], 503)
        c.close()  # RST
        time.sleep(0.2)
        self.assertEqual(s.get("/")[0], 200)

# ---- 포워드 프록시 ----

class Forward(ProxyCase):
    def start(self, script, conf=""):
        self.b = [Backend(script)]
        self.fport = free_port()
        self.s = Server(f"forward 127.0.0.1:{self.fport}\nforward_connect_ports 443 {self.b[0].port}\nroute * / static {ROOT}/www\n{conf}")
        return self.s

    def via(self, *parts, **kw):
        return self.s.raw(*parts, port=self.fport, **kw)

    def test_get_and_post(self):
        self.start(lambda sock, h, b: sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(b) + b))
        to = f"localhost:{self.b[0].port}".encode()  # 이름 해석까지 거친다
        out, end = self.via(b"POST http://" + to + b"/a%2Fb?q HTTP/1.1\r\nHost: ignored\r\nProxy-Connection: keep-alive\r\nX-Forwarded-For: 1.1.1.1\r\nContent-Length: 3\r\n\r\nabc", timeout=1)
        self.assertEqual((statuses(out), end), ([200], "OPEN"))
        self.assertTrue(out.endswith(b"abc"))
        self.assertIn(b"Via: 1.1 jiho-webserver", out)
        head = self.b[0].requests[0][0]
        self.assertTrue(head.startswith(b"POST /a%2Fb?q HTTP/1.1\r\nHost: " + to), head)  # 남의 경로는 고치지 않는다
        self.assertIn(b"Via: 1.1 jiho-webserver", head)
        self.assertIn(b"X-Forwarded-For: 1.1.1.1", head)
        self.assertNotIn(b"Proxy-Connection", head)

    def test_rules(self):
        s = self.start(ok())
        for req, want in [(b"GET / HTTP/1.1\r\n" + H + b"\r\n", 400),
                          (b"GET http://no-such-host.invalid/ HTTP/1.1\r\n" + H + b"\r\n", 502),
                          (b"GET http://127.0.0.1:%d/ HTTP/1.1\r\n" % free_port() + H + b"\r\n", 502),
                          (b"CONNECT 127.0.0.1:22 HTTP/1.1\r\n" + H + b"\r\n", 403),
                          (b"POST http://127.0.0.1:%d/ HTTP/1.1\r\n" % self.b[0].port + H + b"Expect: 100-continue\r\nContent-Length: 1\r\n\r\n", 417),
                          (b"CONNECT 127.0.0.1 HTTP/1.1\r\n" + H + b"\r\n", 400),
                          (b"GET http://127.0.0.1:%d/ HTTP/1.1\r\n" % self.fport + H + b"\r\n", 400)]:
            self.assertEqual(statuses(self.via(req, timeout=6)[0]), [want], req)
        # 일반 포트는 열린 프록시가 아니다: 절대형 주소가 와도 내 규칙대로만 처리
        out, _ = s.raw(b"GET http://127.0.0.1:%d/ HTTP/1.1\r\n" % self.b[0].port + H + b"Connection: close\r\n\r\n")
        self.assertEqual((statuses(out), self.b[0].requests), ([200], []))
        self.assertEqual(statuses(self.via(b"OPTIONS http://127.0.0.1:%d HTTP/1.1\r\n" % self.b[0].port + H + b"Connection: close\r\n\r\n")[0]), [200])
        self.assertTrue(self.b[0].requests[-1][0].startswith(b"OPTIONS * HTTP/1.1"))
        n = len(self.b[0].requests)
        self.assertEqual(statuses(self.via(b"OPTIONS http://127.0.0.1:%d/ HTTP/1.1\r\n" % self.b[0].port + H + b"Max-Forwards: 0\r\nConnection: close\r\n\r\n")[0]), [204])
        self.assertEqual(len(self.b[0].requests), n)
        # HTTP/1.0 클라이언트와는 연결을 유지하지 않는다
        self.assertEqual(self.via(b"GET http://127.0.0.1:%d/ HTTP/1.0\r\nConnection: keep-alive\r\n\r\n" % self.b[0].port)[1], "EOF")

    def test_connect_tunnel(self):
        ls = socket.socket()
        ls.bind(("127.0.0.1", 0))
        ls.listen()
        port = ls.getsockname()[1]

        def echo():
            s, _ = ls.accept()
            s.sendall(s.recv(3)[::-1])
            while d := s.recv(65536):
                s.sendall(d)
            s.sendall(b"BYE")
            s.close()
        threading.Thread(target=echo, daemon=True).start()
        self.start(ok(), f"forward_connect_ports {port}\n")
        c = socket.create_connection(("127.0.0.1", self.fport))
        c.settimeout(10)
        c.sendall(b"CONNECT 127.0.0.1:%d HTTP/1.1\r\n" % port + H + b"\r\nabc")  # 응답을 기다리지 않고 보낸 바이트도 전달
        got = b""
        while not got.endswith(b"cba"):
            got += c.recv(1000)
        self.assertTrue(got.startswith(b"HTTP/1.1 200 "))
        data = os.urandom(3_000_000)
        threading.Thread(target=c.sendall, args=(data,), daemon=True).start()
        back = b""
        while len(back) < len(data):
            back += c.recv(1 << 20)
        self.assertEqual(hashlib.sha256(back).digest(), hashlib.sha256(data).digest())
        c.shutdown(socket.SHUT_WR)  # 보내기만 닫아도 상대의 마지막 말은 들려야 한다
        last = b""
        while d := c.recv(1000):
            last += d
        self.assertEqual(last, b"BYE")
        c.close()
        ls.close()

    def test_forward_to_our_own_reverse_proxy_is_not_a_loop(self):
        b = Backend(ok(b"deep"))
        self.start(ok(), f"pool q 127.0.0.1:{b.port}\nroute * /q proxy q\n")
        out, _ = self.via(b"GET http://127.0.0.1:%d/q HTTP/1.1\r\n" % self.s.port + H + b"Connection: close\r\n\r\n")
        self.assertTrue(statuses(out) == [200] and out.endswith(b"deep"), out)
        b.stop()

    def test_leaving_while_resolving(self):
        s = self.start(ok())
        for i in range(30):
            c = socket.create_connection(("127.0.0.1", self.fport))
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\1\0\0\0\0\0\0\0")
            c.sendall(b"GET http://h%d.invalid/ HTTP/1.1\r\n" % i + H + b"\r\n")
            c.close()
        time.sleep(0.5)
        self.assertEqual(s.get("/")[0], 200)

# ---- 캐시 ----

class Cache(ProxyCase):
    def start(self, extra=b"Cache-Control: max-age=60\r\n", body=b"v%d", pool=""):
        self.n = 0

        def script(sock, head, b):
            self.n += 1
            data = (body % self.n) if b"%d" in body else body
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n" % len(data) + extra + b"\r\n" + data)
        return self.up(script, route="route * / proxy p cache\n", pool=pool)

    def test_hit_miss_and_age(self):
        s = self.start(b'Cache-Control: max-age=60\r\nAge: 5\r\nETag: "e"\r\n')
        st, h, body, _ = s.get("/a?x")
        self.assertEqual((h["x-cache"], body), ("MISS", b"v1"))
        st, h, body, _ = s.get("/a?x")
        self.assertEqual((h["x-cache"], h["age"], body), ("HIT", "5", b"v1"))
        st, h, body, _ = s.get("/a?x", method="HEAD")
        self.assertEqual((h["x-cache"], h["content-length"], body), ("HIT", "2", b""))
        st, h, body, _ = s.get("/a?x", headers=[("If-None-Match", '"e"')])
        self.assertEqual((st, body), (304, b""))
        self.assertEqual(s.get("/a?y")[1]["x-cache"], "MISS")           # 쿼리가 다르면 다른 것
        self.assertEqual(s.get("/a?x", host="other")[1]["x-cache"], "MISS")  # 호스트가 다르면 다른 것
        for h in (("Cookie", "a=b"), ("Authorization", "x"), ("Cache-Control", "no-cache"), ("Range", "bytes=0-0")):
            self.assertEqual(s.get("/a?x", headers=[h])[1]["x-cache"], "MISS", h)
        self.assertEqual(s.get("/a?x", method="POST", body=b"z")[0], 200)  # 값을 바꾸는 요청이 성공하면 지운다
        self.assertEqual(s.get("/a?x")[1]["x-cache"], "MISS")

    def test_private_responses_are_never_stored(self):
        for extra in (b"", b"Cache-Control: private, max-age=60\r\n", b"Cache-Control: max-age=60\r\nSet-Cookie: s=1\r\n",
                      b"Cache-Control: max-age=60\r\nVary: Cookie\r\n", b"Cache-Control: no-store\r\n"):
            s = self.start(extra)
            self.assertEqual([s.get("/")[2] for _ in range(3)], [b"v1", b"v2", b"v3"], extra)
            self.tearDown()
            del self.s

    def test_expires(self):
        s = self.start(b"Cache-Control: max-age=1\r\n")
        self.assertEqual([s.get("/")[2] for _ in range(3)], [b"v1"] * 3)
        time.sleep(1.1)
        self.assertEqual(s.get("/")[2], b"v2")

    def test_truncated_and_big_are_not_stored(self):
        self.n = 0

        def script(sock, head, b):
            self.n += 1
            if b"/cut" in head:
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nCache-Control: max-age=60\r\n\r\nabc")
                return False
            data = b"x" * 2_000_000
            sock.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nCache-Control: max-age=60\r\n\r\n%x\r\n" % len(data) + data + b"\r\n0\r\n\r\n")
        s = self.up(script, route="route * / proxy p cache\n")
        for path, size in (("/cut", None), ("/big", 2_000_000)):
            self.n = 0
            for _ in range(2):
                out, _ = s.raw(f"GET {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n".encode())
                self.assertIn(b"X-Cache: MISS", out)
                if size:
                    self.assertEqual(len(parse(out)[2]), size)
            self.assertEqual(self.n, 2)

    def test_many_readers_and_writers(self):
        s = self.start(b"Cache-Control: max-age=60\r\n", b"z" * 300_000)
        bad = []

        def one(i):
            for k in range(30):
                if i % 4 == 0 and k % 3 == 0:
                    s.get(f"/{k % 5}", method="POST", body=b"x")
                elif s.get(f"/{k % 5}", timeout=20)[2] != b"z" * 300_000:
                    bad.append((i, k))
        ts = [threading.Thread(target=one, args=(i,)) for i in range(16)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(bad, [])

    def test_get_with_a_body_cannot_poison(self):
        s = self.up(lambda sock, h, b: sock.sendall(b"HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nContent-Length: %d\r\n\r\n" % len(b) + b), route="route * / proxy p cache\n")
        s.get("/p", body=b"evil")
        self.assertEqual(s.get("/p")[2], b"")

    def test_old_answer_is_not_stored_after_a_change(self):
        state = {"v": b"v1"}

        def script(sock, head, body):
            if head.startswith(b"POST"):
                state["v"] = b"v2"
                sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
                return
            v = state["v"]
            if b"X-Slow" in head:
                time.sleep(0.5)
            sock.sendall(b"HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nContent-Length: 2\r\n\r\n" + v)
        s = self.up(script, route="route * / proxy p cache\n")
        t = threading.Thread(target=lambda: s.get("/a", headers=[("X-Slow", "1")]))
        t.start()
        time.sleep(0.2)
        s.get("/a", method="POST", body=b"x")
        t.join()
        self.assertEqual(s.get("/a")[2], b"v2")

    def test_survives_dead_backend(self):
        s = self.start()
        s.get("/a")
        self.b[0].stop()
        self.assertEqual((s.get("/a")[0], s.get("/b")[0]), (200, 502))


# ---- CGI, 상태 페이지, 로그 ----

class Cgi(Case):
    @classmethod
    def files(cls, d):
        def script(name, text, mode=0o755):
            p = Path(d, name)
            p.write_text(text)
            p.chmod(mode)
        sh = "#!/bin/sh\n"
        script("env", sh + "printf 'Content-Type: text/plain\\n\\n'; env | sort; echo args=$#\n")
        script("echo", sh + "printf 'Content-Type: application/octet-stream\\r\\n\\r\\n'; cat\n")
        script("status", sh + "printf 'Status: 418 Teapot\\nX-A: b\\n\\nhi'\n")
        script("redirect", sh + "printf 'Location: http://x/y\\n\\n'\n")
        script("length", sh + "printf 'Content-Length: 5\\n\\nhello'\n")
        script("short", sh + "printf 'Content-Length: 50\\n\\nhello'\n")
        script("nohead", sh + "echo no headers here\n")
        script("empty", sh + "exit 0\n")
        script("badhead", sh + "printf 'Bad Header\\n\\nx'\n")
        script("nointerp", "#!/no/such/interpreter\n")
        script("sleep", sh + "sleep 60 & sleep 60\n")
        script("noexec", sh, 0o644)
        script(".hidden", sh)
        script("fds", "#!/usr/bin/env python3\nimport os\nprint('Content-Type: text/plain\\n')\nfor i in range(3, 300):\n    try:\n        os.fstat(i); print('open', i)\n    except OSError: pass\n")
        script("head_first", "#!/usr/bin/env python3\nimport sys\nsys.stdout.write('Content-Type: text/plain\\n\\n'); sys.stdout.flush()\nprint(len(sys.stdin.buffer.read()))\n")
        script("early_exit", sh + "printf 'Content-Type: text/plain\\n\\nSTART\\n'; (sleep 0.6; echo END) & exit 0\n")
        script("large", "#!/usr/bin/env python3\nprint('Content-Type: text/plain\\n'); print('x' * 120000, end='')\n")
        script("endless", sh + "printf 'Content-Type: text/plain\\n\\n'; while true; do echo tick; sleep 0.2; done\n")
        script("over", sh + "printf 'Content-Length: 2\\n\\n'; yes\n")
        script("slow_ok", sh + "sleep 0.7; printf 'Content-Type: text/plain\\n\\nok'\n")
        os.symlink("env", Path(d, "link"))

    @classmethod
    def make_conf(cls):
        return f"route * /cgi/ cgi {cls.www}\ntimeout_cgi 3s\nmax_cgi 4\n"

    def test_environment(self):
        st, _, body, _ = self.s.get("/cgi/env/a%20b/c?x=1&y=2", headers=[("X-One", "1"), ("Proxy", "http://evil"), ("Authorization", "secret"), ("A_B", "z")])
        env = dict(x.split("=", 1) for x in body.decode().splitlines() if "=" in x)
        self.assertEqual(st, 200)
        for k, v in {"REQUEST_METHOD": "GET", "QUERY_STRING": "x=1&y=2", "SCRIPT_NAME": "/cgi/env", "PATH_INFO": "/a b/c",
                     "REMOTE_ADDR": "127.0.0.1", "SERVER_PORT": str(self.s.port), "GATEWAY_INTERFACE": "CGI/1.1", "HTTP_X_ONE": "1", "args": "0"}.items():
            self.assertEqual(env.get(k), v, k)
        for k in ("HTTP_PROXY", "HTTP_AUTHORIZATION", "HOME", "USER", "HTTP_A_B"):
            self.assertNotIn(k, env)

    def test_post_bodies(self):
        for size in (0, 5, 70_000, 3_000_000):
            data = os.urandom(size)
            self.assertEqual(self.s.get("/cgi/echo", method="POST", body=data, timeout=10)[2], data)
        # 머리를 먼저 내보내고 나서 본문을 읽는 프로그램
        self.assertEqual(self.s.get("/cgi/head_first", method="POST", body=b"z" * 1_000_000, timeout=10)[2], b"1000000\n")

    def test_headers(self):
        st, h, body, _ = self.s.get("/cgi/status")
        self.assertEqual((st, h["x-a"], body), (418, "b", b"hi"))
        self.assertEqual(self.s.get("/cgi/redirect")[0], 302)
        st, h, body, _ = self.s.get("/cgi/length")
        self.assertEqual((h["content-length"], body), ("5", b"hello"))
        out, end = self.s.raw(b"GET /cgi/short HTTP/1.1\r\n" + H + b"\r\n")
        self.assertIn(end, ("EOF", "RST"))

    def test_failures(self):
        for path, want in [("nohead", 500), ("empty", 500), ("badhead", 500), ("nointerp", 500), ("none", 404), ("noexec", 403),
                           (".hidden", 403), ("link", 403), ("", 403)]:
            self.assertEqual(self.s.get("/cgi/" + path)[0], want, path)
        self.assertEqual(self.s.get("/cgi/env", method="PUT", body=b"x")[0], 405)

    def running(self):
        return int(subprocess.run(f"pgrep -g $(pgrep -P {self.s.p.pid} | tr '\\n' ',' | sed 's/,$//') 2>/dev/null | wc -l", shell=True, capture_output=True).stdout)

    def test_timeout_kills_the_whole_family(self):
        t = time.time()
        self.assertEqual(self.s.get("/cgi/sleep", timeout=8)[0], 504)
        self.assertLess(time.time() - t, 5)
        time.sleep(0.8)
        self.assertEqual(subprocess.run(["pgrep", "-P", str(self.s.p.pid)], capture_output=True).stdout, b"")

    def test_client_leaving_kills_it(self):
        c = socket.create_connection(("127.0.0.1", self.s.port))
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\1\0\0\0\0\0\0\0")
        c.sendall(b"GET /cgi/sleep HTTP/1.1\r\n" + H + b"\r\n")
        time.sleep(0.3)
        c.close()
        time.sleep(1)
        self.assertEqual(subprocess.run(["pgrep", "-P", str(self.s.p.pid)], capture_output=True).stdout, b"")

    def test_limit_and_recovery(self):
        res = []
        ts = [threading.Thread(target=lambda: res.append(self.s.get("/cgi/slow_ok", timeout=10)[0])) for _ in range(10)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual((res.count(200), res.count(503)), (4, 6))
        end = time.time() + 4
        while self.s.get("/cgi/env")[0] != 200 and time.time() < end:
            time.sleep(0.1)
        self.assertLess(time.time(), end)

    def test_child_gets_no_other_fds(self):
        cs = [socket.create_connection(("127.0.0.1", self.s.port)) for _ in range(20)]
        self.assertEqual(self.s.get("/cgi/fds")[2], b"")
        for c in cs:
            c.close()

    def test_child_that_exits_before_its_output_ends(self):
        self.assertEqual(self.s.get("/cgi/early_exit")[2], b"START\nEND\n")
        c = socket.socket()  # 프로그램은 끝났는데 손님이 천천히 받는 경우
        c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        c.connect(("127.0.0.1", self.s.port))
        c.sendall(b"GET /cgi/large HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        time.sleep(0.8)
        out = b""
        c.settimeout(5)
        while d := c.recv(1 << 16):
            out += d
        c.close()
        self.assertEqual(parse(out)[2], b"x" * 120_000)

    def test_endless_output_does_not_hold_the_slot(self):
        for req in (b"HEAD /cgi/endless", b"GET /cgi/over"):
            for _ in range(4):
                c = socket.create_connection(("127.0.0.1", self.s.port))
                c.sendall(req + b" HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
                c.recv(1000)
                c.close()
            end = time.time() + 4
            while self.s.get("/cgi/status")[0] != 418 and time.time() < end:
                time.sleep(0.1)
            self.assertLess(time.time(), end, req)

    def test_head(self):
        st, _, body, _ = self.s.get("/cgi/status", method="HEAD")
        self.assertEqual((st, body), (418, b""))


class Observe(Case):
    @classmethod
    def files(cls, d):
        Path(d, "index.html").write_text("hello")

    def test_status_page_only_on_admin_port(self):
        self.assertEqual(self.s.get("/status.json")[0], 404)
        out, _ = self.s.raw(b"GET /status.json HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n", port=self.s.admin)
        import json
        st, _, body = parse(out)
        j = json.loads(body)
        self.assertEqual((st, len(j["loops"])), (200, 3))
        self.assertEqual(statuses(self.s.raw(b"GET / HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n", port=self.s.admin)[0]), [200])
        self.assertEqual(statuses(self.s.raw(b"POST / HTTP/1.1\r\n" + H + b"Content-Length: 0\r\nConnection: close\r\n\r\n", port=self.s.admin)[0]), [405])

    def test_connections_are_spread_over_loops(self):
        import json
        for _ in range(30):
            self.s.get("/")
        out, _ = self.s.raw(b"GET /status.json HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n", port=self.s.admin)
        got = [x["accepted"] for x in json.loads(parse(out)[2])["loops"]]
        self.assertGreater(min(got), 5, got)

    def test_access_log(self):
        self.s.get("/index.html?token=secret")
        self.s.raw(b"garbage\r\n\r\n")
        time.sleep(0.5)
        log = Path(self.s.dir, "access.log").read_text()
        self.assertRegex(log, r'127\.0\.0\.1 "GET /index.html" 200 5 \d+ms')
        self.assertIn(" 400 ", log)
        self.assertNotIn("secret", log)

    def test_blocked_log_does_not_block_the_server(self):
        r, w = os.pipe()
        port = free_port()
        p = subprocess.Popen([BIN, "-p", str(port), "-r", self.www, "-t", "1"], stderr=w)
        os.close(w)
        one = b"GET /index.html HTTP/1.1\r\n" + H + b"\r\n"

        def burst(c, n):
            c.sendall(one * n)
            got = 0
            while got < n:
                got += c.recv(1 << 20).count(b"200 OK")
        try:
            time.sleep(0.5)
            c = socket.create_connection(("127.0.0.1", port))
            c.settimeout(5)
            for _ in range(60):  # 로그를 읽지 않는 파이프가 꽉 차도 응답은 계속 나와야 한다
                burst(c, 50)
            os.read(r, 4096)  # 조금만 읽고 다시 멈춰도 마찬가지 (한 번에 크게 쓰면 여기서 잠든다)
            for _ in range(10):
                burst(c, 50)
            c.close()
            p.terminate()
            self.assertEqual(p.wait(5), 0)
        finally:
            os.close(r)
            if p.poll() is None:
                p.kill()
                p.wait()


# ---- 과부하, 제한 시간, 종료 ----

class Limits(unittest.TestCase):
    def serve(self, conf="", **kw):
        self.www = tempfile.mkdtemp(prefix="webserver-www-")
        Path(self.www, "index.html").write_text("hello")
        Path(self.www, "big").write_bytes(b"x" * (30 << 20))
        self.s = Server(f"route * / static {self.www}\n" + conf, **kw)
        return self.s

    def tearDown(self):
        shutil.rmtree(self.www, ignore_errors=True)
        if self.s.p.poll() is None:
            self.s.stop()

    def connect(self, data=b"", rcvbuf=0):
        c = socket.socket()
        if rcvbuf:
            c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        c.connect(("127.0.0.1", self.s.port))
        c.settimeout(5)
        if data:
            c.sendall(data)
        return c

    def test_header_drip_gets_408(self):
        s = self.serve("timeout_head 800ms\n")
        c = self.connect(b"GET / HTTP/1.1\r\n")
        t = time.time()
        try:
            while time.time() - t < 4:  # 0.2초마다 1바이트. 바이트마다 연장되면 끝나지 않는다
                c.sendall(b"X")
                time.sleep(0.2)
                c.setblocking(False)
                try:
                    if c.recv(100).startswith(b"HTTP/1.1 408"):
                        break
                except BlockingIOError:
                    pass
                c.setblocking(True)
        except OSError:
            pass
        self.assertLess(time.time() - t, 2)
        c.close()

    def test_silent_connection_is_closed(self):
        self.serve("timeout_head 500ms\n")
        c = self.connect()
        t = time.time()
        self.assertEqual(c.recv(10), b"")
        self.assertLess(time.time() - t, 2)

    def test_idle_keep_alive_is_closed(self):
        self.serve("timeout_idle 500ms\n")
        c = self.connect(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
        self.assertIn(b"hello", c.recv(4096))
        t = time.time()
        self.assertEqual(c.recv(10), b"")
        self.assertLess(time.time() - t, 2)

    def test_slow_body_gets_408(self):
        b = Backend(ok())
        self.serve(f"timeout_head 600ms\npool p 127.0.0.1:{b.port}\nroute * /up proxy p\n")
        c = self.connect(b"POST /up HTTP/1.1\r\n" + H + b"Content-Length: 1000\r\n\r\nabc")
        self.assertIn(b"408", c.recv(100))
        self.assertEqual(b.requests, [])  # 느린 업로드는 백엔드에 닿지도 않는다
        b.stop()

    def test_upload_that_stops_gives_the_disk_back(self):
        import json
        b = Backend(ok())
        s = self.serve(f"timeout_head 600ms\nmax_upload_disk 4m\npool p 127.0.0.1:{b.port}\nroute * /up proxy p\n")
        c = self.connect(b"POST /up HTTP/1.1\r\n" + H + b"Content-Length: 4194304\r\n\r\n" + b"x" * 3_900_000)
        time.sleep(0.2)
        self.assertEqual(s.get("/up", method="POST", body=b"y" * 400_000)[0], 503)
        time.sleep(1.5)  # 빨리 보내 벌어 둔 시간은 멈춘 순간 소용이 없어야 한다
        self.assertEqual(s.get("/up", method="POST", body=b"y" * 400_000)[0], 200)
        c.close()
        b.stop()

    def test_moving_upload_is_not_evicted(self):
        b = Backend(ok())
        s = self.serve(f"max_connections 2\nloops 1\npool p 127.0.0.1:{b.port}\nroute * /up proxy p\n")
        res = []

        def upload():
            c = socket.create_connection(("127.0.0.1", s.port))
            c.settimeout(10)
            try:
                c.sendall(b"POST /up HTTP/1.1\r\n" + H + b"Content-Length: 2000000\r\nConnection: close\r\n\r\n")
                for _ in range(20):
                    c.sendall(b"z" * 100_000)
                    time.sleep(0.05)
                res.append(c.recv(12))
            except OSError as e:
                res.append(e)
        ts = [threading.Thread(target=upload) for _ in range(2)]
        [t.start() for t in ts]
        time.sleep(0.4)
        self.assertEqual(s.get("/")[0], 503)
        [t.join() for t in ts]
        self.assertEqual(res, [b"HTTP/1.1 200"] * 2)
        b.stop()

    def test_requests_already_in_the_queue_survive_sigterm(self):
        s = self.serve()
        s.p.send_signal(signal.SIGSTOP)
        cs = [self.connect(b"GET / HTTP/1.1\r\n" + H + b"\r\n") for _ in range(100)]
        s.p.send_signal(signal.SIGTERM)
        s.p.send_signal(signal.SIGCONT)
        ok_ = 0
        for c in cs:
            try:
                ok_ += c.recv(1000).startswith(b"HTTP/1.1 200")
            except OSError:
                pass
            c.close()
        self.assertEqual(ok_, 100)
        s.stop()

    def test_reader_that_stops_is_cut(self):
        self.serve("timeout_stall 700ms\n")
        c = self.connect(b"GET /big HTTP/1.1\r\n" + H + b"\r\n", rcvbuf=4096)
        time.sleep(2)
        n = 0
        try:
            while d := c.recv(1 << 20):
                n += len(d)
        except ConnectionError:
            pass
        self.assertLess(n, 30 << 20)

    def test_silent_connections_over_the_limit_do_not_block_others(self):
        # 예전 구현의 결함: 침묵 4개에 9.7초 대기, 68개면 503
        s = self.serve("max_connections 300\n")
        idle = [self.connect() for _ in range(300)]
        idle += [self.connect(b"G") for _ in range(100)]  # 상한보다 100개 더
        t = time.time()
        for _ in range(30):
            self.assertEqual(s.get("/")[0], 200)
        self.assertLess(time.time() - t, 1.5)
        for c in idle:
            c.close()

    def test_busy_connections_are_not_evicted(self):
        s = self.serve("max_connections 3\nloops 1\n")
        busy = [self.connect(b"GET /big HTTP/1.1\r\n" + H + b"\r\n", rcvbuf=4096) for _ in range(3)]
        time.sleep(0.3)
        self.assertEqual(s.get("/")[0], 503)
        for c in busy:
            c.close()
        time.sleep(0.2)
        self.assertEqual(s.get("/")[0], 200)

    def test_finishes_in_flight_on_sigterm(self):
        s = self.serve()
        c = self.connect(b"GET /big HTTP/1.1\r\n" + H + b"\r\n", rcvbuf=65536)
        idle = self.connect(b"GET / HTTP/1.1\r\n" + H + b"\r\n")
        got = len(c.recv(1000))
        idle.recv(4096)
        s.p.send_signal(signal.SIGTERM)
        time.sleep(0.5)
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", s.port), 1)
        self.assertEqual(idle.recv(10), b"")  # 쉬던 연결은 바로 닫는다
        while d := c.recv(1 << 20):
            got += len(d)
        self.assertGreater(got, 30 << 20)
        c.close()
        s.stop()

    def test_drain_has_a_deadline_and_second_signal_is_immediate(self):
        for second in (False, True):
            s = self.serve(f"timeout_drain {'30s' if second else '500ms'}\n")
            c = self.connect(b"GET /big HTTP/1.1\r\n" + H + b"\r\n", rcvbuf=4096)
            time.sleep(0.2)
            t = time.time()
            s.p.send_signal(signal.SIGTERM)
            if second:
                time.sleep(0.3)
                s.p.send_signal(signal.SIGINT)
            s.p.wait(5)
            self.assertLess(time.time() - t, 2.5)
            c.close()
            s.stop()

    def test_clients_that_vanish(self):
        s = self.serve()
        before = len(os.listdir("/dev/fd"))
        rst = b"\1\0\0\0\0\0\0\0"
        for i in range(400):
            c = self.connect([b"", b"GET", b"GET /big HTTP/1.1\r\n" + H + b"\r\n", b"POST / HTTP/1.1\r\n" + H + b"Content-Length: 9\r\n\r\nab"][i % 4])
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, rst)
            c.close()
        time.sleep(0.6)
        self.assertEqual(s.get("/")[0], 200)
        out = subprocess.run(["lsof", "-p", str(s.p.pid)], capture_output=True).stdout.decode()
        self.assertLessEqual(len(re.findall(r"ESTABLISHED|CLOSE_WAIT", out)), 2, out)

    def test_low_fd_limit_lowers_max_connections(self):
        import resource
        s = self.serve("max_connections 1000\n", preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (400, 400)))
        self.assertIn("max_connections를", s.stderr())
        cs = [self.connect() for _ in range(100)]
        self.assertEqual(s.get("/")[0], 200)
        for c in cs:
            c.close()


@unittest.skipUnless(os.environ.get("FAULT"), "고장 주입 빌드에서만 (make e2e가 켠다)")
class Faults(unittest.TestCase):
    def test_disk_error_is_500_and_fd_shortage_is_503(self):
        # 예전 구현의 결함: fd가 모자라면 있는 파일도 404, 500을 보내는 길이 없음
        global BIN
        saved, BIN = BIN, str(ROOT / os.environ["FAULT"])
        s = Server(f"route * / static {ROOT}/www\n")
        try:
            self.assertEqual(s.get("/__fault/eio")[0], 500)
            self.assertEqual(s.get("/__fault/emfile")[0], 503)
        finally:
            BIN = saved
            s.stop()

# ---- 감독 프로세스 ----

class Supervisor(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="webserver-sup-")
        for n in ("a", "b"):
            Path(self.d, n).mkdir()
            Path(self.d, n, "index.html").write_text(n)
            Path(self.d, n, "big").write_bytes(n.encode() * (20 << 20))
        self.port = free_port()
        self.conf = Path(self.d, "c.conf")
        self.write("a")
        self.err = open(Path(self.d, "err"), "w+")
        self.p = subprocess.Popen([BIN, "-s", "-c", str(self.conf)], stderr=self.err)
        self.wait(lambda: self.get() == "a")

    def tearDown(self):
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait(15)
        self.err.close()
        shutil.rmtree(self.d, ignore_errors=True)

    def write(self, which, extra=""):
        self.conf.write_text(f"listen 127.0.0.1:{self.port}\nloops 2\naccess_log off\nroute * / static {self.d}/{which}\n{extra}")

    def get(self):
        try:
            c = socket.create_connection(("127.0.0.1", self.port), 2)
            c.settimeout(3)
            c.sendall(b"GET / HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
            out = b""
            while d := c.recv(4096):
                out += d
            c.close()
            return parse(out)[2].decode()
        except (OSError, ValueError):
            return None

    def wait(self, f, limit=8):
        end = time.time() + limit
        while time.time() < end:
            if f():
                return
            time.sleep(0.05)
        self.fail(Path(self.d, "err").read_text())

    def workers(self):
        return [int(x) for x in subprocess.run(["pgrep", "-P", str(self.p.pid)], capture_output=True).stdout.split()]

    def hup(self, which):
        n = Path(self.d, "err").read_text().count("준비됨")
        self.write(which)
        self.p.send_signal(signal.SIGHUP)
        self.wait(lambda: Path(self.d, "err").read_text().count("준비됨") > n)

    def test_reload_without_losing_a_request(self):
        big = socket.socket()
        big.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        big.connect(("127.0.0.1", self.port))
        big.settimeout(10)
        big.sendall(b"GET /big HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        got = big.recv(1000)
        lost, stop = [], threading.Event()

        def hammer():
            while not stop.is_set():
                if self.get() not in ("a", "b"):
                    lost.append(1)
        ts = [threading.Thread(target=hammer) for _ in range(12)]
        [t.start() for t in ts]
        for i in range(6):
            self.hup("ba"[i % 2])
        stop.set()
        [t.join() for t in ts]
        self.assertEqual(lost, [])
        while d := big.recv(1 << 20):  # 옛 워커가 하던 다운로드는 끝까지
            got += d
        self.assertEqual(len(got) - got.index(b"\r\n\r\n") - 4, 20 << 20)
        self.assertEqual(set(got[-1000:]), set(b"a"))
        big.close()
        self.wait(lambda: len(self.workers()) == 1)

    def test_stop_after_reload_still_finishes_old_downloads(self):
        big = socket.socket()
        big.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        big.connect(("127.0.0.1", self.port))
        big.settimeout(10)
        big.sendall(b"GET /big HTTP/1.1\r\n" + H + b"Connection: close\r\n\r\n")
        got = len(big.recv(1000))
        self.hup("b")
        self.p.terminate()  # 옛 워커에게는 이미 한 번 갔으므로, 또 보내 "당장 끝내라"로 만들면 안 된다
        time.sleep(0.5)
        while d := big.recv(1 << 20):
            got += len(d)
        self.assertGreater(got, 20 << 20)
        big.close()
        self.assertEqual(self.p.wait(10), 0)

    def test_address_change_is_refused_by_reload(self):
        self.conf.write_text(f"listen 0.0.0.0:{self.port}\nloops 2\naccess_log off\nroute * / static {self.d}/b\n")
        self.p.send_signal(signal.SIGHUP)
        self.wait(lambda: "시작하지 못했습니다" in Path(self.d, "err").read_text())
        self.assertEqual(self.get(), "a")

    def test_supervisor_does_not_give_up(self):
        exe = Path(self.d, "srv")
        shutil.copy(BIN, exe)
        self.p.terminate()
        self.p.wait(10)
        self.p = subprocess.Popen([str(exe), "-s", "-c", str(self.conf)], stderr=self.err)
        self.wait(lambda: self.get() == "a")
        w = self.workers()[0]
        exe.rename(str(exe) + "x")  # 실행 파일을 교체하는 순간 워커가 죽었다
        os.kill(w, signal.SIGKILL)
        time.sleep(0.6)
        Path(str(exe) + "x").rename(exe)
        self.wait(lambda: self.get() == "a")

    def test_supervisor_survives_a_lost_log_reader(self):
        self.p.terminate()
        self.p.wait(10)
        r, w = os.pipe()
        self.p = subprocess.Popen([BIN, "-s", "-c", str(self.conf)], stderr=w)
        os.close(w)
        self.wait(lambda: self.get() == "a")
        os.close(r)
        self.write("b")
        self.p.send_signal(signal.SIGHUP)
        self.wait(lambda: self.get() == "b")
        self.assertIsNone(self.p.poll())

    def test_bad_config_keeps_the_old_one(self):
        self.write("b", "nonsense here\n")
        self.p.send_signal(signal.SIGHUP)
        time.sleep(1)
        self.assertEqual(self.get(), "a")
        self.write("b")
        self.p.send_signal(signal.SIGHUP)
        self.wait(lambda: self.get() == "b")

    def test_crashed_worker_comes_back(self):
        w = self.workers()[0]
        os.kill(w, signal.SIGKILL)
        self.wait(lambda: self.workers() and self.workers()[0] != w and self.get() == "a")

    def test_stop_and_orphan(self):
        w = self.workers()[0]
        self.p.terminate()
        self.assertEqual(self.p.wait(10), 0)
        with self.assertRaises(ProcessLookupError):
            os.kill(w, 0)
        self.p = subprocess.Popen([BIN, "-s", "-c", str(self.conf)], stderr=self.err)
        self.wait(lambda: self.get() == "a")
        w = self.workers()[0]
        self.p.kill()  # 감독만 갑자기 죽어도 워커가 혼자 남지 않는다
        self.p.wait()

        def gone():
            try:
                os.kill(w, 0)
                return False
            except ProcessLookupError:
                return True
        self.wait(gone)

    def test_bad_config_at_start(self):
        bad = Path(self.d, "bad.conf")
        bad.write_text("nonsense x\n")
        self.assertEqual(subprocess.run([BIN, "-s", "-c", str(bad)], stderr=subprocess.DEVNULL, timeout=10).returncode, 2)


class Config(unittest.TestCase):
    def run_conf(self, text, *args):
        with tempfile.NamedTemporaryFile("w", suffix=".conf") as f:
            f.write(text)
            f.flush()
            p = subprocess.run([BIN, "-c", f.name, *args], stderr=subprocess.PIPE, timeout=5)
            return p.returncode, p.stderr.decode()

    def test_bad_line_tells_where(self):
        code, err = self.run_conf("# 주석\n\nlisten 127.0.0.1:0\nmax_body 1zz\n")
        self.assertEqual(code, 2)
        self.assertIn(".conf:4:", err)

    def test_port_in_use(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            s.listen()
            code, err = self.run_conf(f"listen 127.0.0.1:{s.getsockname()[1]}\nroute * / static {ROOT}/www\n")
        self.assertEqual(code, 1)
        self.assertIn("Address already in use", err)

    def test_restart_right_away(self):
        # SO_REUSEADDR가 없으면 TIME_WAIT 때문에 두 번째 실행이 실패한다
        s = Server(f"route * / static {ROOT}/www\n")
        self.assertEqual(s.get("/")[0], 200)
        port = s.port
        s.stop()
        code = None
        p = subprocess.Popen([BIN, "-p", str(port), "-r", f"{ROOT}/www"], stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.4)
            code = p.poll()
        finally:
            p.terminate()
            p.wait()
        self.assertIsNone(code)


if __name__ == "__main__":
    unittest.main(verbosity=1)
