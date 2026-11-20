# web-server

POSIX 소켓 API만으로 만든 C 웹 서버입니다. libevent/libuv 같은 이벤트 라이브러리나 외부 HTTP 라이브러리를 쓰지 않았습니다(테스트 프레임워크 Unity만 `vendor/`에 포함).

정적 파일 서빙, **리버스 프록시**(라운드 로빈·헬스 체크·재시도), **백엔드 연결 풀**, **메모리 캐시**를 단일 스레드 I/O 멀티플렉싱(kqueue/epoll) 위에서 처리합니다.

## 기능

| 영역 | 내용 |
|---|---|
| 이벤트 루프 | macOS `kqueue`, Linux `epoll` (`src/event.c`, level-triggered, 논블로킹 소켓) |
| HTTP 파서 | 증분(incremental) 파서. 어느 바이트에서 끊겨 들어와도 이어서 파싱. GET/HEAD/POST, Content-Length 본문 |
| 정적 서빙 | MIME 타입, 403/404/500, 디렉터리 `index.html`, 디렉터리 리다이렉트(301), 64KB 단위 스트리밍 |
| 리버스 프록시 | `route` 접두사 매칭(최장 일치), prefix strip, 라운드 로빈, 연결 실패 시 다른 서버로 재시도, 10초간 장애 서버 제외 |
| 연결 풀 | 백엔드 keep-alive 연결 재사용(서버별 LIFO). 오래된(stale) 연결은 GET/HEAD만 새 연결로 재시도 |
| 메모리 캐시 | RFC 7234 부분 구현. TTL, Vary, LRU 메모리 상한, POST 무효화. 캐시 경로에만 옵트인 |
| 안정성 | SIGPIPE 무시, SIGINT/SIGTERM 정상 종료, 헤더/유휴/connect/upstream 타임아웃(Slowloris 방어) |

### 보안·견고성 처리

- 요청 줄 8KB, 헤더 16KB, 본문 1MB 제한 (414 / 431 / 413)
- 중복 `Content-Length`, obs-fold, 콜론 앞 공백, chunked 요청 본문은 거부
- 정적 경로: 퍼센트 디코딩 후 `.`으로 시작하는 세그먼트 거부, `realpath` + docroot 접두사 검사(심볼릭 링크 탈출 차단), `O_NOFOLLOW`
- 프록시: hop-by-hop 헤더 제거, `X-Forwarded-For` 추가, `Via` 추가, 백엔드 응답 프레이밍(Content-Length/chunked/close) 검증, 백프레셔(클라이언트가 느리면 백엔드 읽기 중단)
- 캐시: `Authorization`·`Range`·`Set-Cookie`·`no-store`·`private`이 있으면 저장·조회하지 않음. 응답이 정확히 끝난 경우에만 저장

## 빌드와 실행

필요한 것: `cc`(gcc/clang), `make`. 테스트에는 `curl`, `nc`, `ab`(Apache Bench)가 추가로 필요합니다.

```bash
make            # build/webserver, build/backend(테스트용 C 백엔드) 생성
make run        # 포트 8080, docroot 기본값으로 실행
```

```bash
./build/webserver -c conf/example.conf        # 설정 파일로 실행
./build/webserver -c conf/example.conf -p 9090 # CLI 옵션이 설정 파일보다 우선
./build/webserver -p 8080 -r ./public          # 정적 서빙만
```

| 옵션 | 의미 |
|---|---|
| `-c FILE` | 설정 파일 |
| `-p PORT` | 포트 (설정 파일의 `listen`을 덮어씀) |
| `-r DIR` | 문서 루트 (설정 파일의 `docroot`를 덮어씀) |

`Ctrl-C`(SIGINT) 또는 SIGTERM으로 종료하면 풀·캐시 통계를 stderr에 출력하고 종료 코드 0으로 끝납니다.

### 프록시 시험해 보기

```bash
./build/backend 9001 &
./build/backend 9002 &
./build/webserver -c conf/example.conf &

curl -i http://localhost:8080/            # 정적 파일 (public/index.html)
curl -i http://localhost:8080/api/whoami  # 9001/9002로 번갈아 전달
```

## 설정 파일

`conf/example.conf` 예시. `#` 뒤는 주석, 시간은 `10s`/`500ms` 형식입니다.

```
listen 8080
docroot ./public

timeout_header   10s     # 요청을 다 받기까지 (Slowloris 방어)
timeout_idle     30s     # keep-alive 대기 / 응답 쓰기 정체
timeout_connect  3s      # 백엔드 connect
timeout_upstream 30s     # 백엔드 응답 무반응
pool_size        16      # 백엔드 서버당 유휴 keep-alive 연결 수 (0이면 풀 끔)
timeout_pool     5s      # 유휴 연결 유지 시간

cache_size       64m     # 0이면 캐시 끔 (k/m/g)
cache_max_object 1m      # 이보다 큰 응답은 저장 안 함
cache_ttl        0       # 백엔드가 신선도를 주지 않을 때 기본 TTL (0이면 저장 안 함)

upstream api {           # 중괄호는 별도 줄에 써야 합니다
    server 127.0.0.1:9001
    server 127.0.0.1:9002
}

route /api/ api strip    # /api/x -> 백엔드의 /x
route /static-api/ api cache   # 이 경로만 캐시 사용 (cache_size > 0 필요)
```

- `route PREFIX UPSTREAM [strip] [cache]`: 가장 긴 접두사가 이깁니다. 어떤 route에도 안 맞으면 docroot에서 정적으로 서빙합니다.
- `cache` 옵션을 쓰면서 `cache_size`가 0이면 설정 오류로 시작하지 않습니다.
- 한 줄짜리 `upstream x { server ... }`은 지원하지 않습니다.

## 캐시 동작 요약

- **키**: `소문자 Host` + `\n` + 요청 URI(쿼리 포함)
- **저장 대상**: GET의 200/301, 명시적 신선도가 있는 404
- **신선도**: `s-maxage` > `max-age` > `cache_ttl`, 백엔드 `Age`는 차감, 최대 1일
- **응답 헤더**: `X-Cache: HIT | MISS | BYPASS`, HIT에는 `Age`
- **무효화**: 같은 URI로 POST가 오면 해당 항목 삭제. 요청의 `no-cache`/`Pragma: no-cache`는 BYPASS(새로 받아 갱신)
- **퇴출**: 메모리 상한 초과 시 LRU

## 테스트

```bash
make test       # 단위 테스트 154개 (Unity, ASan+UBSan)
make itest      # ASan 빌드 후 통합 테스트 128개 (프록시·풀 50, 캐시 78)
```

| 파일 | 대상 | 개수 |
|---|---|---|
| `tests/test_http_parser.c` | 증분 파서, 에러 코드, 바이트 단위 분할 | 43 |
| `tests/test_mime.c` | MIME 타입 | 5 |
| `tests/test_static_file.c` | 경로 해석, 경로 탈출, 심볼릭 링크 | 19 |
| `tests/test_config.c` | 설정 파서 | 15 |
| `tests/test_router.c` | 라우팅, 라운드 로빈, 헬스 체크 | 13 |
| `tests/test_proxy_msg.c` | 응답 파서, chunked 디코더, 요청 재작성 | 27 |
| `tests/test_cache.c` | 정책, TTL, Vary, LRU (시간 주입으로 `sleep` 없음) | 32 |
| `tests/itest_proxy.sh` | 라우팅, 프레이밍, 풀 재사용, 502/504, 타임아웃, 동시성, SIGTERM | 50 |
| `tests/itest_cache.sh` | MISS/HIT, TTL, Vary, 무효화, LRU, 파이프라인 | 78 |

테스트용 백엔드 `tools/backend.c`는 `/whoami`, `/chunked`, `/slow`, `/hang`, `/truncate`, `/badresp`, `/c-*`(캐시 시나리오) 등 장애·경계 상황을 만드는 엔드포인트를 제공합니다.

### 검증 상태

| 환경 | 결과 |
|---|---|
| macOS (kqueue) | 단위 154 + 통합 128 통과, `leaks` 0건 |
| Linux aarch64 / Docker (epoll) | 단위 154 + 통합 128 통과, ASan+LeakSanitizer 오류·누수 없음 |

참고: macOS arm64에서는 LeakSanitizer를 쓸 수 없어 `leaks --atExit`으로 대신 확인했습니다. x86_64 Linux는 직접 실행해 보지 않았습니다.

### 부하 테스트 (참고용)

macOS loopback, `ab -k`, 동시 50 기준의 상대 비교입니다. 테스트 백엔드가 연결마다 `fork`하므로 절대 수치로 읽지 마세요.

| 구성 | 처리량 |
|---|---|
| 연결 풀 켬 / 끔 | 약 3.65만 / 약 0.78만 req/s |
| 캐시 HIT / 캐시 없이 전달 | 약 12만 / 약 4.7만 req/s |

## 구조

```
src/
  main.c          인자 파싱, 시그널, 서버 시작
  server.c        연결 상태 머신: 정적 서빙, 프록시, 풀, 캐시 연동, 타임아웃
  event.c/.h      kqueue / epoll 추상화
  http_parser.*   증분 HTTP 요청 파서
  static_file.*   URL -> 파일 경로 해석과 보안 검사
  mime.*          확장자 -> MIME
  config.*        설정 파일 파서
  router.*        최장 접두사 라우팅, 라운드 로빈, 장애 서버 표시
  proxy_msg.*     백엔드 응답 파서, chunked 디코더, 요청/응답 헤더 재작성
  cache.*         메모리 캐시 (해시 테이블 + LRU)
tools/backend.c   테스트용 백엔드 (C)
tests/            Unity 단위 테스트, bash 통합 테스트
vendor/unity/     Unity v2.6.0
```

`server.c`의 모든 이벤트 `udata` 구조체는 첫 필드가 `int kind`(`CK_LISTENER/CLIENT/UPSTREAM/POOLED`)이며, 이벤트 디스패치가 이 값으로 처리 함수를 고릅니다.

### 요청 처리 흐름

```
accept -> 요청 읽기(증분 파서) -> 라우팅
   |-- route 없음 -> static_resolve -> 파일 스트리밍
   `-- route 있음 -> (캐시 조회: HIT이면 바로 응답)
                  -> 풀에서 백엔드 연결 꺼냄 / 없으면 connect
                  -> 요청 재작성 후 전송 -> 응답 헤더 파싱
                  -> 본문 중계(+캐시 저장) -> 연결을 풀에 반환
```

## 알려진 한계

- HTTPS, HTTP/2, chunked **요청** 본문, Range 요청, gzip은 지원하지 않습니다.
- Forward Proxy(`CONNECT` 등)는 없습니다. 리버스 프록시만 구현했습니다.
- 캐시: 키당 Vary 변형 하나만 보관, `If-None-Match`/304 재검증 없음, 동시 만료 시 요청 합치기 없음, 만료 항목은 재조회나 LRU 퇴출 전까지 메모리를 차지합니다.
- 타임아웃 검사는 250ms마다 연결 전체를 훑는 O(n) 방식입니다(타이머 힙 아님).
- 단일 스레드입니다. 정적 파일 읽기는 이벤트 루프 안에서 일어납니다.
- 큰 정적 파일은 이벤트 한 번에 최대 256KB까지만 쓰고 다음 이벤트로 넘기는 방식이라, 한 연결이 루프를 독점하지는 않지만 디스크 읽기 자체는 논블로킹이 아닙니다.
- 백엔드 동시 연결 수 상한이 없습니다.
- 포트폴리오 목적의 학습용 프로젝트이며 프로덕션 사용을 전제로 하지 않았습니다.
