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
        self.assertEqual(self.b[0].connections, 1)  # 백엔드 연결 하나를 계속 다시 썼다

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
