#!/usr/bin/env bash
# curl-based smoke test: starts the backend fixture + the C server, checks
# status codes / headers / bodies with real curl, then stops both with SIGINT.
#
#   make smoke        (or)   WEBSERVER_BIN=build/webserver bash tests/integration/curl_smoke.sh
set -u
cd "$(dirname "$0")/../.."

BIN=${WEBSERVER_BIN:-build/webserver}
LOG_DIR=build/test-logs/smoke
mkdir -p "$LOG_DIR"
BPORT=${BACKEND_PORT:-19090}
PORT=${SERVER_PORT:-18090}
BASE="http://127.0.0.1:$PORT"
PASS=0
FAIL=0

python3 tests/integration/backend.py --port "$BPORT" >"$LOG_DIR/backend.log" 2>&1 &
BE_PID=$!
"$BIN" --bind 127.0.0.1 --port "$PORT" --root ./www --threads 4 \
       --backend "127.0.0.1:$BPORT" --proxy-timeout-ms 1000 >"$LOG_DIR/server.log" 2>&1 &
SRV_PID=$!
cleanup() { kill -INT "$SRV_PID" 2>/dev/null; kill "$BE_PID" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 50); do
  curl -s -o /dev/null "$BASE/" && curl -s -o /dev/null "http://127.0.0.1:$BPORT/api/hello" && break
  sleep 0.1
done

check() { # name expected actual
  if [ "$2" = "$3" ]; then PASS=$((PASS + 1)); echo "PASS  $1 ($3)";
  else FAIL=$((FAIL + 1)); echo "FAIL  $1: expected '$2' got '$3'"; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

check "GET / html"            200 "$(code "$BASE/")"
check "Content-Type html"     "text/html; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/")"
check "GET css"               "text/css; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/style.css")"
check "GET js"                "text/javascript; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/app.js")"
check "GET png type"          image/png "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/images/logo.png")"
curl -s -o "$LOG_DIR/logo.png" "$BASE/images/logo.png"
check "png bytes identical"   same "$(cmp -s "$LOG_DIR/logo.png" www/images/logo.png && echo same || echo different)"
check "Content-Length = size" "$(stat -c %s www/index.html)" "$(curl -s -o /dev/null -w '%{size_download}' "$BASE/index.html")"
check "HEAD"                  200 "$(code -I "$BASE/style.css")"
check "404"                   404 "$(code "$BASE/no-such-file")"
check "301 dir redirect"      301 "$(code "$BASE/about")"
check "dir index"             200 "$(code "$BASE/about/")"
check "403 traversal ../"     403 "$(code --path-as-is "$BASE/../../etc/passwd")"
check "403 traversal %2e%2e"  403 "$(code --path-as-is "$BASE/%2e%2e/%2e%2e/etc/passwd")"
check "405 POST static"       405 "$(code -X POST -d x=1 "$BASE/index.html")"
# curl always sends well-formed requests, so malformed ones go through a raw socket
raw_status() { printf "$1" | timeout 5 python3 -c "
import socket,sys; s=socket.create_connection(('127.0.0.1',$PORT)); s.sendall(sys.stdin.buffer.read()); print(s.recv(100).split()[1].decode())"; }
check "400 malformed line"    400 "$(raw_status 'garbage\r\n\r\n')"
check "400 missing Host"      400 "$(raw_status 'GET / HTTP/1.1\r\n\r\n')"
check "505 HTTP/2.0 line"     505 "$(raw_status 'GET / HTTP/2.0\r\nHost: a\r\n\r\n')"
check "501 chunked request"   501 "$(code -X POST -H 'Transfer-Encoding: chunked' --data-binary x "$BASE/api/echo")"
check "413 body too large"    413 "$(head -c 2000000 /dev/zero | curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Expect:' --data-binary @- "$BASE/api/echo")"
check "414 long URI"          414 "$(code "$BASE/$(head -c 3000 /dev/zero | tr '\0' a)")"
check "431 big header"        431 "$(code -H "X-Big: $(head -c 9000 /dev/zero | tr '\0' b)" "$BASE/")"
check "proxy GET"             200 "$(code "$BASE/api/hello")"
check "proxy POST echo"       '"body": "hello from curl"' "$(curl -s -X POST --data 'hello from curl' "$BASE/api/echo" | grep -o '"body": "hello from curl"')"
check "proxy large POST"      100000 "$(head -c 100000 /dev/zero | curl -s -X POST --data-binary @- "$BASE/api/echo" | python3 -c 'import json,sys; print(json.load(sys.stdin)["body_len"])')"
check "proxy 504 timeout"     504 "$(code "$BASE/api/slow?ms=3000")"
check "proxy 502 hangup"      502 "$(code "$BASE/api/hangup")"
check "keep-alive reuse"      "1 0" "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$BASE/" "$BASE/style.css" | xargs)"

kill -INT "$SRV_PID"
wait "$SRV_PID"
check "SIGINT exit code"      0 "$?"
grep -q "shutdown complete" "$LOG_DIR/server.log" && check "shutdown logged" yes yes || check "shutdown logged" yes no
trap - EXIT
kill "$BE_PID" 2>/dev/null

echo
echo "curl smoke: $PASS passed, $FAIL failed (logs: $LOG_DIR)"
[ "$FAIL" -eq 0 ]
