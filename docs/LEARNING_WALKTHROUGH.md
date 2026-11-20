# LEARNING WALKTHROUGH — 요청 하나가 서버를 통과하는 길 (Top-down)

이 문서는 "브라우저가 `http://localhost:8080/style.css` 를 요청했을 때 무슨 일이 일어나는가" 를
**실제 함수와 파일:줄 번호**로 따라가는 지도입니다. 줄 번호는 이 문서를 커밋한 시점 기준이며,
코드를 고치면 조금씩 달라질 수 있습니다. 함수 이름은 모두 실제 코드에 있는 이름입니다.

> **가장 먼저 볼 함수: [`server_run()` — src/server.c:249](../src/server.c#L249)**
> 서버의 일생(설정 확인 → 소켓 준비 → 스레드 풀 시작 → accept 루프 → 종료 정리)이
> 이 함수 하나에 위에서 아래로 순서대로 적혀 있습니다. 여기서 호출하는 함수를 하나씩 따라 내려가면
> 이 문서의 나머지 순서와 그대로 일치합니다.

---

## 0. 전체 지도

```
[브라우저] --TCP 연결--> [커널: listen 소켓의 accept 대기열]
                                │
  main 스레드  server_run() ─▶ accept_loop() ─ accept() ─▶ 연결 fd
                                │                          │ tp_submit(fd)
                                │                          ▼
                                │                 conn_queue_t (원형 큐, mutex+cond)
                                │                          │ cq_pop()
  worker 스레드 worker_main() ─▶ handle_connection(fd) ◀───┘
                                 │  ┌─ (keep-alive 반복) ─────────────────────────┐
                                 │  │ http_read_request()  recv → 버퍼 → head/body │
                                 │  │   └ http_parse_request_head()               │
                                 │  │ dispatch() → router_route()                 │
                                 │  │   ├ static_serve()  → open/read → send_all  │
                                 │  │   └ proxy_forward() → connect → send/recv   │
                                 │  │ http_conn_consume()  남은 바이트 앞으로     │
                                 │  └─────────────────────────────────────────────┘
                                 └ close(fd)
```

| 단계 | 함수 | 위치 |
|---|---|---|
| 진입 | `main()` | [src/main.c:9](../src/main.c#L9) |
| CLI 파싱 | `config_parse_args()` | [src/config.c:81](../src/config.c#L81) |
| 서버 일생 | `server_run()` | [src/server.c:249](../src/server.c#L249) |
| 리스닝 소켓 | `create_listen_socket()` | [src/net_io.c:29](../src/net_io.c#L29) |
| 스레드 풀 시작 | `start_pool_with_signals_blocked()` → `tp_init()` | [src/server.c:196](../src/server.c#L196), [src/thread_pool.c:144](../src/thread_pool.c#L144) |
| 연결 수락 | `accept_loop()` | [src/server.c:211](../src/server.c#L211) |
| 큐 넣기/빼기 | `cq_push()` / `cq_pop()` | [src/thread_pool.c:55](../src/thread_pool.c#L55), [:74](../src/thread_pool.c#L74) |
| worker 루프 | `worker_main()` | [src/thread_pool.c:131](../src/thread_pool.c#L131) |
| 연결 처리 | `handle_connection()` | [src/server.c:118](../src/server.c#L118) |
| 요청 수신 | `http_read_request()` | [src/http_conn.c:68](../src/http_conn.c#L68) |
| head 파싱 | `http_parse_request_head()` | [src/http_parser.c:171](../src/http_parser.c#L171) |
| 라우팅 | `dispatch()` → `router_route()` | [src/server.c:95](../src/server.c#L95), [src/router.c:21](../src/router.c#L21) |
| 정적 파일 | `static_serve()` | [src/static_files.c:192](../src/static_files.c#L192) |
| 프록시 | `proxy_forward()` | [src/proxy.c:173](../src/proxy.c#L173) |
| 전송 | `send_all()` | [src/net_io.c:111](../src/net_io.c#L111) |
| 종료 | `on_stop_signal()` → `tp_shutdown()` | [src/server.c:50](../src/server.c#L50), [src/thread_pool.c:195](../src/thread_pool.c#L195) |

---

## 1. 브라우저 → TCP

브라우저는 URL 에서 호스트(`localhost`)와 포트(`8080`)를 꺼내 **TCP 연결**(3-way handshake)을 만든 뒤,
그 연결 위로 아래와 같은 **텍스트 바이트**를 씁니다.

```
GET /style.css HTTP/1.1\r\n
Host: localhost:8080\r\n
Connection: keep-alive\r\n
\r\n
```

TCP 는 이 바이트들을 순서대로, 빠짐없이 전달해 주지만 **"여기까지가 요청 1개"라는 경계는 주지 않습니다.**
경계("\r\n\r\n" 과 Content-Length)를 찾는 것은 전적으로 서버 코드의 일입니다(→ 5장).

## 2. 리스닝 소켓 만들기 — `create_listen_socket()` (src/net_io.c:29)

`server_run()` 이 [src/server.c:281](../src/server.c#L281) 에서 호출합니다.

| 줄 | 호출 | 의미 |
|---|---|---|
| [net_io.c:33](../src/net_io.c#L33) | `socket(AF_INET, SOCK_STREAM, 0)` | IPv4 TCP 소켓 생성 → **fd(정수 번호)** 를 받음 |
| [net_io.c:42](../src/net_io.c#L42) | `setsockopt(SO_REUSEADDR)` | 재시작 시 TIME_WAIT 때문에 bind 가 실패하지 않게 |
| [net_io.c:58](../src/net_io.c#L58) | `bind()` | "이 IP:포트는 내 것" 등록. 이미 쓰는 포트면 `EADDRINUSE` 메시지 |
| [net_io.c:68](../src/net_io.c#L68) | `listen(fd, SOMAXCONN)` | 연결을 **받기만 하는** 리스닝 소켓으로 전환 |

실패하면 각 단계가 `strerror(errno)` 를 담은 메시지를 `err` 에 쓰고 fd 를 닫은 뒤 -1 을 돌려줍니다.

### fd(file descriptor) 의 역할 정리

서버를 실행한 상태에서 `ls -l /proc/$(pgrep webserver)/fd` 를 보면 아래 종류의 fd 가 보입니다.

| fd 종류 | 만드는 곳 | 누가 닫나 |
|---|---|---|
| 0,1,2 | 프로세스 기본 (stdin/stdout/stderr) | - |
| **리스닝 fd** (보통 3) | `create_listen_socket()` | `server_run()` 종료부 [server.c:308](../src/server.c#L308) |
| **연결 fd** (클라이언트 1명당 1개) | `accept()` [server.c:225](../src/server.c#L225) | `handle_connection()` 끝 [server.c:171](../src/server.c#L171) 또는 `close_gracefully()`, 큐에 못 들어가면 `reject_connection()` |
| **파일 fd** (`ffd`) | `open()` in `static_serve()` | 같은 함수의 모든 반환 경로 |
| **백엔드 fd** (`bfd`) | `connect_with_timeout()` [proxy.c:194](../src/proxy.c#L194) | `proxy_forward()` 의 `cleanup:` [proxy.c:318](../src/proxy.c#L318) 한 곳 |

리스닝 fd 는 "문지기"이고, `accept()` 가 돌려주는 연결 fd 가 "실제 대화 상대"입니다.
같은 포트 번호를 쓰지만 서로 다른 소켓입니다.

## 3. accept 루프 — `accept_loop()` (src/server.c:211)

```c
int r = poll(&p, 1, 500);                 // server.c:215  새 연결이 있거나 0.5초 지나면 깨어남
int cfd = accept(listen_fd, NULL, NULL);  // server.c:225  커널 대기열에서 연결 하나를 꺼냄 → 연결 fd
cq_status_t qs = tp_submit(&srv->pool, cfd); // server.c:240 큐에 넣고 바로 다음 accept 로
```

- main 스레드는 **요청을 한 바이트도 읽지 않습니다.** 그래서 느린 클라이언트가 accept 루프를 막을 수 없습니다.
- 큐가 가득 차면(`CQ_FULL`) `reject_connection()` 이 `503 Service Unavailable` + `Retry-After: 1` 을
  최대 50ms 동안 시도하고 닫습니다.
- `accept()` 가 `EMFILE`(fd 고갈)을 내면 50ms 쉬고 다시 시도합니다(바쁜 루프 방지).
- `poll` 에 500ms 타임아웃을 주는 이유: SIGINT 가 "g_stop 확인 직후, poll 진입 직전" 에 와도 최대 0.5초 안에 종료를 알아챕니다.

## 4. pthread 가 요청을 배분하는 방법 — src/thread_pool.c

```
accept_loop (생산자 1)                       worker 1..N (소비자)
  cq_push(fd):                                cq_pop():
    lock(mu)                                    lock(mu)
    가득 차면 → unlock, CQ_FULL                 while (count==0 && !closed)
    items[tail] = fd; count++                       cond_wait(not_empty, mu)  ← 잠들며 mu 를 놓음
    cond_signal(not_empty)  ──── 깨움 ───────▶  closed 면 → unlock, -1 (worker 종료)
    unlock(mu)                                  fd = items[head]; head++; count--
                                                unlock(mu)
                                                handler(fd) = handle_connection(fd)
```

- 공유 상태는 `conn_queue_t` 하나뿐이고 모든 필드는 `q->mu` 를 잡은 상태에서만 바뀝니다
  ([thread_pool.c:55–90](../src/thread_pool.c#L55)).
- `pthread_cond_wait` 를 `while` 로 감싸는 이유: spurious wakeup, 또는 다른 worker 가 먼저 가져간 경우 대비.
- 어느 worker 가 깨어날지는 OS 가 정합니다. 로그의 `[w1]`, `[w3]` 표시(`tp_worker_id()`)로 실제 분배를 볼 수 있습니다.
- 연결별 데이터(`http_conn_t conn`, 수신 버퍼 8 KiB)는 **worker 의 스택**에 있으므로 다른 스레드와 공유되지 않습니다.
  그 밖에 여러 스레드가 함께 쓰는 것은 `g_stop`(atomic), 프록시의 round-robin 커서 `p->next`(atomic),
  읽기 전용 설정(`server_config_t`)뿐입니다.

### 느린 클라이언트와 worker 점유
연결 1개를 worker 1개가 끝까지 처리하므로, 유휴/느린 연결이 worker 를 붙잡을 수 있습니다. 완화 장치:
1. `--read-timeout-ms`: 요청 1개 전체의 **총 마감 시각** — 1바이트씩 흘려보내도 결국 408
2. `wait_for_next_request()` [server.c:72](../src/server.c#L72): keep-alive 대기 중 큐에 다른 연결이 기다리면 이 유휴 연결을 닫고 worker 를 양보
3. keep-alive 결정 [server.c:156](../src/server.c#L156): 대기열이 있으면 응답에 `Connection: close`

## 5. recv 와 버퍼 — `http_read_request()` (src/http_conn.c:68)

연결 버퍼 `http_conn_t.buf[8192]` 에 바이트를 이어 붙이다가 `"\r\n\r\n"` 이 보이면 head 가 완성된 것입니다.

| 줄 | 하는 일 |
|---|---|
| [http_conn.c:77](../src/http_conn.c#L77) | 이미 버퍼에 head 가 있는지 확인 (이전 recv 가 2개 요청을 한꺼번에 줬을 수도) |
| [http_conn.c:91](../src/http_conn.c#L91) | `recv_until_deadline(c->fd, c->buf + c->len, ...)` — **빈 공간 뒤에 이어서** 받기 |
| [http_conn.c:105](../src/http_conn.c#L105) | 받을 때마다 다시 `http_find_head_end()` |
| [http_conn.c:109](../src/http_conn.c#L109) | head 완성 → `http_parse_request_head()` |
| [http_conn.c:125](../src/http_conn.c#L125) | `Content-Length` 만큼 `malloc` → 버퍼에 이미 온 본문 앞부분 복사 |
| [http_conn.c:146](../src/http_conn.c#L146) | 모자란 본문을 `recv_until_deadline` 으로 채움 |

### 요청이 3조각으로 쪼개져 올 때 버퍼 변화

요청: `GET /style.css HTTP/1.1\r\nHost: test.local\r\n\r\n` (45바이트)를 클라이언트가 7 / 18 / 20 바이트로
0.2초 간격을 두고 나눠 보냄 (통합 테스트 `test_header_split_into_three_sends` 가 실제로 이렇게 보냅니다).

```
recv #1 (7B)   buf = "GET /st"                                            len=7   head_end=0 → 계속 recv
recv #2 (18B)  buf = "GET /style.css HTTP/1.1\r\n"                         len=25  head_end=0 → 계속 recv
recv #3 (20B)  buf = "GET /style.css HTTP/1.1\r\nHost: test.local\r\n\r\n" len=45  head_end=45 → 파싱!
```

`"\r\n\r"` 까지만 오고 `"\n"` 이 다음 recv 로 오는 경우도(`test_split_inside_terminator_and_byte_by_byte`)
`http_find_head_end()` 가 **매번 버퍼 전체를 다시 훑기** 때문에 문제없이 처리됩니다.

### 요청 2개가 한 번에 올 때 (파이프라이닝)

```
recv #1        buf = [요청A head][요청A body 5B][요청B head]      len=전체
처리 A 후      http_conn_consume(): consumed(=A head + A body) 만큼 지우고 memmove
               buf = [요청B head]                                 → 다음 루프에서 recv 없이 바로 B 처리
```

`handle_connection()` 은 `conn.len > 0` 이면 keep-alive 대기를 건너뜁니다([server.c:130](../src/server.c#L130)).

### 파싱 — `http_parse_request_head()` (src/http_parser.c:171)
버퍼를 **제자리에서** 고칩니다: 각 `"\r\n"` 의 `\r` 자리와 `:` 자리에 `'\0'` 을 넣고,
`req->method_str`, `req->target`, `req->headers[i].name/value` 는 그 버퍼 안을 가리킵니다.
그래서 `http_conn_consume()` 은 요청 처리가 **끝난 뒤**에만 호출해야 합니다.
반환값 0 이 아니면 그 숫자가 곧 보낼 오류 상태 코드(400/414/431/501/505)입니다.

## 6. 라우팅 — `dispatch()` (src/server.c:95)

1. `router_route()`: 백엔드가 설정되어 있고 target 이 `/api`, `/api/...`, `/api?...` 이면 프록시, 아니면 정적
2. `router_method_allowed()`: 정적은 GET/HEAD, 프록시는 GET/HEAD/POST. 아니면 405 + `Allow` 헤더

## 7a. 정적 파일 — `static_serve()` (src/static_files.c:192)

```
static_normalize_target()  "/a/../style.css?v=1" → "/style.css"  (디코딩, ., .., 숨김파일)  → 400/403
snprintf(root_real + rel [+ index.html])
realpath()                 static_files.c:213  심볼릭 링크까지 풀어낸 실제 경로          → 404/403
path_is_within()           static_files.c:216  여전히 root 아래인가?                      → 403
open() + fstat()           디렉터리면 301, 일반 파일 아니면 403
http_send_head(200, mime_type_for_path(), st_size)
loop: read(ffd, 64KiB) → send_all(fd, chunk)   static_files.c:261, 270
close(ffd)
```

## 7b. 프록시 — `proxy_forward()` (src/proxy.c:173)

```
static_normalize_target(allow_hidden) + router_prefix_matches  "/api/../x" → 403
atomic_fetch_add(&p->next) % count          round-robin 백엔드 선택
connect_with_timeout()                      proxy.c:194   거부 → 502, 시간 초과 → 504
send_backend_request()                      Host 교체, hop-by-hop 제거, X-Forwarded-*, Content-Length, Connection: close
read_backend_head()                         "\r\n\r\n" 까지 수신, 시간 초과 → 504, 끊김 → 502
http_parse_response_head()                  상태 코드/Content-Length/Transfer-Encoding
클라이언트용 head 재작성 → send_all
본문 중계: Content-Length 만큼 (BODY_LENGTH) / EOF 까지 (BODY_UNTIL_EOF, 클라이언트 연결도 닫음)
cleanup: close(bfd)                         proxy.c:318
```

## 8. send — `send_all()` (src/net_io.c:111)

```c
ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);   // net_io.c:117
```
- `send()` 는 **요청한 길이보다 적게** 보낼 수 있습니다(커널 송신 버퍼가 찼을 때). 그래서 `p += n; len -= n` 을 반복.
- `EAGAIN` 이면 `wait_fd(POLLOUT)` 로 공간이 생길 때까지(마감 시각까지) 기다립니다.
- `MSG_NOSIGNAL`: 클라이언트가 떠난 소켓에 써도 SIGPIPE 로 프로세스가 죽지 않고 `EPIPE` 를 받습니다.
  (`install_signal_handlers()` 에서 SIGPIPE 를 무시하도록 설정한 것과 이중 안전장치)
- 이 루프를 실제로 검증하는 테스트: 단위 `test_send_all_partial` (socketpair 4 KiB 버퍼로 2 MiB 전송),
  통합 `test_slow_reader_gets_complete_file` (수신 버퍼 4 KiB 클라이언트가 4 MiB 를 천천히 읽음).

## 9. 오류가 나면 어떤 자원을 정리하나

| 상황 | 응답 | 정리 |
|---|---|---|
| 요청 파싱 오류/크기 초과/408 | 오류 응답 + `Connection: close` | `http_request_free()` (본문 malloc 해제) → `close_gracefully()`: `shutdown(SHUT_WR)` 후 남은 입력을 잠깐 버리고 close (바로 close 하면 RST 로 응답이 사라질 수 있음) |
| 요청 도중 클라이언트 종료 | 없음 | 본문 해제, `close(fd)` |
| 응답 전송 중 클라이언트 종료 (EPIPE) | - | `info.keep_open=false` → 루프 탈출 → `close(fd)`; 정적이면 `close(ffd)` |
| 백엔드 연결 실패/시간 초과 | 502/504 | `bfd` 는 열린 적 없음/이미 닫힘, `cleanup:` 통과 |
| 백엔드 응답 중간 끊김 | (이미 200 전송) | 클라이언트 연결 닫기 + `close(bfd)` |
| 큐 가득 참 | 503 | `reject_connection()` 이 즉시 close |
| SIGINT/SIGTERM | 처리 중 요청은 끝까지 | `accept_loop` 탈출 → `close(listen_fd)` → `tp_shutdown()`: 큐 닫기 + 모든 worker join + 대기 중 fd 에 503 후 close + 큐/스레드 배열 free |

검증: 통합 테스트 `test_no_fd_leak_and_fixed_threads`, `test_no_fd_leak_after_proxy_errors` 는
`/proc/<pid>/fd` 개수가 수백 건의 정상/오류 요청 후 원래 값으로 돌아오는지 확인합니다.
메모리는 ASan+LSan 빌드(`make test-asan`)에서 종료 시 누수 보고 0건이었습니다.

## 10. 시그널과 종료 순서

1. `on_stop_signal()` [server.c:50](../src/server.c#L50) 은 `atomic_store(&g_stop, true)` **만** 합니다 (async-signal-safe).
2. worker 를 만들 때 SIGINT/SIGTERM 을 막아 두었으므로(`start_pool_with_signals_blocked()`), 시그널은 main 스레드로만 와서
   `poll()` 을 `EINTR` 로 깨웁니다 (`SA_RESTART` 를 쓰지 않은 이유).
3. `accept_loop()` 가 끝나고, worker 들은 keep-alive 대기 중이면 200ms 안에 `g_stop` 을 보고 연결을 닫습니다.
   처리 중인 요청은 끝까지 응답합니다(`test_graceful_shutdown_finishes_in_flight_request`).

---

## 11. 내일 직접 해 볼 실험 (8개)

각 실험은 바꾼 뒤 `make && make test` 또는 표시한 명령으로 결과를 확인하고, 끝나면 `git checkout -- <파일>` 로 되돌리세요.

1. **TCP 스트림 눈으로 보기** — `http_read_request()` 의 [http_conn.c:91](../src/http_conn.c#L91) recv 다음 줄에
   `log_info("recv %zd bytes, buffered=%zu", n, c->len + n);` 를 넣고 아래로 요청을 3조각 보내 보세요.
   ```bash
   python3 -c "import socket,time; s=socket.create_connection(('127.0.0.1',8080))
   for p in [b'GET /st', b'yle.css HTTP/1.1\r\nH', b'ost: a\r\n\r\n']: s.sendall(p); time.sleep(0.5)
   print(s.recv(200))"
   ```
2. **스레드 1개의 한계** — `./build/webserver --threads 1 --read-timeout-ms 10000` 으로 띄우고
   터미널 A 에서 `nc localhost 8080` 후 `GET / HTTP/1.1` 만 입력(엔터 1번). 터미널 B 의 `curl localhost:8080/` 가
   언제 응답하는지(408 이후) 관찰. `--threads 4` 와 비교.
3. **큐 포화 503** — `--threads 1 --queue 1` 로 띄우고 `nc` 3개를 연달아 열어 세 번째가 즉시
   `503 Service Unavailable` 을 받는지 확인 (통합 테스트 `test_queue_full_returns_503` 과 같은 상황).
4. **Nagle 알고리즘** — [server.c:126](../src/server.c#L126) 의 `set_tcp_nodelay(fd);` 를 주석 처리하고
   `python3 tests/load/stress.py --clients 8 --requests 1000` 실행. 이번 작업 중 실제 측정값:
   제거 전 173 req/s(요청당 ~47ms) → 추가 후 2,481 req/s. 왜 ~40ms 인지 "delayed ACK" 로 찾아보세요.
5. **keep-alive 양보 타이밍** — `wait_for_next_request()` 의 `slice_ms = 100` 을 `1` 로 바꾸고
   `python3 tests/load/stress.py --clients 50 --requests 200` 실행. 작업 중 실제로 `RemoteDisconnected` 오류가
   1만 건 중 몇 건씩 나왔던 경쟁 상태(race)를 재현해 보세요.
6. **두 번째 방어선 끄기** — [static_files.c:216](../src/static_files.c#L216) 의 `path_is_within` 검사를 주석 처리하고
   `make integration` → `test_traversal_is_blocked` 의 symlink(`/link_out.txt`) 검사가 실패해야 합니다.
   문자열 정규화(1겹)만으로는 심볼릭 링크 탈출을 못 막는다는 것을 확인.
7. **헤더 버퍼 크기** — `include/http.h` 의 `HTTP_MAX_HEAD_BYTES` 를 `128` 로 줄이고 `curl -v localhost:8080/` →
   curl 의 평범한 요청도 431 이 되는지, `make unit` 에서 어떤 검사가 깨지는지 확인.
8. **SIGPIPE** — `install_signal_handlers()` 의 `SIG_IGN` 설정과 `send_all()` 의 `MSG_NOSIGNAL` 을 **둘 다** 빼고
   `make integration` 의 `test_client_reset_during_download_no_sigpipe` 를 돌려 보세요.
   (예상: 서버가 SIGPIPE 로 종료되어 테스트 실패 — 이 실험은 작업 중 직접 실행해 보지는 않았으니 결과를 직접 확인해 보세요.)

## 12. 읽는 순서 추천

1. `src/server.c` — `server_run()` → `accept_loop()` → `handle_connection()` (전체 흐름)
2. `src/thread_pool.c` — `cq_push()` / `cq_pop()` / `tp_shutdown()` (동시성)
3. `src/http_conn.c` + `src/net_io.c` — `http_read_request()`, `recv_until_deadline()`, `send_all()` (바이트 스트림)
4. `src/http_parser.c` — `http_parse_request_head()` (단위 테스트 `tests/unit/test_unit.c` 와 같이 보기)
5. `src/static_files.c` → `src/proxy.c` (기능)
