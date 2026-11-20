# WEEK13 C Web Server (jiho)

C11 + POSIX 소켓 API만으로 만든 작은 HTTP/1.1 서버입니다. 완성된 HTTP 서버/이벤트 루프 라이브러리는 쓰지 않았습니다.
요청 하나가 흘러가는 경로를 먼저 끝까지 이은 뒤(고정 응답), 가짜였던 칸을 하나씩 진짜로 바꾸는 탑다운 순서로 만들었습니다.
커밋 `step1` ~ `step5`를 순서대로 보면 그 과정이 그대로 보입니다.

- **정적 파일**: 문서 루트 아래 HTML/CSS/JS/PNG/JPG 등 (GET, HEAD)
- **Reverse Proxy**: `/api/...` → 고정 백엔드로 전달 (POST 본문 포함), 백엔드가 2개 이상이면 Round-Robin
- **동시성**: 고정 개수 작업 스레드 + 크기 제한 연결 큐 (mutex + condition variable), 큐가 가득 차면 503
- **안전한 기본 동작**: 요청 크기·시간 제한, `..`·심볼릭 링크 탈출 차단, SIGPIPE 무시, Ctrl+C 정상 종료

## 1. 빌드와 실행

```bash
make                 # → build/webserver
make test            # 통합(Python) + curl 스모크
make clean
```

macOS에서 Xcode 라이선스 동의 전이라 `cc`가 막히면, 명령마다 Command Line Tools를 지정해서 빌드할 수 있습니다:
`DEVELOPER_DIR=/Library/Developer/CommandLineTools make`

데모 (터미널 2개):

```bash
python3 tests/integration/backend.py --port 9000                                  # 테스트용 백엔드
./build/webserver --port 8080 --root ./www --threads 4 --backend 127.0.0.1:9000   # = make run
```

### 실행 옵션 (`./build/webserver --help`)

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `-p, --port N` | 8080 | 리스닝 포트 (0 = 커널이 빈 포트 선택, 로그에 실제 포트 출력) |
| `--bind ADDR` | 0.0.0.0 | 바인드할 IPv4 주소 |
| `-r, --root DIR` | ./www | 정적 파일 폴더 |
| `-t, --threads N` | 4 | 작업 스레드 수 (1–256) |
| `--queue N` | 64 | 작업 스레드를 기다리는 연결 큐 크기 (가득 차면 503) |
| `-b, --backend HOST:PORT` | 없음 | 프록시 백엔드. 반복하면 Round-Robin (최대 4개) |
| `--proxy-prefix PATH` | /api | 백엔드로 보낼 경로 접두사 |
| `--read-timeout-ms N` | 10000 | 요청 하나(헤더+본문)를 다 받는 제한 시간 → 408 |
| `--keepalive-ms N` | 5000 | keep-alive 연결의 유휴 제한 시간 |
| `--proxy-timeout-ms N` | 5000 | 백엔드 연결·응답 대기 제한 → 504 |
| `--max-body N` | 1048576 | 요청 본문 상한 → 413 |
| `--max-requests N` | 100 | 연결 하나당 최대 요청 수 |
| `-q, --quiet` | - | 요청별 로그 끄기 |

### curl 예시

```bash
curl -i http://localhost:8080/                               # 200 text/html
curl -I http://localhost:8080/style.css                      # HEAD: 헤더만
curl -i http://localhost:8080/about                          # 301 → /about/
curl -i --path-as-is http://localhost:8080/../../etc/passwd  # 403
curl -i -X POST -d 'x=1' http://localhost:8080/index.html    # 405 (Allow: GET, HEAD)
curl -i http://localhost:8080/api/hello                      # 프록시 → 백엔드 JSON
curl -i -X POST --data 'hello' http://localhost:8080/api/echo
curl -i "http://localhost:8080/api/slow?ms=7000"             # 백엔드가 느림 → 504
```

## 2. 요청 하나의 흐름

```
 클라이언트 ──TCP──▶ [커널: listen 소켓 → accept 큐]
                              │ accept()                        main.c
                              ▼
               메인 스레드: 연결 큐에 넣기 (가득 차면 503)      threadpool.c
                              │
                              ▼
               작업 스레드: 연결 하나를 keep-alive 끝까지 처리   conn.c
                  ├ 빈 줄까지 읽기 → 요청 줄·헤더 해석           http.c
                  ├ 경로 디코딩·검사 → 라우팅                    http.c, router.c
                  │    ├ /api/... → 백엔드로 중계                proxy.c
                  │    └ 그 외    → 파일 보내기                  static.c, mime.c
                  └ 응답 쓰기 → keep-alive면 다음 요청, 아니면 닫기  response.c, net.c
```

| 파일 | 역할 |
|---|---|
| `src/main.c` | 옵션 읽기, 시그널 등록, accept 루프, 정상 종료 |
| `src/config.c` | 실행 옵션 파싱 |
| `src/net.c` | listen 소켓, 제한 시간 있는 대기·connect, 전부 쓰기, lingering close |
| `src/threadpool.c` | 고정 작업 스레드 + 원형 연결 큐 |
| `src/conn.c` | 연결 하나: 요청 조립, 본문 읽기·100-continue, keep-alive, 타임아웃 |
| `src/http.c` | 요청 파서와 경로 디코딩 (소켓을 모르는 순수 함수) |
| `src/response.c` | 상태 줄·공통 헤더·오류 페이지 |
| `src/static.c` | 경로 → 파일, realpath 검사, 파일 전송 |
| `src/mime.c` | 확장자 → Content-Type |
| `src/router.c` | 프록시 접두사 판정, Round-Robin |
| `src/proxy.c` | 요청 다시 쓰기, 응답 중계, 502·504 |

## 3. 지원 범위

- 요청 줄 `METHOD SP /경로 SP HTTP/1.0|HTTP/1.1`, 헤더 최대 64개, 헤더 블록 최대 8 KiB, URI 최대 2048 B
- 메서드: 정적 = GET, HEAD / 프록시 = GET, HEAD, POST. 다른 표준 메서드 → 405 + Allow, 모르는 메서드 → 501
- 본문은 `Content-Length`만 지원. `Transfer-Encoding: chunked` 요청은 501, `Content-Length`와 같이 오면 400
- keep-alive: HTTP/1.1 기본, HTTP/1.0은 `Connection: keep-alive`일 때. 한 번에 온 여러 요청은 순서대로 처리
- 상태 코드: 200, 301, 400, 403, 404, 405, 408, 413, 414, 431, 501, 502, 503, 504, 505
- 하지 않는 것: HTTPS, HTTP/2, IPv6, 디렉터리 목록, Range·조건부 요청, 압축, CGI, 백엔드 연결 재사용

### 경로 검사 (정적 파일)
1. `?` 뒤는 경로에서 뺍니다. `%XX`를 **한 번만** 풀고, 잘못된 `%`나 `%00` 같은 제어 문자는 400
2. 푼 뒤의 경로에서 `.`으로 시작하는 조각(`.`, `..`, `.git`, `.env`)은 전부 403. 그래서 `%2e%2e`, `..%2f`도 같은 규칙에 걸립니다
3. `realpath()`로 심볼릭 링크까지 푼 결과가 문서 루트 밖이면 403
4. 없음 → 404, 권한 없음 → 403, 폴더인데 `/`로 안 끝남 → 301

### Reverse Proxy
- 백엔드 주소는 실행 옵션으로 고정합니다. `GET http://x/...`이나 `CONNECT` 같은 요청은 400이라 남의 서버로 가는 통로(열린 프록시)가 되지 않습니다
- 백엔드로 보낼 때: Host를 백엔드 주소로 바꾸고, hop-by-hop 헤더(`Connection`과 거기 적힌 이름 포함)와 클라이언트가 보낸 `X-Forwarded-*`를 지운 뒤 새로 씁니다. 백엔드 연결은 요청마다 새로 열고 `Connection: close`를 보냅니다
- 응답: `Content-Length`가 있으면 그만큼만 중계하고 keep-alive를 유지합니다. 길이가 없거나 chunked면 백엔드가 닫을 때까지 그대로 중계하고 클라이언트 연결도 닫습니다
- 연결 거절·응답 없이 끊김 → 502, 응답 헤더가 제한 시간 안에 안 옴 → 504. 본문을 보내기 시작한 뒤의 실패는 상태 코드를 바꿀 수 없으니 연결을 닫아 알립니다

## 4. 구현 선택

| 선택 | 이유 | 대가 |
|---|---|---|
| 스레드 풀 + blocking 소켓 (연결 1개 = 작업 스레드 1개) | 코드가 위에서 아래로 읽힘 | 놀고 있는 연결이 스레드를 잡는다 → 큐에 대기자가 있으면 응답에 `Connection: close`를 싣고, 0.1초 이상 유휴인 연결은 양보 |
| 큐가 가득 차면 즉시 503 | accept 루프가 멈추지 않음 | 순간 폭주 때 일부 거절 |
| 모든 대기를 `poll` + 0.1초 간격으로 | 무한 대기 없음, Ctrl+C를 바로 알아챔 | 코드가 조금 길어짐 |
| 본문은 메모리에 모으지 않고 받는 대로 백엔드로 | 큰 본문도 버퍼 16 KiB로 처리 | 중간 실패 처리가 조금 복잡 |
| `TCP_NODELAY` | 헤더·본문을 따로 보내도 지연 ACK 대기가 없음 | 작은 패킷이 늘 수 있음 |

## 5. 테스트

```bash
make integration   # Python 통합 테스트 44개: 실제 서버 + 백엔드 fixture, raw socket
make smoke         # curl 스모크 29개 (Linux용: stat -c 사용)
make test-asan     # ASan+UBSan(+LSan) 빌드로 통합 테스트 (LSan은 Linux만. macOS는 ASAN_OPTIONS=detect_leaks=0으로 직접 실행)
make test-tsan     # TSan 빌드로 통합 테스트
make load          # 동시 클라이언트 부하
```

통합·스모크·부하 테스트는 팀원 구현 때 만든 것을 그대로 썼습니다. 서버 안을 보지 않고 바깥에서 요청·응답만 확인하는 테스트라 구현이 바뀌어도 그대로 돌아갑니다.
단위 테스트는 팀원 구현의 내부 함수를 직접 부르던 것이라 이 브랜치에서는 뺐습니다 (`main` 브랜치에 그대로 있음).

최근 실행 결과 (2026-11-20, macOS, Apple clang):

| 항목 | 결과 |
|---|---|
| 빌드 경고 | 0 |
| 통합 | 42 / 44 통과. 남은 2개(`test_no_fd_leak_*`)는 Linux `/proc`을 읽는 테스트라 macOS에서 실행 불가 → 같은 요청을 보내고 `lsof`로 셌을 때 fd 7 → 7, 스레드 5 → 5 |
| 통합 (ASan+UBSan 빌드, detect_leaks=0) | 42 / 44 통과 (같은 2개 제외), 보고 0건 |
| 통합 (TSan 빌드) | 42 / 44 통과 (같은 2개 제외), TSan 경고 0 |
| curl 스모크 | 28 / 29 통과. 남은 1개는 GNU `stat -c`를 써서 macOS에서 실패 (파일 크기 1388 = 받은 크기 1388로 직접 확인) |
| 부하 50 clients × 200 req (정적+프록시 혼합, 8 스레드) | 약 7,600 req/s, 오류 0 |
| 부하 100 clients × 200 req (정적만, 4 스레드) | 약 10,000 req/s, 오류 0 |
| Linux(WSL2)에서 실행, LeakSanitizer | 미검증 |

## 6. 디렉터리 구조

```
include/   모듈 헤더
src/       C 구현
tests/integration/test_server.py  통합 테스트 (unittest, 표준 라이브러리만)
tests/integration/backend.py      프록시 테스트용 백엔드
tests/integration/curl_smoke.sh   curl 스모크
tests/load/stress.py              부하 테스트
www/       데모용 정적 파일
```
