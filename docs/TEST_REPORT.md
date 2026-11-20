# TEST REPORT

모든 수치는 아래 환경에서 **직접 실행한 결과**입니다. 실행하지 않은 항목은 "미실행" 으로 표시했습니다.

## 1. 환경

| 항목 | 값 |
|---|---|
| 실행 일시 | 2026-11-20 10:36 KST (최종 전체 실행) |
| OS | WSL2, Linux 6.18.58.1-microsoft-standard-WSL2 x86_64, Ubuntu 26.04.1 LTS |
| CPU | 16 vCPU (nproc), 메모리 7.6 GiB |
| 컴파일러 | gcc 15.2.0 (`-std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 ...`) |
| Python | 3.14.4 (테스트 클라이언트/백엔드 fixture, 표준 라이브러리만) |
| curl | 8.18.0 |
| 작업 경로 | Windows 드라이브 위 (`/mnt/c/...`, drvfs). 통합 테스트의 임시 docroot 는 `/tmp` (ext4) |
| 네트워크 | 모두 127.0.0.1 (외부 서버·인터넷·API 키 사용 안 함) |

## 2. 재현 방법

```bash
make clean && make          # 경고 0 확인
make unit                   # C 단위
make integration            # Python 통합 (서버/백엔드 자동 기동, --port 0)
make smoke                  # curl 스모크
make test-asan              # ASan + UBSan + LSan 빌드로 단위+통합
make test-tsan              # TSan 빌드로 단위+통합
python3 tests/load/stress.py --clients 50 --requests 200 --threads 8
python3 tests/load/stress.py --clients 8 --requests 1000 --threads 8
python3 tests/load/stress.py --clients 100 --requests 200 --threads 4 --no-proxy
```

로그 위치 (로컬, `build/` 는 git 에 포함하지 않음):
- `build/test-logs/{unit,integration,smoke,asan-run,tsan-run,load-*}.txt` — 각 명령의 출력
- `build/test-logs/{default,asan,tsan}/<서버이름>.log` — 통합 테스트가 띄운 서버별 stderr 로그
- `build/test-logs/smoke/server.log`, `backend.log`
- `build/test-logs/environment.txt`

## 3. 결과 요약

| 관문 | 명령 | 결과 |
|---|---|---|
| 빌드 | `make` | **경고/오류 0** |
| 1. 단위 | `make unit` | **244 checks 통과 / 0 실패** |
| 2–6. 기능·스트림·동시성·프록시·안전 | `make integration` | **44 tests 통과 / 0 실패** (약 15초) |
| 2. curl 기능 | `make smoke` | **29 checks 통과 / 0 실패** |
| 7. 메모리 (ASan+UBSan+LSan) | `make test-asan` | 단위 244/244, 통합 44/44 통과, 서버 로그 11개에서 sanitizer 보고 **0건**, 모든 서버 SIGINT 후 종료 코드 0 (LSan 누수 시 0이 아님) |
| 7. 스레드 (TSan) | `make test-tsan` | 단위 244/244, 통합 44/44 통과, 서버 로그 11개에서 data race 보고 **0건** |
| 7. 부하 | `tests/load/stress.py` | 아래 표, 모든 실행 **오류 0** |
| 7. valgrind | - | **미실행** (설치되어 있지 않음, sudo 없이 설치 불가) |
| 7. ab / wrk / hey | - | **미실행** (설치되어 있지 않음) → Python 동시 클라이언트로 대체 |

## 4. 단위 테스트 (tests/unit/test_unit.c, 244 checks)

| 그룹 | 내용 |
|---|---|
| `http_find_head_end` | 완성/미완성 head, 본문 앞 경계, bare LF 불인정 |
| 요청 라인 | GET/HEAD/POST/OTHER, 소문자·모르는 메서드 501, 잘못된 문자 400, HTTP/2.0·1.2 → 505, `HTTP/1.1x` 400, 공백 2개, absolute-form·`*` 400, URI 2048 바이트 통과 / 2049 → 414 |
| 헤더 | OWS 제거, 대소문자 무시 조회, Host 없음/중복 400, 이름 뒤 공백 400, 콜론 없음, obs-fold, bare LF, Connection close/keep-alive (1.0/1.1), Expect, 헤더 65개 → 431, 토큰 목록 검사 |
| Content-Length | 0, 일반, ULLONG_MAX, 오버플로, 음수, 공백, 16진, 빈 값, `+5`, 중복 동일(허용)/상이(400), TE chunked 501, TE+CL 400 |
| 응답 head (프록시) | 상태/이유, 이유 없는 상태 라인, TE, HTTP/2 거부, 숫자 아닌 코드, CL 충돌 |
| 경로 정규화 | `.`/`..`/`//`, 쿼리 제거, `%20`, 이중 인코딩 `%252e`, `../`·`%2e%2e`·`%2E%2E%2F`·`..%2f` → 403, 숨김 파일 403, `%00`/`%0a`/`%zz`/`%4`/끝의 `%` → 400 |
| MIME | html/HTML/css/js/png/jpg/JPEG/json, 확장자 없음, 디렉터리 이름의 점, 숨김 파일, `.tar.gz` |
| `path_is_within` | 같은 경로, 하위, 접두사만 같은 형제(`/srv/www2`), 루트 `/` |
| 라우팅 | 접두사 경계(`/api`, `/api/x`, `/api?q`, `/apix`), 백엔드 미설정 시 정적, `%61pi`, 메서드 허용/Allow 값, hop-by-hop 판별 |
| CLI | 정상 옵션 전체, `--help`, 잘못된 포트/스레드/백엔드/접두사/큐, 알 수 없는 옵션, 값 누락 |
| 큐 | 용량, FULL 즉시 반환, FIFO, wraparound, close 후 push/pop, try_pop 정리 |
| 큐 블로킹 | 빈 큐에서 잠든 pop 을 push 가 깨움, close 가 깨움 |
| 스레드 풀 | 200개 작업 모두 처리, worker 2 + 큐 2 포화 시 5번째 FULL, 종료 시 처리 중 2개 완료 + 대기 2개 reject |
| `send_all` | socketpair 4 KiB 버퍼로 2 MiB 부분 전송 후 바이트 일치, 아무도 안 읽으면 IO_TIMEOUT, 닫힌 상대에 EPIPE(-1, SIGPIPE 없이) |
| `http_read_request` | 요청 2개+본문 한 번에 도착(파이프라인), 413, 헤더 중간 정지 → 408, 본문 도중 종료 → READ_ABORTED |

## 5. 통합 테스트 (tests/integration/test_server.py, 44 tests)

**StaticFileTests (22)** — 서버 `--threads 4 --read-timeout-ms 2000 --keepalive-ms 1500 --max-body 65536`, 임시 docroot
- GET `/` → index, Content-Type/Length/Date 확인 · HTML/CSS/JS/PNG/JPG/bin MIME + 바이트 일치 · 4 MiB 무작위 파일 sha256 일치
- `%20` 파일명, 쿼리 무시 · HEAD 후 같은 연결의 다음 GET 이 깨끗하게 파싱됨(본문 누출 없음)
- 404 / 숨김 403 / 권한 없음 403 / `/sub` → 301 `Location: /sub/` (쿼리 보존) / 디렉터리 index / index 없는 디렉터리 404
- POST 정적 405 + `Allow: GET, HEAD`, PUT/DELETE 405, BREW 501
- 잘못된 요청 15종 → 400/501/505, 모두 `Connection: close` + 정확한 Content-Length + 서버가 연결 종료
- 414(긴 URI), 414(8 KiB 넘는 요청 라인), 431(헤더 70개), 431(9 KB 헤더), 413(본문 전송 전에 거부)
- keep-alive 3회 + `Connection: close` 후 종료 · 요청 4개를 한 번의 send 로(파이프라인) → 순서대로 4개 응답
- HTTP/1.0 기본 close, `Connection: keep-alive` 시 유지 · keep-alive 유휴 1.5초 후 서버가 닫음
- **traversal 7종 403** (`../`, `%2e%2e`, `..%2f`, `%2E%2E`, `.%2e`, 하위 디렉터리 경유), `%00`/`%zz` 400, symlink 안쪽 200 / 바깥 파일·디렉터리 403
- **스트림**: 헤더 3조각(0.2초 간격), `\r\n\r` | `\n` 분리, 1바이트씩 전송
- **부분 send**: 수신 버퍼 4 KiB 클라이언트가 4 MiB 를 천천히 읽어도 sha256 일치
- 헤더 도중 정지 → 1.5~5초 사이 408 · 아무것도 안 보내는 연결 → 응답 없이 종료
- 요청/본문 도중 클라이언트 종료 후 서버 정상 · **RST 로 다운로드 20회 중단 후에도 서버 생존 (SIGPIPE 없음)**
- **fd 누수 없음** (`/proc/<pid>/fd` 개수가 400+ 요청 후 원래 값), 스레드 수 고정

**ProxyTests (16)** — 백엔드 fixture A, B (ThreadingHTTPServer), `--proxy-timeout-ms 1000`
- GET `/api/hello` · POST 100 KB 무작위 본문 sha256 일치, `Host` 가 백엔드 주소로, `X-Forwarded-For/Host`, 백엔드가 받은 `Connection: close`
- POST 본문 4조각 분할 · `Expect: 100-continue` → `100 Continue` 후 본문 전달, Expect 헤더는 백엔드로 안 감
- hop-by-hop(`Connection` 에 나열된 `X-Secret`, Keep-Alive, Proxy-Connection, TE, Upgrade) 제거, 위조된 X-Forwarded-For 교체
- 프록시/정적 섞인 keep-alive 4회 · HEAD, 204, 404 그대로 전달 · 3 MB 응답 스트리밍 sha256 일치
- chunked 응답 원본 중계 + `Connection: close` · Content-Length 없는 EOF 응답
- **504**: 백엔드 4초 지연, 제한 1초 → 3초 안에 504 · **502**: 백엔드가 응답 없이 끊음
- 백엔드가 Content-Length 1000 약속 후 10바이트만 보내고 끊음 → 클라이언트도 연결 종료
- `/api/../index.html`·`/api/%2e%2e/...` 403, `/apix` 는 정적(404, X-Backend 없음), absolute-form·CONNECT 400, PUT 405
- **백엔드 미기동 → 502** (1.5초 안), **round-robin**: 6회 중 A 3회, B 3회, 번갈아
- 502/504/정상 요청 60+회 후 fd 개수 원래 값

**ConcurrencyTests (6)**
- 64 스레드 × 10 요청(정적/프록시 혼합) = 640 요청 모두 200, 본문 일치
- slowloris 3개가 worker 4개 중 3개를 점유한 상태에서 정상 요청 20개가 2초 안에 완료
- `--threads 1`: keep-alive 유휴 연결이 있어도 새 클라이언트가 1.5초 안에 응답받음(유휴 연결 양보)
- `--threads 1 --queue 1`: 처리 중 1 + 대기 1 → 세 번째 연결은 **503 + Retry-After: 1**, 이후 회복
- **SIGINT / SIGTERM 각각**: 처리 중이던 프록시 요청(1초 지연)이 200 으로 끝까지 응답되고 그 연결은 닫힘, 유휴 keep-alive 연결 종료, 종료 코드 0, `shutdown complete` 로그, 이후 포트 연결 거부
- `SO_REUSEADDR`: 서버 측 TIME_WAIT 을 만든 뒤 같은 포트로 즉시 재시작 성공

## 6. curl 스모크 (tests/integration/curl_smoke.sh, 29 checks)
HTML/CSS/JS/PNG Content-Type, PNG 바이트 일치(`cmp`), Content-Length = 파일 크기, HEAD, 404, 301, 디렉터리 index,
traversal 403 ×2, POST 정적 405, 400 ×2, 505, 501(chunked 요청), 413, 414, 431, 프록시 GET/POST 에코, 100 KB POST,
504, 502, keep-alive 연결 재사용(`num_connects` 1 → 0), SIGINT 종료 코드 0, 종료 로그.

## 7. 부하 (tests/load/stress.py)

조건: 같은 머신의 Python `http.client` 스레드가 클라이언트(GIL 때문에 클라이언트 쪽이 병목일 가능성이 큼),
keep-alive 사용, 서버 `--quiet`, docroot 는 drvfs 위의 `www/`, 프록시 백엔드는 Python ThreadingHTTPServer.
**벤치마크가 아니라 기능 스트레스 결과로 보아야 합니다.**

| 실행 | 서버 스레드 | 요청 수 | 처리량 | 지연 p50 / p99 / max | 오류 |
|---|---|---|---|---|---|
| 50 clients × 200, 정적 4종 + `/api/hello` | 8 | 10,000 | 1,871 req/s | 25.3 / 43.4 / 70.7 ms | **0** |
| 8 clients × 1000, 혼합 | 8 | 8,000 | 2,481 req/s | 3.0 / 6.4 / 10.1 ms | **0** |
| 100 clients × 200, 정적만 | 4 | 20,000 | 1,756 req/s | 56.6 / 70.5 / 83.8 ms | **0** |
| ASan 빌드, 20 clients × 100, 혼합 | 8 | 2,000 | 1,946 req/s | 9.5 / 19.6 / 26.7 ms | **0** (종료 코드 0 → LSan 누수 없음) |

## 8. 테스트 중 발견해 고친 문제 (기록)

| 발견 | 원인 | 수정 | 수정 후 |
|---|---|---|---|
| 부하 테스트에서 1만 건 중 4~8건 `RemoteDisconnected` | keep-alive 대기 시 "대기열 있음" 을 "다음 요청이 이미 왔는가" 보다 먼저 확인했고, 첫 대기가 너무 짧아 보내는 중인 요청을 끊음 | `wait_for_next_request()` 에서 데이터 확인을 먼저 하고, 양보 전 최소 100ms 대기 | 오류 0 (3회 실행) |
| 클라이언트 8개일 때 요청당 ~47ms, 173 req/s | 응답 head 와 body 를 따로 send → Nagle + delayed ACK | 연결/백엔드 소켓에 `TCP_NODELAY` | 2,481 req/s, p50 3.0ms |
| 프록시 부하에서 max 지연 ~1초 | 테스트 백엔드(Python)의 listen backlog 기본값 5 → SYN 재전송 | fixture 의 `request_queue_size = 128` (서버 코드 아님) | max 70ms |
| 종료 테스트에서 처리 중 응답이 `Connection: keep-alive` | keep-alive 여부는 요청 시작 시 결정되므로 시그널 전에 정해짐 | 동작은 유지(응답 직후 서버가 연결을 닫음), 테스트가 실제 보장(연결 종료)을 검사하도록 수정 | 통과 |

## 9. 실행하지 않았거나 환경상 제한된 항목

- **valgrind, ab, wrk, hey**: 미설치로 미실행. (ASan/LSan/TSan 으로 메모리·스레드 검사는 대신 수행)
- **실제 브라우저 데모**: 이 작업에서 브라우저로 직접 열어 보지는 않았음. 같은 정적 파일과 `/api/*` 요청은 curl/통합 테스트로 확인.
  WSL2 의 localhost 포워딩으로 Windows 브라우저에서 접속되는지는 사용자 환경에서 확인 필요.
- **실제 네트워크(다른 머신, 지연·패킷 손실)**: 모든 테스트는 loopback.
- **fd 고갈(EMFILE) 경로**: 코드에는 처리(50ms 대기 후 재시도)가 있지만 `ulimit -n` 을 낮춰 테스트하지는 않음.
- **IPv6**: 리스닝은 IPv4 만 지원(비목표).
- **매우 큰 파일(> 수 GB)·파일이 전송 중 줄어드는 경우**: 코드 경로는 있으나(연결 종료) 테스트하지 않음.
- 실험 8(SIGPIPE 보호를 빼면 서버가 죽는지)은 예상만 적었고 직접 실행하지 않음.

## 10. 남은 위험

- **스레드 풀 구조의 한계**: worker 수보다 많은 느린 클라이언트가 동시에 오면, read deadline(기본 10초)까지 새 요청이 큐에서 기다리거나 큐가 차면 503.
  epoll 기반 구조가 아니므로 대규모 동시 연결(C10K)에는 적합하지 않음.
- **keep-alive 종료 경쟁**: 서버가 유휴 연결을 닫는 순간 클라이언트가 요청을 보내면 클라이언트는 연결 리셋을 봄(HTTP/1.1 의 일반적 성질, 클라이언트가 재시도해야 함).
  100ms 유예로 빈도를 줄였지만 0 은 아님.
- **TOCTOU**: `realpath()` 검사와 `open()` 사이에 docroot 안 링크가 바뀌는 경우는 막지 못함(docroot 쓰기 권한이 신뢰된다는 전제).
- 프록시는 백엔드 응답 head 를 16 KiB 까지만 허용, 응답 1xx 는 502 처리, HTTP/1.0 클라이언트에게도 백엔드의 chunked 를 그대로 중계(드문 경우).
- 요청 본문은 메모리에 모두 받은 뒤 전달 → `--max-body` 와 스레드 수의 곱만큼 메모리 사용 가능.
