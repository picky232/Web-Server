#!/bin/bash
# 캐시 통합 테스트: make itest   (필요 도구: curl, nc, ab)
cd "$(dirname "$0")/.." || exit 1

SERVER=./build/webserver
BACKEND=./build/backend
P=18081; BE=19011

TMP=$(mktemp -d)
PASS=0; FAIL=0
cleanup() {
    kill "$SPID" 2>/dev/null
    pkill -f "$BACKEND $BE" 2>/dev/null
    sleep 0.3
    [ -n "$KEEP" ] || rm -rf "$TMP"
}
trap cleanup EXIT

ok()  { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s  (%s)\n' "$1" "$2"; }
check()    { [ "$2" == "$3" ] && ok "$1" || bad "$1" "expected='$2' actual='$3'"; }
differ()   { [ "$2" != "$3" ] && ok "$1" || bad "$1" "두 값이 같음: '$2'"; }
contains() { [[ "$3" == *"$2"* ]] && ok "$1" || bad "$1" "'$2' not found in: ${3:0:160}"; }
absent()   { [[ "$3" != *"$2"* ]] && ok "$1" || bad "$1" "'$2' unexpectedly found"; }

cat > "$TMP/cache.conf" <<EOF
listen $P
docroot ./public
timeout_upstream 2s
cache_size       1m
cache_max_object 100k
cache_ttl        2s

upstream be {
    server 127.0.0.1:$BE
}
route /c/  be strip cache
route /nc/ be strip
EOF

$BACKEND $BE &
[ "$(uname)" == "Linux" ] && export ASAN_OPTIONS=detect_leaks=1
$SERVER -c "$TMP/cache.conf" 2> "$TMP/server.log" & SPID=$!
sleep 1.2
U=http://127.0.0.1:$P

# fetch URL [curl 옵션...] -> 전역 H(응답 헤더), B(본문)
fetch() { local out; out=$(curl -s -i --max-time 6 "$@" | tr -d '\r'); H=${out%%$'\n\n'*}; B=${out#*$'\n\n'}; }
xc()    { echo "$H" | grep -i '^x-cache:' | sed 's/^[^:]*: *//'; }
code()  { echo "$H" | head -1 | awk '{print $2}'; }

echo "== 기본 동작: MISS -> HIT"
fetch $U/c/c-maxage; b1=$B
check "첫 요청은 MISS" "MISS" "$(xc)"
fetch $U/c/c-maxage; b2=$B
check "두 번째 요청은 HIT" "HIT" "$(xc)"
check "HIT 본문은 첫 응답과 바이트까지 동일 (백엔드를 다시 호출하지 않음)" "$b1" "$b2"
contains "HIT 응답에 Age 헤더" "Age:" "$H"
contains "HIT 응답에도 Via 와 Content-Length 유지" "Via: 1.1 webserver" "$H"
check "HIT 는 keep-alive 로 응답" "keep-alive" "$(echo "$H" | grep -i '^connection:' | sed 's/^[^:]*: *//')"
fetch $U/c/c-maxage?i=1; b1=$B; fetch $U/c/c-maxage?i=2; b2=$B
check "쿼리가 다르면 별개 항목 (둘 다 MISS)" "MISS" "$(xc)"
differ "쿼리가 다르면 본문도 다름" "$b1" "$b2"
fetch $U/nc/c-maxage; fetch $U/nc/c-maxage
check "cache 옵션이 없는 route 는 X-Cache 헤더 자체가 없음" "" "$(xc)"

echo "== 신선도(TTL)"
fetch $U/c/c-maxage?n=1; first=$B
check "max-age=1: MISS" "MISS" "$(xc)"
fetch $U/c/c-maxage?n=1
check "max-age=1: 즉시 HIT" "HIT" "$(xc)"
sleep 2.2
fetch $U/c/c-maxage?n=1
check "max-age 지난 뒤 MISS" "MISS" "$(xc)"
differ "만료 후에는 백엔드에서 새 응답을 받음" "$first" "$B"
fetch $U/c/c-smax; fetch $U/c/c-smax
check "max-age=0 + s-maxage=60: 공유 캐시 지시가 우선해 HIT" "HIT" "$(xc)"
fetch $U/c/c-none; b1=$B; fetch $U/c/c-none
check "Cache-Control 없음: 기본 TTL(cache_ttl)로 HIT" "HIT" "$(xc)"
check "  기본 TTL 응답도 같은 본문" "$b1" "$B"
sleep 2.4
fetch $U/c/c-none
check "  기본 TTL(2s) 지나면 MISS" "MISS" "$(xc)"

echo "== 저장하면 안 되는 응답"
for kind in nostore private cookie expires 404none; do
    fetch $U/c/c-$kind; b1=$B; fetch $U/c/c-$kind
    if [ "$(xc)" == "MISS" ] && [ "$b1" != "$B" ]; then ok "$kind: 저장하지 않음 (매번 MISS, 본문 다름)"
    else bad "$kind" "x-cache=$(xc)"; fi
done
fetch $U/c/close; fetch $U/c/close
check "길이를 모르는(연결 종료로 끝나는) 응답은 저장하지 않음" "MISS" "$(xc)"
curl -s -o /dev/null --max-time 5 $U/c/c-trunc; rc=$?
check "잘린 응답: 클라이언트에는 오류로 전달 (curl exit 18)" "18" "$rc"
fetch $U/c/c-trunc
check "잘린 응답은 캐시에 저장되지 않음 (다시 요청해도 MISS)" "MISS" "$(xc)"

echo "== 요청 측 제어"
fetch "$U/c/c-maxage?t=nc"; fetch "$U/c/c-maxage?t=nc"; old=$B
check "준비: 항목이 캐시됨" "HIT" "$(xc)"
fetch "$U/c/c-maxage?t=nc" -H 'Cache-Control: no-cache'
check "요청 Cache-Control: no-cache -> BYPASS" "BYPASS" "$(xc)"
differ "  BYPASS 는 백엔드에서 새 응답을 받음" "$old" "$B"; fresh=$B
fetch "$U/c/c-maxage?t=nc"
check "  새 응답으로 캐시가 갱신되어 이후 HIT" "HIT" "$(xc)"
check "  갱신된 본문이 제공됨" "$fresh" "$B"
fetch "$U/c/c-maxage?t=pragma" -H 'Pragma: no-cache'
check "Pragma: no-cache -> BYPASS" "BYPASS" "$(xc)"

fetch "$U/c/c-maxage?t=auth" -H 'Authorization: Bearer secret'; a1=$B
check "Authorization 요청 -> BYPASS" "BYPASS" "$(xc)"
fetch "$U/c/c-maxage?t=auth" -H 'Authorization: Bearer secret'
differ "  Authorization 응답은 저장되지 않아 매번 새로 받음" "$a1" "$B"
fetch "$U/c/c-maxage?t=auth"
check "  (인증된 요청의 응답이 다른 사용자에게 새어 나가지 않음) 일반 요청은 MISS" "MISS" "$(xc)"
differ "  일반 요청이 받은 본문은 인증 요청의 본문이 아님" "$a1" "$B"

fetch "$U/c/c-maxage?t=ns" -H 'Cache-Control: no-store'
check "요청 no-store -> BYPASS" "BYPASS" "$(xc)"
fetch "$U/c/c-maxage?t=ns"
check "  no-store 요청의 응답은 저장되지 않음" "MISS" "$(xc)"

fetch "$U/c/c-maxage?t=range" -H 'Range: bytes=0-3'
check "Range 요청 -> BYPASS" "BYPASS" "$(xc)"

echo "== 무효화 / 메서드"
fetch "$U/c/c-maxage?t=post"; fetch "$U/c/c-maxage?t=post"; before=$B
check "준비: HIT" "HIT" "$(xc)"
curl -s -o /dev/null -X POST -d x "$U/c/c-maxage?t=post"
fetch "$U/c/c-maxage?t=post"
check "같은 URI 에 POST 하면 캐시가 무효화되어 MISS" "MISS" "$(xc)"
differ "  새 본문" "$before" "$B"
fetch -I "$U/c/c-maxage?t=head"
check "HEAD 는 캐시를 쓰지 않음 (X-Cache 없음)" "" "$(xc)"
fetch "$U/c/c-maxage?t=head"
check "  HEAD 가 항목을 만들지도 않음" "MISS" "$(xc)"

echo "== 응답 형식 보존"
fetch $U/c/c-chunked; b1=$B
check "chunked 응답: MISS" "MISS" "$(xc)"
contains "  본문 내용" " world" "$b1"
fetch $U/c/c-chunked
check "  chunked 응답도 HIT" "HIT" "$(xc)"
check "  HIT 본문이 MISS 본문과 동일" "$b1" "$B"
raw=$(curl -s -i --raw --max-time 5 $U/c/c-chunked | tr -d '\r')
contains "  HIT 도 Transfer-Encoding: chunked 와 종료 청크(0)를 그대로 재생" "Transfer-Encoding: chunked" "$raw"
check "  종료 청크로 끝남" "0" "$(echo "$raw" | grep -v '^$' | tail -1)"
fetch $U/c/c-404; fetch $U/c/c-404
check "명시적 max-age 가 있는 404 는 HIT" "HIT" "$(xc)"
check "  HIT 에서도 상태 코드 404 유지" "404" "$(code)"

echo "== Vary"
fetch $U/c/c-vary -H 'Accept-Language: en'
check "Vary: 첫 요청 MISS" "MISS" "$(xc)"; en=$B
fetch $U/c/c-vary -H 'Accept-Language: en'
check "  같은 언어는 HIT" "HIT" "$(xc)"
check "  HIT 본문 동일" "$en" "$B"
fetch $U/c/c-vary -H 'Accept-Language: fr'
check "  다른 언어는 MISS (영어 응답을 주면 안 됨)" "MISS" "$(xc)"
contains "  프랑스어 요청에는 프랑스어 응답" "lang=fr" "$B"
fetch $U/c/c-vary
check "  헤더가 없는 요청도 별개 변형이라 MISS" "MISS" "$(xc)"

echo "== 크기 / 메모리 상한 / LRU"
fetch "$U/c/c-big?n=200000"; fetch "$U/c/c-big?n=200000"
check "cache_max_object(100k) 초과 응답은 저장하지 않음" "MISS" "$(xc)"
check "  그래도 응답은 끝까지 정상 전달" "200000" "$(curl -s --max-time 5 "$U/c/c-big?n=200000" | wc -c | tr -d ' ')"
fetch "$U/c/c-big?n=50000&k=a"; fetch "$U/c/c-big?n=50000&k=a"
check "한도 안의 큰 응답은 HIT" "HIT" "$(xc)"
for i in $(seq 1 30); do curl -s -o /dev/null --max-time 5 "$U/c/c-big?n=50000&k=lru$i"; done
fetch "$U/c/c-big?n=50000&k=lru30"
check "LRU: 가장 최근 항목은 HIT" "HIT" "$(xc)"
fetch "$U/c/c-big?n=50000&k=lru1"
check "LRU: 1MB 를 넘겨 가장 오래된 항목은 퇴출되어 MISS" "MISS" "$(xc)"

echo "== keep-alive / 파이프라인 / HTTP/1.0 (HIT 응답)"
fetch "$U/c/c-maxage?t=ka"; fetch "$U/c/c-maxage?t=ka"
out=$(curl -s --max-time 5 "$U/c/c-maxage?t=ka" "$U/c/c-maxage?t=ka" | grep -c '^u=')
check "한 연결에서 HIT 두 번 연속" "2" "$out"
HOST="127.0.0.1:$P"   # 캐시 키에 Host 가 들어가므로, 앞서 curl 이 채운 항목과 같은 Host 를 써야 적중한다
out=$(printf "GET /c/c-maxage?t=ka HTTP/1.1\r\nHost: $HOST\r\n\r\nGET /c/c-maxage?t=ka HTTP/1.1\r\nHost: $HOST\r\nConnection: close\r\n\r\n" | nc -w 2 127.0.0.1 $P | tr -d '\r' | grep -ci '^x-cache: hit')
check "파이프라인으로 보낸 두 요청 모두 HIT" "2" "$out"
out=$(printf "GET /c/c-maxage?t=ka HTTP/1.0\r\nHost: $HOST\r\n\r\n" | nc -w 2 127.0.0.1 $P | tr -d '\r')
contains "HTTP/1.0 클라이언트의 HIT: Connection: close" "Connection: close" "$out"
contains "  HIT" "X-Cache: HIT" "$out"
out=$(printf "GET /c/c-maxage?t=ka HTTP/1.1\r\nHost: other.example\r\nConnection: close\r\n\r\n" | nc -w 2 127.0.0.1 $P | tr -d '\r')
contains "Host 가 다르면 별개 항목이라 MISS (가상 호스트별 캐시 분리)" "X-Cache: MISS" "$out"
out=$(printf "GET /c/c-maxage?t=ka HTTP/1.1\r\nHost: 127.0.0.1:$P\r\nConnection: close\r\n\r\n" | nc -w 2 127.0.0.1 $P | tr -d '\r')
contains "  원래 Host 의 항목은 그대로 HIT" "X-Cache: HIT" "$out"
out=$(printf "GET /c/c-maxage?t=ka HTTP/1.1\r\nHost: OTHER.Example\r\nConnection: close\r\n\r\n" | nc -w 2 127.0.0.1 $P | tr -d '\r')
contains "Host 대소문자는 구분하지 않음 (OTHER.Example == other.example)" "X-Cache: HIT" "$out"

echo "== 동시성"
fetch "$U/c/c-smax?t=load"
ab_out=$(ab -k -n 5000 -c 50 "$U/c/c-smax?t=load" 2>&1)
contains "캐시 적중 5000건/동시 50: 실패 0" "Failed requests:        0" "$ab_out"
absent   "  non-2xx 없음" "Non-2xx" "$ab_out"
echo "     $(echo "$ab_out" | grep 'Requests per second' | tr -s ' ')"
ab_out=$(ab -k -n 2000 -c 50 "$U/nc/c-smax?t=load" 2>&1)
echo "     캐시 없는 route(백엔드로 전달) $(echo "$ab_out" | grep 'Requests per second' | tr -s ' ')"

kill -TERM $SPID; wait $SPID 2>/dev/null; src=$?
sleep 0.3
check "SIGTERM 으로 정상 종료 (exit 0)" "0" "$src"
stat=$(grep '^cache:' "$TMP/server.log")
echo "     $stat"
val() { echo "$stat" | sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
[ "$(val hits)" -ge 5000 ] && ok "통계: hits=$(val hits)" || bad "통계 hits" "$(val hits)"
[ "$(val evictions)" -ge 1 ] && ok "통계: evictions=$(val evictions) (LRU 퇴출 발생)" || bad "통계 evictions" "$(val evictions)"
[ "$(val expired)" -ge 1 ] && ok "통계: expired=$(val expired) (TTL 만료 발생)" || bad "통계 expired" "$(val expired)"
[ "$(val invalidations)" -ge 1 ] && ok "통계: invalidations=$(val invalidations)" || bad "통계 invalidations" "$(val invalidations)"
[ "$(val bytes)" -le 1048576 ] && ok "통계: 메모리 사용 $(val bytes) 바이트 <= 상한 1048576" || bad "통계 bytes" "$(val bytes)"
if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error' "$TMP/server.log"; then
    bad "ASan/UBSan 검사" "서버 로그에 오류 있음"; KEEP=1; echo "$TMP/server.log"; head -40 "$TMP/server.log"
else
    ok "ASan/UBSan: 오류 없음"
fi

echo
echo "결과: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
