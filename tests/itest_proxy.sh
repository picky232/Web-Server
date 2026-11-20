#!/bin/bash
# 리버스 프록시 통합 테스트: make itest
# 필요 도구: curl, nc, ab, cmp  (모두 검증용이며 서버 자체는 C 로만 구현되어 있다)
cd "$(dirname "$0")/.." || exit 1

SERVER=./build/webserver
BACKEND=./build/backend
P=18080            # 프록시
B1=19001; B2=19002; DEAD=19099
B3=19003   # 연결 재사용 확인용 (conn 번호를 세는 단일 서버)
B4=19004   # -t 300: 유휴 300ms 후 조용히 닫는 백엔드
B6=19006   # -m 1: 응답 1번 후 예고 없이 닫는 백엔드 (부하 시 stale 경쟁 유발)
B7=19007   # -m 1 -d 200: 응답 후 200ms 뒤에 닫는 백엔드 (stale 재시도 재현용)

TMP=$(mktemp -d)
PIDS=()
PASS=0; FAIL=0

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    sleep 0.3
    pkill -f "$BACKEND 190" 2>/dev/null   # 부모와 fork 된 자식 모두 (19001~19007)
    [ -n "$KEEP" ] || rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s  (%s)\n' "$1" "$2"; }
check() { # check "이름" "기대" "실제"
    if [ "$2" == "$3" ]; then ok "$1"; else bad "$1" "expected='$2' actual='$3'"; fi
}
contains() { # contains "이름" "찾을 문자열" "대상"
    if [[ "$3" == *"$2"* ]]; then ok "$1"; else bad "$1" "'$2' not found in: ${3:0:200}"; fi
}
absent() {
    if [[ "$3" != *"$2"* ]]; then ok "$1"; else bad "$1" "'$2' unexpectedly found"; fi
}

cat > "$TMP/proxy.conf" <<EOF
listen $P
docroot ./public
timeout_header   1s
timeout_idle     1s
timeout_connect  1s
timeout_upstream 1s
pool_size        16
timeout_pool     1s

upstream solo {
    server 127.0.0.1:$B3
}
upstream idle {
    server 127.0.0.1:$B4
}
upstream m1 {
    server 127.0.0.1:$B6
}
upstream stale {
    server 127.0.0.1:$B7
}
route /solo/  solo  strip
route /idle/  idle  strip
route /m1/    m1    strip
route /stale/ stale strip

upstream api   {
    server 127.0.0.1:$B1
    server 127.0.0.1:$B2
}
upstream dead  {
    server 127.0.0.1:$DEAD
}
upstream mixed {
    server 127.0.0.1:$B1
    server 127.0.0.1:$DEAD
}
route /api/   api   strip
route /raw/   api
route /dead/  dead  strip
route /mixed/ mixed strip
EOF

$BACKEND $B1 & PIDS+=($!)
$BACKEND $B2 & PIDS+=($!)
$BACKEND $B3 & PIDS+=($!)
$BACKEND $B4 -t 300 & PIDS+=($!)
$BACKEND $B6 -m 1 & PIDS+=($!)
$BACKEND $B7 -m 1 -d 200 & PIDS+=($!)
# LeakSanitizer 는 Linux 에서만 지원된다 (macOS 에서는 옵션을 주면 시작 시 abort). macOS 누수는 `leaks` 로 따로 검사.
[ "$(uname)" == "Linux" ] && export ASAN_OPTIONS=detect_leaks=1
$SERVER -c "$TMP/proxy.conf" 2> "$TMP/server.log" & SPID=$!; PIDS+=($SPID)
sleep 1.2
U=http://127.0.0.1:$P
C="curl -s --max-time 5"

echo "== 라우팅 / 로드밸런싱"
r=$(for i in 1 2 3 4; do $C $U/api/whoami; done | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
check "Round-Robin 이 두 백엔드에 균등 분배" " 2 backend-$B1  2 backend-$B2 " "$r"
check "strip: /api/whoami -> 백엔드의 /whoami" "200" "$($C -o /dev/null -w '%{http_code}' $U/api/whoami)"
check "strip 없음: /raw/whoami 는 백엔드에 그대로 전달되어 404" "404" "$($C -o /dev/null -w '%{http_code}' $U/raw/whoami)"
contains "route 에 안 맞으면 정적 서빙" "It works!" "$($C $U/)"
contains "POST 는 정적 경로에서 405, 프록시 경로에서는 전달" "405" "$($C -o /dev/null -w '%{http_code}' -X POST -d x $U/index.html)"

echo "== 헤더 재작성"
h=$($C -H 'Connection: keep-alive' -H 'Keep-Alive: 5' -H 'Expect: 100-continue' -H 'X-Forwarded-For: 9.9.9.9' $U/api/headers)
contains "X-Forwarded-For 에 접속자 IP 를 덧붙임" "X-Forwarded-For: 9.9.9.9, 127.0.0.1" "$h"
contains "풀이 켜져 있으면 백엔드에 Connection: keep-alive" "Connection: keep-alive" "$h"
check "Connection 헤더는 하나만 전달" "1" "$(echo "$h" | grep -ci '^connection:')"
absent   "Keep-Alive 헤더는 전달하지 않음" "Keep-Alive" "$h"
absent   "Expect 헤더는 전달하지 않음" "Expect" "$h"
resp=$($C -i $U/api/whoami | tr -d '\r')
contains "응답에 Via 추가" "Via: 1.1 webserver" "$resp"
contains "클라이언트에는 keep-alive 로 응답" "Connection: keep-alive" "$resp"

echo "== 본문 framing"
check "POST 본문 에코" "hello=world" "$($C -X POST -d 'hello=world' $U/api/echo)"
head -c 700000 /dev/urandom > "$TMP/post.bin"
$C -X POST --data-binary @"$TMP/post.bin" -o "$TMP/post.out" $U/api/echo
cmp -s "$TMP/post.bin" "$TMP/post.out" && ok "700KB POST 본문 왕복 일치" || bad "700KB POST 본문" "달라짐"
check "chunked 응답 본문" "hello world" "$($C $U/api/chunked)"
resp=$($C -i --raw $U/api/chunked | tr -d '\r')
contains "chunked 는 chunked 로 그대로 전달" "Transfer-Encoding: chunked" "$resp"
check "close-delimited 응답 본문" "close-delimited-body" "$($C $U/api/close)"
contains "close-delimited 는 클라이언트에 Connection: close" "Connection: close" "$($C -i $U/api/close | tr -d '\r')"
check "204 No Content" "204" "$($C -o /dev/null -w '%{http_code}' $U/api/nocontent)"
check "백엔드 404 를 그대로 전달" "404" "$($C -o /dev/null -w '%{http_code}' "$U/api/status?code=404")"
check "백엔드 500 을 그대로 전달" "500" "$($C -o /dev/null -w '%{http_code}' "$U/api/status?code=500")"
check "20MB 응답 크기" "20000000" "$($C -o /dev/null -w '%{size_download}' "$U/api/big?n=20000000")"
resp=$($C -I $U/api/whoami | tr -d '\r')
contains "HEAD: Content-Length 만 있고 본문 없음" "Content-Length: 14" "$resp"   # "backend-19001\n" = 14바이트

echo "== keep-alive 연결 재사용 (프록시 응답 경계가 정확해야 다음 요청이 성공)"
out=$($C $U/api/chunked $U/api/whoami $U/api/close 2>&1 | tr '\n' '|')
contains "한 연결에서 chunked -> CL 응답 연속 처리" "hello world" "$out"
contains "chunked 다음 요청도 성공" "backend-" "$out"
out=$(printf 'GET /api/whoami HTTP/1.1\r\nHost: x\r\n\r\nGET /api/whoami HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc -w 2 127.0.0.1 $P | grep -c '^HTTP/1.1 200')
check "파이프라인 2요청 모두 200" "2" "$out"
check "HTTP/1.0 요청에는 Connection: close" "1" "$(printf 'GET /api/whoami HTTP/1.0\r\n\r\n' | nc -w 2 127.0.0.1 $P | tr -d '\r' | grep -c '^Connection: close')"

echo "== 백엔드 연결 풀"
exp=""; for i in $(seq 1 10); do exp="${exp}conn=1 req=$i "; done
got=$(for i in $(seq 1 10); do $C $U/solo/conn; done | tr '\n' ' ')
check "10번 요청이 백엔드의 같은 연결(conn=1)을 재사용" "$exp" "$got"

got=$($C $U/idle/conn | tr '\n' ' '; sleep 0.8; $C $U/idle/conn | tr '\n' ' ')
check "백엔드가 유휴 연결을 닫으면 풀에서 버리고 새 연결 사용" "conn=1 req=1 conn=2 req=1 " "$got"

got=$($C $U/solo/conn | tr '\n' ' '; sleep 2.6; $C $U/solo/conn | tr '\n' ' ')
check "timeout_pool(1s) 지나면 유휴 연결을 정리하고 새 연결 사용" "conn=1 req=11 conn=2 req=1 " "$got"

ab_out=$(ab -n 2000 -c 50 $U/m1/whoami 2>&1)
contains "응답마다 닫는 백엔드에 동시 2000건: 실패 0 (재사용 경쟁 포함)" "Failed requests:        0" "$ab_out"
absent   "  non-2xx 없음" "Non-2xx" "$ab_out"

# stale 재현: 백엔드가 풀 안의 연결을 닫는 시점보다 클라이언트 요청이 먼저 도착하도록 프록시를 잠시 멈춘다.
# SIGCONT 후 이벤트 큐에서 클라이언트 요청이 풀 연결의 EOF 보다 먼저 처리되므로, 이미 닫힌 연결을 꺼내 쓰게 된다.
recv_resp() { # fd 3 에서 응답 하나를 읽어 "코드 본문" 을 출력
    local line code cl=0 lc
    IFS= read -r -t 5 -u 3 line || { echo "ERR"; return; }
    code=$(echo "$line" | awk '{print $2}')
    while IFS= read -r -t 5 -u 3 line; do
        line=${line%$'\r'}
        [ -z "$line" ] && break
        lc=$(printf '%s' "$line" | tr 'A-Z' 'a-z')
        case "$lc" in content-length:*) cl=$(echo "${lc#*:}" | tr -d ' ');; esac
    done
    local body=""
    [ "$cl" -gt 0 ] && body=$(head -c "$cl" <&3)
    echo "$code $body"
}
stale_scenario() { # $1: 두 번째 요청 (printf 형식)
    exec 3<>/dev/tcp/127.0.0.1/$P
    printf 'GET /stale/whoami HTTP/1.1\r\nHost: x\r\n\r\n' >&3
    recv_resp > "$TMP/first.out"
    kill -STOP $SPID
    printf "$1" >&3
    sleep 0.6                      # 백엔드가 200ms 뒤 풀의 연결을 닫는다
    kill -CONT $SPID
    recv_resp
    exec 3<&- 3>&-
}
r=$(stale_scenario 'GET /stale/whoami HTTP/1.1\r\nHost: x\r\n\r\n')
check "stale 연결: GET 은 새 연결로 재시도해 성공" "200 backend-$B7" "$r"
r=$(stale_scenario 'POST /stale/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc')
check "stale 연결: POST 는 중복 실행 위험으로 재시도하지 않고 502" "502 502 Bad Gateway" "$r"

echo "== 장애 처리"
check "깨진 백엔드 응답 -> 502" "502" "$($C -o /dev/null -w '%{http_code}' $U/api/badresp)"
check "죽은 백엔드(connection refused) -> 502" "502" "$($C -o /dev/null -w '%{http_code}' $U/dead/x)"
r=$(for i in 1 2 3 4 5 6; do $C -o /dev/null -w '%{http_code} ' $U/mixed/whoami; done)
check "서버 하나가 죽어도 다른 서버로 재시도해 모두 200" "200 200 200 200 200 200 " "$r"
$C -o /dev/null $U/api/truncate; rc=$?
check "잘린 응답: 클라이언트 연결을 끊어 오류로 알림 (curl exit 18)" "18" "$rc"
s=$SECONDS
code=$($C -o /dev/null -w '%{http_code}' $U/api/hang)
check "백엔드 무응답 -> 504" "504" "$code"
[ $((SECONDS - s)) -le 3 ] && ok "504 는 timeout_upstream(1s) 근처에서 반환" || bad "504 지연" "$((SECONDS - s))s"
check "느리지만 제한시간 안의 응답은 성공" "200" "$($C -o /dev/null -w '%{http_code}' "$U/api/slow?ms=300")"

echo "== 타임아웃 (Slowloris / 유휴)"
# bash 내장 /dev/tcp 로 연결을 열고, 서버가 연결을 닫는(EOF) 순간까지의 시간을 잰다.
# (파이프라인 + nc 는 쓰는 쪽 프로세스가 끝날 때까지 기다려서 측정이 왜곡된다)
wait_for_close() { # wait_for_close "보낼 문자열" -> 서버가 닫을 때까지 걸린 초, 5초 안에 안 닫으면 99
    local start=$SECONDS
    exec 3<>/dev/tcp/127.0.0.1/$P || { echo 98; return; }
    [ -n "$1" ] && printf "$1" >&3
    read -t 5 -u 3 _                           # 서버가 닫으면(EOF) 즉시 반환, 5초 내 안 닫으면 타임아웃(>128)
    local rc=$?
    exec 3<&- 3>&-
    [ $rc -gt 128 ] && echo 99 || echo $((SECONDS - start))
}
t=$(wait_for_close 'GET /index.html HTTP/1.1\r\nHost: x\r\nX-Slow: ')
[ "$t" -le 2 ] && ok "헤더를 끝내지 않는 연결을 header 제한시간(1s) 후 끊음 (${t}s)" || bad "Slowloris" "${t}s"
t=$(wait_for_close '')
[ "$t" -le 2 ] && ok "아무것도 안 보내는 연결을 idle 제한시간(1s) 후 끊음 (${t}s)" || bad "idle" "${t}s"

echo "== 동시성 / 비정상 종료"
ab_out=$(ab -k -n 3000 -c 50 $U/api/whoami 2>&1)
contains "ab -k 3000건/동시 50: 실패 0" "Failed requests:        0" "$ab_out"
absent   "ab: non-2xx 없음" "Non-2xx" "$ab_out"
$C -m 0.2 -o /dev/null "$U/api/big?n=200000000"
$C -m 0.2 -o /dev/null "$U/api/big?n=200000000"
sleep 0.5
check "대용량 전송 중 클라이언트가 끊어도 서버는 정상" "200" "$($C -o /dev/null -w '%{http_code}' $U/api/whoami)"

kill -TERM $SPID; wait $SPID 2>/dev/null; src=$?
sleep 0.3
check "SIGTERM 으로 정상 종료 (exit 0)" "0" "$src"
stat=$(grep '^pool:' "$TMP/server.log")
echo "     $stat"
reused=$(echo "$stat" | sed -n 's/.*reused=\([0-9]*\).*/\1/p')
stale=$(echo "$stat" | sed -n 's/.*stale_retries=\([0-9]*\).*/\1/p')
[ "${reused:-0}" -ge 9 ] && ok "풀 통계: 연결 재사용 횟수 ${reused}" || bad "풀 통계" "reused=${reused:-없음}"
[ "${stale:-0}" -ge 1 ] && ok "풀 통계: stale 재시도 경로가 실제로 실행됨 (${stale}회)" || bad "풀 통계" "stale_retries=${stale:-없음}"
if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error' "$TMP/server.log"; then
    bad "ASan/UBSan 검사" "서버 로그에 오류 있음 (KEEP=1 로 로그 보존)"; KEEP=1; echo "$TMP/server.log"; head -40 "$TMP/server.log"
else
    ok "ASan/UBSan: 오류·누수 없음"
fi

echo
echo "결과: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
