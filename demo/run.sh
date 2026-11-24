#!/bin/sh
# make demo && demo/run.sh   전부 켜기 (끄기: Ctrl+C)
# demo/run.sh 9001           백엔드 하나만 다시 켜기
cd "$(dirname "$0")/.." || exit 1
if [ $# -gt 0 ]; then
    exec build/seats -c "demo/backend-$1.conf"
fi
mkdir -p build/demo

# Ctrl+C는 세 프로그램에도 바로 간다. 여기서 또 보내면 두 번째 신호라 처리 중인 예매까지 끊긴다
trap 'trap - EXIT; wait; exit 130' INT
trap 'trap - EXIT; kill $front $one $two 2>/dev/null; wait; exit' TERM HUP EXIT
build/webserver -c demo/front.conf 2>build/demo/front.log &
front=$!
build/seats -c demo/backend-9001.conf 2>build/demo/9001.log &
one=$!
build/seats -c demo/backend-9002.conf 2>build/demo/9002.log &
two=$!
sleep 1
for pid in $front $one $two; do
    if ! kill -0 "$pid" 2>/dev/null; then
        cat build/demo/front.log build/demo/9001.log build/demo/9002.log
        echo "켜지 못했습니다. 남아 있는 데모가 있으면 먼저 꺼 주세요: pkill -f demo/backend-; pkill -f demo/front.conf"
        exit 1
    fi
done

# 켤 때마다 빈 좌석에서 시작. 파일을 지우면 이미 열어 둔 백엔드는 옛 파일을 계속 보므로, 내용만 0으로 덮는다
dd if=/dev/zero of=demo/seats.dat bs=$(($(wc -c <demo/seats.dat))) count=1 conv=notrunc 2>/dev/null

cat <<EOF
데모 화면             http://localhost:9000
9001 끄기             kill -9 $one
9001 다시 켜기        demo/run.sh 9001
손님 몰리기           ab -n 200 -c 40 -p demo/crowd.txt -T application/x-www-form-urlencoded http://127.0.0.1:9000/api/book
먼저 보내는 것 보기   curl -N http://127.0.0.1:9000/api/events
접근 로그             tail -f build/demo/front.log
EOF
wait $front
