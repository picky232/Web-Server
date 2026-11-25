#!/usr/bin/env python3
"""부하 측정: ab(있으면 wrk도)로 동시 요청 수에 따른 처리량과, 침묵 연결이 있을 때의 지연을 잰다.

    make bench
"""
import os
import platform
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = str(ROOT / os.environ.get("WEBSERVER", "build/webserver"))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def ab(url, c, n, keep=True):
    cmd = ["ab", "-q", "-n", str(n), "-c", str(c)] + (["-k"] if keep else []) + [url]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout

    def num(pattern):
        m = re.search(pattern, out)
        return m.group(1) if m else "-"
    bad = int(re.search(r"Failed requests:\s+(\d+)", out).group(1)) if "Failed requests" in out else -1
    non2xx = re.search(r"Non-2xx responses:\s+(\d+)", out)
    return num(r"Requests per second:\s+([\d.]+)"), bad + (int(non2xx.group(1)) if non2xx else 0), num(r" 50%\s+(\d+)"), num(r" 99%\s+(\d+)")


def table(title, url, levels, n, keep=True):
    print(f"\n### {title}\n\n| 동시 | req/s | 실패 | p50 (ms) | p99 (ms) |\n|---|---|---|---|---|")
    for c in levels:
        rps, bad, p50, p99 = ab(url, c, n, keep)
        print(f"| {c} | {rps} | {'측정 실패' if bad < 0 else bad} | {p50} | {p99} |", flush=True)


def silent(port, pid, levels):
    print("\n### 침묵 연결이 있을 때 정상 요청의 지연\n\n| 침묵 연결 | 성공 | 중앙값 (ms) | 최댓값 (ms) | 서버 메모리 (MB) |\n|---|---|---|---|---|")
    held = []
    for target in levels:
        while len(held) < target:
            held.append(socket.create_connection(("127.0.0.1", port)))
        time.sleep(0.5)
        lat, ok = [], 0
        for _ in range(50):
            t = time.perf_counter()
            s = socket.create_connection(("127.0.0.1", port))
            s.settimeout(20)
            s.sendall(b"GET /1k.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            ok += s.recv(100).startswith(b"HTTP/1.1 200")
            s.close()
            lat.append((time.perf_counter() - t) * 1000)
        lat.sort()
        rss = int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)])) / 1024
        print(f"| {target} | {ok}/50 | {lat[25]:.2f} | {lat[-1]:.2f} | {rss:.1f} |", flush=True)
    for s in held:
        s.close()


def main():
    if not shutil.which("ab"):
        sys.exit("ab(Apache Bench)가 필요합니다")
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(max(soft, 20000), hard), hard))
    with tempfile.TemporaryDirectory() as d:
        Path(d, "1k.txt").write_text("x" * 1024)
        port, bports = free_port(), [free_port(), free_port()]
        conf = Path(d, "c.conf")
        conf.write_text(f"listen 127.0.0.1:{port}\naccess_log off\npool p {' '.join(f'127.0.0.1:{b}' for b in bports)}\n"
                        f"route * /api/ proxy p\nroute * / static {d}\n")
        ps = [subprocess.Popen([sys.executable, str(ROOT / "tests/backend.py"), str(b)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for b in bports]
        srv = subprocess.Popen([BIN, "-c", str(conf)], stderr=subprocess.DEVNULL)
        try:
            time.sleep(1.2)
            url = f"http://127.0.0.1:{port}"
            print(f"{platform.platform()}, 코어 {os.cpu_count()}, {BIN}")
            table("정적 1KB, keep-alive", url + "/1k.txt", (1, 4, 16, 64, 200, 500), 100000)
            table("정적 1KB, keep-alive 없음", url + "/1k.txt", (8, 200), 5000, keep=False)
            time.sleep(3)
            table("리버스 프록시 (Python 백엔드 2대가 한계)", url + "/api/x", (16, 64, 200), 20000)
            if shutil.which("wrk"):
                print("\n### wrk (여러 스레드)\n")
                print(subprocess.run(["wrk", "-t4", "-c256", "-d5s", "--latency", url + "/1k.txt"], capture_output=True, text=True).stdout)
            silent(port, srv.pid, (0, 4, 1000, 4596))
        finally:
            srv.send_signal(signal.SIGTERM)
            srv.wait(20)
            for p in ps:
                p.kill()
                p.wait()


if __name__ == "__main__":
    main()
