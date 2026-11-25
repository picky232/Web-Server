#!/usr/bin/env python3
"""데모용 앱 백엔드. 응답에 자기 포트를 적어 어느 백엔드가 받았는지 보이게 한다.

    python3 tests/backend.py 8001
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class App(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sick = False

    def answer(self, code, obj):
        body = (json.dumps(obj, ensure_ascii=False) + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path == "/health":
            self.answer(200, {"ok": True})
        elif path.endswith("/slow"):
            time.sleep(float(query or 1))
            self.answer(200, {"port": self.server.server_port, "slept": query})
        elif path.endswith("/fail"):
            self.answer(503, {"port": self.server.server_port})
        else:
            self.answer(200, {"port": self.server.server_port, "method": self.command, "path": self.path,
                              "headers": dict(self.headers)})

    do_HEAD = do_GET

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.answer(200, {"port": self.server.server_port, "method": self.command, "path": self.path,
                          "size": len(body), "body": body[:200].decode("utf-8", "replace")})

    do_PUT = do_POST

    def log_message(self, *args):
        pass


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128  # 기본값 5로는 동시 접속이 몰릴 때 커널이 연결을 끊는다

    def handle_error(self, request, client_address):
        pass  # 상대가 먼저 끊은 것은 데모에서 흔한 일


if __name__ == "__main__":
    server = Server(("127.0.0.1", int(sys.argv[1])), App)
    print(f"backend on {sys.argv[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
