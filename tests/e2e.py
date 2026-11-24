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
            f.write(f"listen 127.0.0.1:{self.port}\nadmin 127.0.0.1:{self.admin}\nloops 3\naccess_log {self.dir}/access.log\n" + conf)
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
