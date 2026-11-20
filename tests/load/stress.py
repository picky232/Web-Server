#!/usr/bin/env python3
"""Simple concurrent load generator (substitute for ab/wrk/hey, stdlib only).

Starts the backend fixture and the C server (unless --port is given), then runs
C client threads, each sending N requests over keep-alive connections, mixing
static files and proxied /api/hello calls. Prints throughput and latency.

    make load
    python3 tests/load/stress.py --clients 50 --requests 200 --threads 8
    python3 tests/load/stress.py --port 8080 --no-proxy    # against a running server

Numbers depend heavily on the machine (and on Python being the client); treat
them as a functional stress check, not a benchmark.
"""
import argparse
import http.client
import os
import re
import signal
import statistics
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tests" / "integration"))
from backend import start_backend  # noqa: E402


def start_server(bin_path, threads, backend_port):
    args = [bin_path, "--bind", "127.0.0.1", "--port", "0", "--root", str(ROOT / "www"),
            "--threads", str(threads), "--queue", "1024", "--quiet"]
    if backend_port:
        args += ["--backend", f"127.0.0.1:{backend_port}"]
    proc = subprocess.Popen(args, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL)
    line = proc.stderr.readline().decode()
    m = re.search(r"listening on http://[\d.]+:(\d+)", line)
    if not m:
        proc.kill()
        sys.exit(f"server failed to start: {line}")
    threading.Thread(target=lambda: proc.stderr.read(), daemon=True).start()
    return proc, int(m.group(1))


def client(port, n, paths, kinds):
    """Returns (latencies, errors). Every non-200 / exception counts as an error;
    `kinds` collects a breakdown (shared dict, guarded by the GIL for += on ints)."""
    lat, errors = [], 0
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    for i in range(n):
        path = paths[i % len(paths)]
        t0 = time.perf_counter()
        try:
            conn.request("GET", path)
            r = conn.getresponse()
            r.read()
            if r.status != 200:
                errors += 1
                kinds[f"HTTP {r.status}"] = kinds.get(f"HTTP {r.status}", 0) + 1
            if r.getheader("Connection", "").lower() == "close":
                conn.close()
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        except Exception as e:
            errors += 1
            name = type(e).__name__
            kinds[name] = kinds.get(name, 0) + 1
            conn.close()
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        lat.append((time.perf_counter() - t0) * 1000)
    conn.close()
    return lat, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clients", type=int, default=50)
    ap.add_argument("--requests", type=int, default=200, help="requests per client")
    ap.add_argument("--threads", type=int, default=8, help="server worker threads")
    ap.add_argument("--port", type=int, help="use an already running server")
    ap.add_argument("--no-proxy", action="store_true", help="static files only")
    ap.add_argument("--bin", default=os.environ.get("WEBSERVER_BIN", str(ROOT / "build/webserver")))
    a = ap.parse_args()

    proc = backend = None
    paths = ["/index.html", "/style.css", "/app.js", "/images/logo.png"]
    if not a.no_proxy:
        paths.append("/api/hello")
    port = a.port
    if port is None:
        bport = None
        if not a.no_proxy:
            backend, bport = start_backend(name="A")
        proc, port = start_server(a.bin, a.threads, bport)

    total = a.clients * a.requests
    print(f"load: {a.clients} clients x {a.requests} req = {total} requests, "
          f"server threads={a.threads}, paths={paths}")
    kinds = {}
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=a.clients) as ex:
        results = list(ex.map(lambda _: client(port, a.requests, paths, kinds), range(a.clients)))
    elapsed = time.perf_counter() - t0
    lat = sorted(x for r in results for x in r[0])
    errors = sum(r[1] for r in results)

    def pct(p):
        return lat[min(len(lat) - 1, int(len(lat) * p))]

    print(f"elapsed {elapsed:.2f}s  throughput {total / elapsed:.0f} req/s  errors {errors}"
          + (f"  {kinds}" if kinds else ""))
    print(f"latency ms: mean {statistics.mean(lat):.2f}  p50 {pct(0.5):.2f}  "
          f"p90 {pct(0.9):.2f}  p99 {pct(0.99):.2f}  max {lat[-1]:.2f}")

    rc = 0 if errors == 0 else 1
    if proc:
        proc.send_signal(signal.SIGINT)
        code = proc.wait(15)
        print(f"server exit code after SIGINT: {code}")
        rc = rc or code
    if backend:
        backend.shutdown()
    return rc


if __name__ == "__main__":
    sys.exit(main())
