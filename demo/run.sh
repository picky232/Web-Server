#!/bin/sh
# make demo && demo/run.sh   전부 켜기 (끄기: Ctrl+C)
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/demo

# Ctrl+C는 켜 둔 프로그램에도 바로 간다. 여기서 바로 또 보내면 두 번째 신호라 처리 중인 예매까지 끊긴다.
# 3초 뒤에도 남은 것에만 보낸다: 막 켜져서(0.1초 안) 첫 신호를 놓친 것이 있을 수 있다
trap 'trap - EXIT; (sleep 3; kill $front $one $two $crowd) 2>/dev/null & wait $front $one $two $crowd; kill $!; exit 130' INT
trap 'trap - EXIT; kill $front $one $two $crowd 2>/dev/null; wait; exit' TERM HUP EXIT
build/webserver -s -c demo/front.conf 2>build/demo/front.log & # -s: 감독 프로세스. 켠 채로 설정을 다시 읽을 수 있다
front=$!
build/seats -c demo/backend-9001.conf 2>build/demo/9001.log &
one=$!
build/seats -c demo/backend-9002.conf 2>build/demo/9002.log &
two=$!
FRONT_PID=$front build/crowd -c demo/crowd.conf 2>build/demo/crowd.log &
crowd=$!
sleep 1
for pid in $front $one $two $crowd; do
    if ! kill -0 "$pid" 2>/dev/null; then
        cat build/demo/front.log build/demo/9001.log build/demo/9002.log build/demo/crowd.log
        echo "켜지 못했습니다. 남아 있는 데모가 있으면 먼저 꺼 주세요: pkill -f demo/backend-; pkill -f demo/front.conf; pkill -f demo/crowd.conf"
        exit 1
    fi
done

# 켤 때마다 빈 좌석에서 시작. 파일을 지우면 이미 열어 둔 백엔드는 옛 파일을 계속 보므로, 내용만 0으로 덮는다
dd if=/dev/zero of=demo/seats.dat bs=$(($(wc -c <demo/seats.dat))) count=1 conv=notrunc 2>/dev/null

cat <<EOF
데모 화면             http://localhost:9000
먼저 보내는 것 보기   curl -N http://127.0.0.1:9000/api/events
접근 로그             tail -f build/demo/front.log
EOF

# 백엔드가 죽으면 4초 뒤 다시 켠다. 그 4초 동안 화면에서 "꺼짐"과 장애 넘기기를 볼 수 있다
down_one=0
down_two=0
while kill -0 $front 2>/dev/null; do
    sleep 1
    if kill -0 $one 2>/dev/null; then down_one=0; else down_one=$((down_one + 1)); fi
    if kill -0 $two 2>/dev/null; then down_two=0; else down_two=$((down_two + 1)); fi
    if [ $down_one -ge 4 ]; then
        build/seats -c demo/backend-9001.conf 2>>build/demo/9001.log &
        one=$!
    fi
    if [ $down_two -ge 4 ]; then
        build/seats -c demo/backend-9002.conf 2>>build/demo/9002.log &
        two=$!
    fi
done
