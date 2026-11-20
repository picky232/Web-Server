# WEEK13 C Web Server

C11 + POSIX socket API만으로 구현한 작은 HTTP/1.1 서버입니다.
완성된 HTTP 서버/이벤트 루프 라이브러리는 사용하지 않았습니다.

- **정적 파일 서빙**: document root 아래 HTML/CSS/JS/PNG/JPG 등 (GET/HEAD)
- **Reverse proxy (동적 요청)**: `/api/*` → 고정 백엔드(들)로 전달, POST 본문 포함, 2개 이상이면 round-robin
- **동시성**: pthread 고정 크기 스레드 풀 + 크기 제한 연결 큐 (mutex + condition variable)
- **안전한 기본 동작**: 요청 크기/시간 제한, path traversal·symlink 탈출 차단, SIGPIPE 무시, SIGINT/SIGTERM graceful shutdown

> 학습 문서: [docs/LEARNING_WALKTHROUGH.md](docs/LEARNING_WALKTHROUGH.md) · 테스트 결과: [docs/TEST_REPORT.md](docs/TEST_REPORT.md)

---

## 1. 빌드와 실행 (WSL2 Ubuntu / Linux)

```bash
make                 # → build/webserver  (gcc, C11, -Wall -Wextra -Wpedantic ...)
make test            # 단위(C) + 통합(Python) + curl 스모크
make clean
```

데모 실행 (터미널 2개):

```bash
# 터미널 1: 테스트용 백엔드 (Python 표준 라이브러리, C 서버의 일부가 아님)
python3 tests/integration/backend.py --port 9000

# 터미널 2: C 웹 서버
./build/webserver --port 8080 --root ./www --threads 4 --backend 127.0.0.1:9000
# (= make run)
```

브라우저에서 `http://localhost:8080/` 을 열면 정적 페이지와 `/api/*` 버튼 데모를 볼 수 있습니다.
종료는 서버 터미널에서 **Ctrl+C (SIGINT)** — 처리 중인 요청을 마치고 `shutdown complete` 를 출력한 뒤 종료 코드 0으로 끝납니다.

### CLI 옵션 (`./build/webserver --help`)

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `-p, --port N` | 8080 | 리스닝 포트 (0 = OS가 빈 포트 선택, 로그에 실제 포트 출력) |
| `--bind ADDR` | 0.0.0.0 | 바인드할 IPv4 주소 |
| `-r, --root DIR` | ./www | 정적 파일 document root |
| `-t, --threads N` | 4 | worker 스레드 수 (1–256) |
| `--queue N` | 64 | worker 를 기다리는 연결 큐 크기 (가득 차면 503) |
| `-b, --backend HOST:PORT` | 없음 | 프록시 백엔드. 반복 지정 시 round-robin (최대 4개) |
| `--proxy-prefix PATH` | /api | 백엔드로 보낼 경로 접두사 |
| `--read-timeout-ms N` | 10000 | 요청 1개(헤더+본문)를 다 받는 제한 시간 → 408 |
| `--keepalive-ms N` | 5000 | keep-alive 연결의 유휴 제한 시간 |
| `--proxy-timeout-ms N` | 5000 | 백엔드 connect/응답 대기 제한 → 504 |
| `--max-body N` | 1048576 | 요청 본문 상한 → 413 |
| `--max-requests N` | 100 | 연결 1개당 최대 요청 수 |
| `-q, --quiet` | - | 요청별 access log 끄기 |

### curl 예시

```bash
curl -i http://localhost:8080/                          # 200 text/html
curl -I http://localhost:8080/style.css                 # HEAD: 헤더만
curl -o logo.png http://localhost:8080/images/logo.png  # 바이너리 그대로
curl -i http://localhost:8080/about                     # 301 → /about/
curl -i http://localhost:8080/nope                      # 404
curl -i --path-as-is http://localhost:8080/../../etc/passwd    # 403
curl -i -X POST -d 'x=1' http://localhost:8080/index.html      # 405 (Allow: GET, HEAD)

curl -i http://localhost:8080/api/hello                 # 프록시 → 백엔드 JSON
curl -i -X POST --data 'hello' http://localhost:8080/api/echo  # 본문이 백엔드까지 전달됨
curl -i http://localhost:8080/api/chunked               # chunked 응답 중계 (Connection: close)
curl -i "http://localhost:8080/api/slow?ms=7000"        # 백엔드가 느림 → 504
```

백엔드를 띄우지 않은 상태에서 `/api/hello` → **502 Bad Gateway**.

---

## 2. 아키텍처

```
 브라우저/curl ──TCP──▶ [listen fd]  main 스레드: accept_loop()  (src/server.c)
                              │ accept() → 연결 fd
                              ▼
                      tp_submit(fd) ──▶ [conn_queue_t 원형 큐, 크기 제한]  (src/thread_pool.c)
                              │            가득 차면 → 503 + close (reject_connection)
                              ▼ cq_pop()
                 worker 1..N : handle_connection(fd)        (src/server.c)
                              │  keep-alive 동안 반복
                              ├─ http_read_request()  바이트 스트림 → 요청 1개   (src/http_conn.c)
                              │     └ http_parse_request_head()  순수 파서      (src/http_parser.c)
                              ├─ dispatch() → router_route()                    (src/router.c)
                              │     ├ static_serve()   파일 → send_all()        (src/static_files.c)
                              │     └ proxy_forward()  백엔드 fd 열고 중계      (src/proxy.c)
                              └─ close(fd)
```

| 파일 | 책임 |
|---|---|
| `src/main.c` | 설정 파싱 후 `server_run()` 호출 |
| `src/config.c` | CLI 파싱 (`config_parse_args`) |
| `src/server.c` | listen/accept 루프, 연결 처리, keep-alive, 시그널, 종료 |
| `src/thread_pool.c` | 크기 제한 큐 + 고정 worker 스레드 |
| `src/net_io.c` | 소켓 생성, deadline 기반 `recv`/`send_all`, 타임아웃 connect, lingering close |
| `src/http_conn.c` | 연결 버퍼에서 요청 1개(head+body) 조립 |
| `src/http_parser.c` | 요청/응답 head 파서 (소켓 없음 → 단위 테스트 대상) |
| `src/http_response.c` | 상태 라인/헤더/오류 응답 작성 |
| `src/static_files.c` | 경로 정규화, MIME, realpath 검사, 파일 전송 |
| `src/router.c` | 접두사 라우팅, 메서드 허용 여부 |
| `src/proxy.c` | reverse proxy (헤더 재작성, 응답 중계, 502/504) |
| `src/log.c` | 스레드 표시가 붙는 한 줄 로그 |

---

## 3. 지원 범위와 비목표

### 지원하는 것
- 요청 라인 `METHOD SP /origin-form SP HTTP/1.0|HTTP/1.1`, CRLF 줄 끝, 헤더 최대 64개, head 최대 8 KiB, URI 최대 2048 B
- 메서드: 정적 = GET, HEAD / 프록시 = GET, HEAD, POST. 표준이지만 미지원(PUT, DELETE…) → **405 + Allow**, 모르는 메서드 → **501**
- 요청 본문: `Content-Length` 만 지원 (최대 `--max-body`), `Expect: 100-continue` 에 `100 Continue` 응답
- 응답: 모든 응답에 상태 라인, `Date`, `Server`, `Content-Type`, `Content-Length`, `Connection`
- keep-alive (HTTP/1.1 기본, HTTP/1.0 은 `Connection: keep-alive` 일 때), 한 번에 도착한 여러 요청(파이프라인)을 **순서대로** 처리
- 상태 코드: 200, 301, 400, 403, 404, 405, 408, 413, 414, 431, 500, 501, 502, 503, 504, 505
- 프록시: Host 교체, hop-by-hop 헤더 제거(`Connection` 에 나열된 헤더 포함), `X-Forwarded-For/Host/Proto` 재설정, 백엔드 응답의 `Content-Length` 기반 스트리밍 중계, 길이 정보가 없거나 chunked 인 응답은 원본 그대로 EOF 까지 중계하고 클라이언트 연결을 닫음

### 하지 않는 것 (명시적 비목표)
- **요청의 `Transfer-Encoding: chunked`** → 501 로 거부 (`Content-Length` 와 함께 오면 request smuggling 위험이 있으므로 400)
- TLS/HTTPS, HTTP/2, IPv6 리스닝, 디렉터리 목록, Range/조건부 요청(If-Modified-Since), 압축, CGI
- 파이프라인 요청의 **병렬** 처리 (순차 처리만), 백엔드 연결 재사용(요청마다 새 연결), 백엔드 장애 시 다른 백엔드로 재시도
- HTTP 표준 "완전 준수" 를 주장하지 않습니다. 위의 범위만 구현·테스트했습니다.

---

## 4. 경로 디코딩과 파일 접근 정책 (정적 파일)

`static_normalize_target()` + `static_serve()` (`src/static_files.c`)

1. `?` 와 `#` 뒤(query/fragment)는 경로에서 제외
2. `%XX` 를 **정확히 한 번** 디코딩. 잘못된 `%` 시퀀스, `%00` 등 제어 문자 → **400**.
   `%252e` 는 `%2e` 라는 글자로 남을 뿐 `.` 이 되지 않음 (이중 인코딩 우회 불가)
3. 디코딩 **후** `/` 로 나눠 정규화: `.` 무시, `//` 축약, `..` 은 한 단계 위. 루트 위로 가려 하면 **403**.
   따라서 `%2e%2e`, `..%2f` 같은 인코딩 변형도 같은 규칙으로 막힘
4. `.` 으로 시작하는 이름(`.git`, `.env`)은 **403**
5. 끝이 `/` 면 `index.html`, `/`없이 디렉터리를 요청하면 **301** `Location: <path>/`
6. `realpath()` 로 심볼릭 링크를 모두 풀고, 결과가 `realpath(document root)` 아래가 아니면 **403** (`path_is_within`)
7. `open()` + `fstat()`: 없음 → 404, 권한 없음 → 403, 일반 파일 아님 → 403, 그 외 → 500
8. 64 KiB 단위 `read()` → `send_all()` 로 바이너리 그대로 전송. 길이는 `fstat` 의 `st_size`

알려진 한계: `realpath()` 검사와 `open()` 사이에 공격자가 docroot 안의 링크를 바꿀 수 있는 TOCTOU 여지는 남아 있습니다
(docroot 에 신뢰하지 않는 사용자가 쓸 수 없다는 전제).

## 5. Reverse proxy 정책
- 백엔드 주소는 **시작 시 CLI 로 고정**되고 `getaddrinfo` 로 한 번만 해석됩니다. 클라이언트가 URL/Host 로 목적지를 바꿀 수 없습니다.
  absolute-form(`GET http://x/ ...`)·`CONNECT` 요청은 400 → 개방형 프록시가 아닙니다.
- `/api/../index.html` 처럼 디코딩·정규화하면 프록시 접두사를 벗어나는 경로는 403.
- 백엔드 연결 실패 → 502, connect/응답 head 대기 시간 초과 → 504, 잘못된/잘린 head → 502.
  응답 본문을 보내기 시작한 뒤의 실패는 상태 코드를 바꿀 수 없으므로 클라이언트 연결을 닫아 "잘린 응답" 임을 알립니다.
- 백엔드 fd 는 `proxy_forward()` 의 `cleanup:` 레이블 한 곳에서 항상 닫힙니다.

---

## 6. 구현 선택과 장단점

| 선택 | 장점 | 단점 / 대안 |
|---|---|---|
| 스레드 풀 + 블로킹 처리 (연결 1개 = worker 1개) | 코드가 위→아래로 읽힘, 디버깅 쉬움 | 느린/유휴 연결이 worker 를 점유 → read deadline, keep-alive 시 대기열 있으면 양보, 큐 가득 차면 503 으로 완화. 대안: epoll 이벤트 루프 |
| 큐 가득 참 → 즉시 503 | accept 루프가 절대 막히지 않음 | 순간 폭주 시 일부 거절. 대안: 블로킹 push (accept 지연) |
| `MSG_DONTWAIT` + `poll` 로 deadline | 어떤 recv/send 도 무한 대기하지 않음 | 코드가 조금 길어짐. 대안: `SO_RCVTIMEO/SO_SNDTIMEO` (총 시간 제한은 어려움) |
| 요청별 총 deadline (`--read-timeout-ms`) | 1바이트씩 보내는 slowloris 도 끊김 | 아주 느린 정상 업로드도 끊김 |
| 본문을 메모리에 다 받은 뒤 전달 | 프록시/파서가 단순 | 큰 업로드는 `--max-body` 로 제한 |
| 프록시: 요청마다 새 백엔드 연결 + `Connection: close` | 상태 관리 불필요, 응답 경계 명확 | 연결 비용. 대안: 백엔드 커넥션 풀 |
| `TCP_NODELAY` | head/body 두 번 send 해도 delayed-ACK 지연 없음 (측정: 173 → 2481 req/s) | 작은 패킷이 늘 수 있음. 대안: `writev` 로 한 번에 |

---

## 7. 테스트

```bash
make unit          # C 단위 테스트 (244 checks)
make integration   # Python 통합 테스트 44개: 실제 서버 + 백엔드 fixture, raw socket
make smoke         # curl 스모크 29개
make test          # 위 세 가지
make test-asan     # ASan+UBSan(+LSan) 빌드로 단위+통합
make test-tsan     # TSan 빌드로 단위+통합
make load          # 동시 클라이언트 부하 (ab/wrk/hey 대체 스크립트)
```

최근 실행 결과 요약 (2026-11-20, WSL2 Ubuntu 26.04, gcc 15.2, 16 vCPU) — 자세한 내용은 [docs/TEST_REPORT.md](docs/TEST_REPORT.md):

| 항목 | 결과 |
|---|---|
| 빌드 경고 | 0 |
| 단위 | 244 / 244 통과 |
| 통합 | 44 / 44 통과 (기본, ASan, TSan 빌드 모두) |
| curl 스모크 | 29 / 29 통과 |
| 부하 (Python 클라이언트, localhost) | 50 clients × 200 req 혼합: 1,871 req/s, 오류 0 |
| ASan/UBSan/LSan, TSan | 보고 0건 |
| valgrind, ab/wrk/hey | **미실행** (설치되어 있지 않음) |

로그: `build/test-logs/` (서버별 로그, 각 테스트 출력)

---

## 8. 제출 데모 (4분) 안내

| 시간 | 할 일 | 보여줄 것 |
|---|---|---|
| 0:00–0:30 | `make && make unit` | 경고 없는 빌드, 244 checks 통과 |
| 0:30–1:15 | 백엔드 + 서버 실행, 브라우저 `http://localhost:8080/` | HTML/CSS/JS/PNG 로딩, 서버 로그의 `[w1]…[w4]` worker 분배 |
| 1:15–2:00 | 페이지의 `GET /api/hello`, `POST /api/echo` 버튼 | 백엔드가 받은 `Host`, `X-Forwarded-For`, 본문 |
| 2:00–2:45 | `curl --path-as-is .../../../etc/passwd` (403), 백엔드 끄고 `/api/hello` (502), `/api/slow?ms=7000` (504) | 안전/오류 처리 |
| 2:45–3:30 | `make integration` 일부 또는 `make load` | 동시성·스트림 테스트 통과, 오류 0 |
| 3:30–4:00 | 서버 터미널 Ctrl+C | `shutting down… shutdown complete`, 종료 코드 0 |

---

## 9. 디렉터리 구조

```
include/   모듈 헤더 (각 파일 맨 위 주석에 역할 설명)
src/       C 구현
tests/unit/test_unit.c           C 단위 테스트
tests/integration/test_server.py Python 통합 테스트 (unittest, 표준 라이브러리만)
tests/integration/backend.py     프록시 테스트용 백엔드 fixture
tests/integration/curl_smoke.sh  curl 스모크
tests/load/stress.py             부하 테스트
www/       데모용 정적 파일
docs/      학습 문서, 테스트 리포트
```
