/*
 * test_unit.c — 소켓 없이(또는 socketpair 로) 돌릴 수 있는 C 단위 테스트.
 *
 * 외부 테스트 프레임워크 없이 CHECK 매크로로 성공/실패 개수만 센다.
 * 실행: make unit   (실패가 하나라도 있으면 종료 코드 1)
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "http_conn.h"
#include "net_io.h"
#include "proxy.h"
#include "router.h"
#include "static_files.h"
#include "thread_pool.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                          \
    } while (0)

#define CHECK_INT(actual, expected)                                                \
    do {                                                                           \
        long long a_ = (long long)(actual), e_ = (long long)(expected);            \
        if (a_ == e_) {                                                            \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,  \
                    __LINE__, #actual, a_, e_);                                    \
        }                                                                          \
    } while (0)

#define CHECK_STR(actual, expected)                                                \
    do {                                                                           \
        const char *a_ = (actual), *e_ = (expected);                               \
        if (a_ != NULL && strcmp(a_, e_) == 0) {                                   \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n",        \
                    __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_);          \
        }                                                                          \
    } while (0)

/* 문자열 리터럴을 수정 가능한 버퍼로 복사해서 파싱 (파서는 버퍼를 고친다) */
static int parse_req(const char *raw, http_request_t *req, char *buf, size_t bufsz)
{
    size_t len = strlen(raw);
    if (len >= bufsz)
        return -1;
    memcpy(buf, raw, len + 1);
    size_t head_end = http_find_head_end(buf, len);
    if (head_end == 0)
        return -2;
    return http_parse_request_head(buf, head_end, req);
}

/* ------------------------------------------------------------------ */
static void test_find_head_end(void)
{
    puts("[unit] http_find_head_end");
    CHECK_INT(http_find_head_end("GET / HTTP/1.1\r\nHost: a\r\n\r\n", 27), 27);
    CHECK_INT(http_find_head_end("GET / HTTP/1.1\r\nHost: a\r\n\r", 26), 0); /* 아직 덜 옴 */
    CHECK_INT(http_find_head_end("GET / HTTP/1.1\r\n\r\nBODY", 22), 18);    /* 본문 앞에서 끝 */
    CHECK_INT(http_find_head_end("", 0), 0);
    CHECK_INT(http_find_head_end("\n\n\n\n", 4), 0); /* bare LF 는 인정하지 않음 */
}

static void test_request_line(void)
{
    puts("[unit] request line / version / method");
    char buf[4096];
    http_request_t r;

    CHECK_INT(parse_req("GET /index.html?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK_STR(r.method_str, "GET");
    CHECK_INT(r.method, HTTP_METHOD_GET);
    CHECK_STR(r.target, "/index.html?x=1");
    CHECK_INT(r.version_minor, 1);
    CHECK(r.keep_alive);

    CHECK_INT(parse_req("HEAD / HTTP/1.0\r\n\r\n", &r, buf, sizeof buf), 0); /* 1.0 은 Host 불필요 */
    CHECK_INT(r.method, HTTP_METHOD_HEAD);
    CHECK(!r.keep_alive);

    CHECK_INT(parse_req("POST /api HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK_INT(r.method, HTTP_METHOD_POST);
    CHECK_INT(parse_req("DELETE /x HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK_INT(r.method, HTTP_METHOD_OTHER);

    CHECK_INT(parse_req("get / HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 501);
    CHECK_INT(parse_req("BREW / HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 501);
    CHECK_INT(parse_req("G@T / HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/2.0\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 505);
    CHECK_INT(parse_req("GET / HTTP/1.2\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 505);
    CHECK_INT(parse_req("GET / HTTP/1.1x\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / FTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET /\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET http://evil.com/ HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("garbage\r\n\r\n", &r, buf, sizeof buf), 400);

    /* URI 길이: 정확히 한도는 통과, 1 바이트 넘으면 414 */
    char line[HTTP_MAX_TARGET_LEN + 128];
    char target[HTTP_MAX_TARGET_LEN + 2];
    target[0] = '/';
    memset(target + 1, 'a', HTTP_MAX_TARGET_LEN - 1);
    target[HTTP_MAX_TARGET_LEN] = '\0';
    snprintf(line, sizeof line, "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", target);
    CHECK_INT(parse_req(line, &r, buf, sizeof buf), 0);
    target[HTTP_MAX_TARGET_LEN] = 'a';
    target[HTTP_MAX_TARGET_LEN + 1] = '\0';
    snprintf(line, sizeof line, "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", target);
    CHECK_INT(parse_req(line, &r, buf, sizeof buf), 414);
}

static void test_headers(void)
{
    puts("[unit] headers / Host / Connection");
    char buf[8192];
    http_request_t r;

    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost:   example.com  \r\nX-A:b\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK_INT(r.header_count, 2);
    CHECK_STR(http_header_get(r.headers, r.header_count, "host"), "example.com"); /* OWS 제거, 대소문자 무시 */
    CHECK_STR(http_header_get(r.headers, r.header_count, "X-A"), "b");
    CHECK(http_header_get(r.headers, r.header_count, "missing") == NULL);

    CHECK_INT(parse_req("GET / HTTP/1.1\r\n\r\n", &r, buf, sizeof buf), 400);              /* Host 없음 */
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &r, buf, sizeof buf), 400); /* 이름 뒤 공백 */
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\n: novalue\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n", &r, buf, sizeof buf), 400); /* bare LF */

    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK(!r.keep_alive);
    CHECK_INT(parse_req("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK(!r.keep_alive);
    CHECK_INT(parse_req("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK(r.keep_alive);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK(r.expect_continue);

    /* 헤더 개수 상한 */
    char many[8192];
    size_t off = (size_t)snprintf(many, sizeof many, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++)
        off += (size_t)snprintf(many + off, sizeof many - off, "X-%d: v\r\n", i);
    snprintf(many + off, sizeof many - off, "\r\n");
    CHECK_INT(parse_req(many, &r, buf, sizeof buf), 431); /* Host + 64개 = 65개 */

    CHECK(http_header_has_token("keep-alive, Upgrade", "upgrade"));
    CHECK(!http_header_has_token("keep-alive-ish", "keep-alive"));
    CHECK(http_header_has_token(" close ", "close"));
}

static void test_content_length(void)
{
    puts("[unit] Content-Length / Transfer-Encoding");
    unsigned long long v = 0;
    CHECK_INT(http_parse_content_length("0", &v), 0);
    CHECK_INT(v, 0);
    CHECK_INT(http_parse_content_length("12345", &v), 0);
    CHECK_INT(v, 12345);
    CHECK_INT(http_parse_content_length("18446744073709551615", &v), 0);
    CHECK_INT(http_parse_content_length("18446744073709551616", &v), -1); /* 오버플로 */
    CHECK_INT(http_parse_content_length("-1", &v), -1);
    CHECK_INT(http_parse_content_length("1 2", &v), -1);
    CHECK_INT(http_parse_content_length("0x10", &v), -1);
    CHECK_INT(http_parse_content_length("", &v), -1);
    CHECK_INT(http_parse_content_length("+5", &v), -1);

    char buf[4096];
    http_request_t r;
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK(r.has_content_length);
    CHECK_INT(r.content_length, 10);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &r, buf, sizeof buf), 0);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &r, buf, sizeof buf), 400);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", &r, buf, sizeof buf), 501);
    CHECK_INT(parse_req("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", &r, buf, sizeof buf), 400);
}

static void test_response_head(void)
{
    puts("[unit] backend response head");
    char buf[1024];
    http_response_head_t h;
    strcpy(buf, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: b\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), 0);
    CHECK_INT(h.status, 200);
    CHECK_STR(h.reason, "OK");
    CHECK(h.has_content_length);
    CHECK_INT(h.content_length, 5);
    strcpy(buf, "HTTP/1.0 404\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), 0);
    CHECK_INT(h.status, 404);
    CHECK_STR(h.reason, "");
    strcpy(buf, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), 0);
    CHECK(h.has_transfer_encoding);
    strcpy(buf, "HTTP/2 200 OK\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), -1);
    strcpy(buf, "HTTP/1.1 2x0 OK\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), -1);
    strcpy(buf, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
    CHECK_INT(http_parse_response_head(buf, strlen(buf), &h), -1);
}

static void test_normalize(void)
{
    puts("[unit] path normalization / traversal");
    char out[HTTP_MAX_TARGET_LEN + 16];
#define NORM(t) static_normalize_target((t), out, sizeof out, false)
    CHECK_INT(NORM("/"), 0);
    CHECK_STR(out, "/");
    CHECK_INT(NORM("/index.html"), 0);
    CHECK_STR(out, "/index.html");
    CHECK_INT(NORM("/a/./b"), 0);
    CHECK_STR(out, "/a/b");
    CHECK_INT(NORM("/a//b"), 0);
    CHECK_STR(out, "/a/b");
    CHECK_INT(NORM("/a/../b"), 0);
    CHECK_STR(out, "/b");
    CHECK_INT(NORM("/a/b/.."), 0);
    CHECK_STR(out, "/a/");
    CHECK_INT(NORM("/dir/"), 0);
    CHECK_STR(out, "/dir/");
    CHECK_INT(NORM("/a?x=../../etc"), 0); /* query 는 경로가 아님 */
    CHECK_STR(out, "/a");
    CHECK_INT(NORM("/hello%20world.txt"), 0);
    CHECK_STR(out, "/hello world.txt");
    CHECK_INT(NORM("/%252e%252e/x"), 0); /* 이중 인코딩은 한 번만 디코딩 → 글자 "%2e%2e" */
    CHECK_STR(out, "/%2e%2e/x");

    CHECK_INT(NORM("/../etc/passwd"), 403);
    CHECK_INT(NORM("/a/../../etc/passwd"), 403);
    CHECK_INT(NORM("/%2e%2e/etc/passwd"), 403);
    CHECK_INT(NORM("/%2E%2E%2Fetc%2Fpasswd"), 403);
    CHECK_INT(NORM("/..%2f..%2fetc/passwd"), 403);
    CHECK_INT(NORM("/.git/config"), 403);
    CHECK_INT(NORM("/a/.env"), 403);
    CHECK_INT(NORM("/a%00.html"), 400);
    CHECK_INT(NORM("/a%0a"), 400);
    CHECK_INT(NORM("/%zz"), 400);
    CHECK_INT(NORM("/%4"), 400);
    CHECK_INT(NORM("/abc%"), 400);
    CHECK_INT(NORM("noslash"), 400);
    CHECK_INT(static_normalize_target("/api/.well-known", out, sizeof out, true), 0);
    CHECK_STR(out, "/api/.well-known");
#undef NORM
}

static void test_mime(void)
{
    puts("[unit] MIME mapping");
    CHECK_STR(mime_type_for_path("/x/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("/x/INDEX.HTML"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for_path("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("logo.png"), "image/png");
    CHECK_STR(mime_type_for_path("photo.jpg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("photo.JPEG"), "image/jpeg");
    CHECK_STR(mime_type_for_path("data.json"), "application/json");
    CHECK_STR(mime_type_for_path("README"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/dir.v1/file"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/x/.hidden"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("archive.tar.gz"), "application/octet-stream");
}

static void test_path_within(void)
{
    puts("[unit] path_is_within");
    CHECK(path_is_within("/srv/www", "/srv/www"));
    CHECK(path_is_within("/srv/www", "/srv/www/a.html"));
    CHECK(!path_is_within("/srv/www", "/srv/www2/a.html")); /* 접두사만 같은 형제 디렉터리 */
    CHECK(!path_is_within("/srv/www", "/etc/passwd"));
    CHECK(path_is_within("/", "/etc/passwd"));
}

static void test_router(void)
{
    puts("[unit] routing");
    server_config_t cfg;
    config_set_defaults(&cfg);
    CHECK(router_prefix_matches("/api", "/api"));
    CHECK(router_prefix_matches("/api/x", "/api"));
    CHECK(router_prefix_matches("/api?q=1", "/api"));
    CHECK(!router_prefix_matches("/apix", "/api"));
    CHECK(!router_prefix_matches("/static/api", "/api"));

    CHECK_INT(router_route(&cfg, "/api/x"), ROUTE_STATIC); /* 백엔드 미설정 → 정적 */
    cfg.backend_count = 1;
    CHECK_INT(router_route(&cfg, "/api/x"), ROUTE_PROXY);
    CHECK_INT(router_route(&cfg, "/index.html"), ROUTE_STATIC);
    CHECK_INT(router_route(&cfg, "/%61pi/x"), ROUTE_STATIC);

    const char *allow = NULL;
    CHECK(router_method_allowed(ROUTE_STATIC, HTTP_METHOD_GET, &allow));
    CHECK(router_method_allowed(ROUTE_STATIC, HTTP_METHOD_HEAD, &allow));
    CHECK(!router_method_allowed(ROUTE_STATIC, HTTP_METHOD_POST, &allow));
    CHECK_STR(allow, "GET, HEAD");
    CHECK(router_method_allowed(ROUTE_PROXY, HTTP_METHOD_POST, &allow));
    CHECK(!router_method_allowed(ROUTE_PROXY, HTTP_METHOD_OTHER, &allow));
    CHECK_STR(allow, "GET, HEAD, POST");

    CHECK(proxy_is_hop_by_hop("connection", NULL));
    CHECK(proxy_is_hop_by_hop("Keep-Alive", NULL));
    CHECK(proxy_is_hop_by_hop("Transfer-Encoding", NULL));
    CHECK(proxy_is_hop_by_hop("X-Custom", "keep-alive, X-Custom"));
    CHECK(!proxy_is_hop_by_hop("Content-Type", "keep-alive"));
}

static void test_config(void)
{
    puts("[unit] CLI parsing");
    server_config_t cfg;
    char err[256];
    config_set_defaults(&cfg);
    char *ok[] = {"ws", "-p", "9090", "--root", "/tmp", "-t", "8", "--queue", "16",
                  "--backend", "127.0.0.1:9000", "-b", "localhost:9001",
                  "--proxy-prefix", "/svc/", "--max-body", "100", "-q", NULL};
    CHECK_INT(config_parse_args(&cfg, 18, ok, err, sizeof err), CONFIG_OK);
    CHECK_INT(cfg.port, 9090);
    CHECK_STR(cfg.doc_root, "/tmp");
    CHECK_INT(cfg.threads, 8);
    CHECK_INT(cfg.queue_capacity, 16);
    CHECK_INT(cfg.backend_count, 2);
    CHECK_STR(cfg.backends[1].host, "localhost");
    CHECK_INT(cfg.backends[1].port, 9001);
    CHECK_STR(cfg.proxy_prefix, "/svc");
    CHECK_INT(cfg.max_body_bytes, 100);
    CHECK(cfg.quiet);

    char *help[] = {"ws", "--help", NULL};
    config_set_defaults(&cfg);
    CHECK_INT(config_parse_args(&cfg, 2, help, err, sizeof err), CONFIG_HELP);

    char *bad_sets[][3] = {
        {"ws", "--port", "70000"}, {"ws", "--port", "12ab"}, {"ws", "--threads", "0"},
        {"ws", "--backend", "nohost"}, {"ws", "--backend", "h:0"}, {"ws", "--bogus", "1"},
        {"ws", "--proxy-prefix", "api"}, {"ws", "--proxy-prefix", "/"}, {"ws", "--queue", "-1"},
    };
    for (size_t i = 0; i < sizeof bad_sets / sizeof bad_sets[0]; i++) {
        config_set_defaults(&cfg);
        CHECK_INT(config_parse_args(&cfg, 3, bad_sets[i], err, sizeof err), CONFIG_ERROR);
    }
    char *missing[] = {"ws", "--port", NULL};
    config_set_defaults(&cfg);
    CHECK_INT(config_parse_args(&cfg, 2, missing, err, sizeof err), CONFIG_ERROR);
}

/* ---------------- 큐 / 스레드 풀 ---------------- */

static void test_queue_basic(void)
{
    puts("[unit] bounded queue: capacity, FIFO, wraparound, close");
    conn_queue_t q;
    CHECK_INT(cq_init(&q, 3), 0);
    CHECK_INT(cq_push(&q, 10), CQ_OK);
    CHECK_INT(cq_push(&q, 11), CQ_OK);
    CHECK_INT(cq_push(&q, 12), CQ_OK);
    CHECK_INT(cq_push(&q, 13), CQ_FULL); /* 가득 차면 즉시 FULL (블록 안 함) */
    CHECK_INT(cq_size(&q), 3);
    int fd = -1;
    CHECK_INT(cq_pop(&q, &fd), 0);
    CHECK_INT(fd, 10); /* FIFO */
    CHECK_INT(cq_push(&q, 14), CQ_OK); /* 원형 버퍼 wraparound */
    CHECK_INT(cq_pop(&q, &fd), 0);
    CHECK_INT(fd, 11);
    CHECK_INT(cq_pop(&q, &fd), 0);
    CHECK_INT(fd, 12);
    CHECK_INT(cq_pop(&q, &fd), 0);
    CHECK_INT(fd, 14);
    CHECK_INT(cq_try_pop(&q, &fd), -1);
    CHECK_INT(cq_push(&q, 15), CQ_OK);
    cq_close(&q);
    CHECK_INT(cq_push(&q, 16), CQ_CLOSED);
    CHECK_INT(cq_pop(&q, &fd), -1);       /* 닫힌 큐: 남은 항목이 있어도 -1 */
    CHECK_INT(cq_try_pop(&q, &fd), 0);    /* 정리용 try_pop 으로는 꺼낼 수 있음 */
    CHECK_INT(fd, 15);
    cq_destroy(&q);
    CHECK_INT(cq_init(&q, 0), -1);
}

typedef struct {
    conn_queue_t *q;
    int result;
    int fd;
} popper_arg_t;

static void *popper(void *arg)
{
    popper_arg_t *pa = arg;
    pa->result = cq_pop(pa->q, &pa->fd);
    return NULL;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void test_queue_blocking(void)
{
    puts("[unit] bounded queue: blocking pop woken by push / close");
    conn_queue_t q;
    cq_init(&q, 2);
    pthread_t t;
    popper_arg_t pa = {.q = &q, .result = 99, .fd = -1};
    pthread_create(&t, NULL, popper, &pa);
    sleep_ms(50); /* popper 가 빈 큐에서 잠들 시간 */
    cq_push(&q, 42);
    pthread_join(t, NULL);
    CHECK_INT(pa.result, 0);
    CHECK_INT(pa.fd, 42);

    popper_arg_t pb = {.q = &q, .result = 99, .fd = -1};
    pthread_create(&t, NULL, popper, &pb);
    sleep_ms(50);
    cq_close(&q); /* 잠든 worker 를 깨워 종료시킨다 */
    pthread_join(t, NULL);
    CHECK_INT(pb.result, -1);
    cq_destroy(&q);
}

typedef struct {
    atomic_int handled;
    atomic_int closed_by_reject;
    atomic_bool release;
    atomic_int in_handler;
} pool_ctx_t;

static void counting_handler(int fd, void *ctx)
{
    pool_ctx_t *pc = ctx;
    atomic_fetch_add(&pc->in_handler, 1);
    while (!atomic_load(&pc->release))
        sleep_ms(1);
    atomic_fetch_add(&pc->handled, 1);
    close(fd);
}

static void counting_reject(int fd, void *ctx)
{
    pool_ctx_t *pc = ctx;
    atomic_fetch_add(&pc->closed_by_reject, 1);
    close(fd);
}

static void test_thread_pool(void)
{
    puts("[unit] thread pool: all jobs run, queue saturation, shutdown cleanup");
    pool_ctx_t pc;
    atomic_init(&pc.handled, 0);
    atomic_init(&pc.closed_by_reject, 0);
    atomic_init(&pc.release, true);
    atomic_init(&pc.in_handler, 0);

    thread_pool_t tp;
    CHECK_INT(tp_init(&tp, 4, 64, counting_handler, &pc), 0);
    int submitted = 0;
    for (int i = 0; i < 200; i++) {
        int fd = dup(STDERR_FILENO); /* 진짜 fd 로 테스트 (handler 가 close) */
        while (tp_submit(&tp, fd) == CQ_FULL)
            sleep_ms(1);
        submitted++;
    }
    for (int i = 0; i < 2000 && atomic_load(&pc.handled) < submitted; i++)
        sleep_ms(1);
    CHECK_INT(atomic_load(&pc.handled), 200);
    tp_shutdown(&tp, counting_reject, &pc);
    CHECK_INT(atomic_load(&pc.closed_by_reject), 0);

    /* 포화: worker 2개가 막혀 있고 큐 2칸이 차면 다음 submit 은 FULL */
    atomic_store(&pc.handled, 0);
    atomic_store(&pc.release, false);
    atomic_store(&pc.in_handler, 0);
    CHECK_INT(tp_init(&tp, 2, 2, counting_handler, &pc), 0);
    CHECK_INT(tp_submit(&tp, dup(STDERR_FILENO)), CQ_OK);
    CHECK_INT(tp_submit(&tp, dup(STDERR_FILENO)), CQ_OK);
    for (int i = 0; i < 1000 && atomic_load(&pc.in_handler) < 2; i++)
        sleep_ms(1); /* 두 worker 가 각각 하나씩 집어 갈 때까지 */
    CHECK_INT(tp_submit(&tp, dup(STDERR_FILENO)), CQ_OK);
    CHECK_INT(tp_submit(&tp, dup(STDERR_FILENO)), CQ_OK);
    int extra = dup(STDERR_FILENO);
    CHECK_INT(tp_submit(&tp, extra), CQ_FULL);
    close(extra);
    CHECK_INT(tp_pending(&tp), 2);

    /* 종료: 처리 중인 2개는 끝까지, 대기 중인 2개는 reject 로 정리 */
    atomic_store(&pc.release, true);
    tp_shutdown(&tp, counting_reject, &pc);
    CHECK_INT(atomic_load(&pc.handled) + atomic_load(&pc.closed_by_reject), 4);
    CHECK(atomic_load(&pc.handled) >= 2);
}

/* ---------------- 소켓 I/O ---------------- */

typedef struct {
    int fd;
    size_t total;
    unsigned char *got;
} slow_reader_t;

static void *slow_reader(void *arg)
{
    slow_reader_t *sr = arg;
    size_t off = 0;
    while (off < sr->total) {
        ssize_t n = recv(sr->fd, sr->got + off, 1000, 0); /* 일부러 조금씩 */
        if (n <= 0)
            break;
        off += (size_t)n;
        if ((off / 1000) % 64 == 0)
            sleep_ms(1);
    }
    sr->total = off;
    return NULL;
}

static void test_send_all_partial(void)
{
    puts("[unit] send_all with partial sends (socketpair, small buffers)");
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);

    size_t total = 2 * 1024 * 1024;
    unsigned char *data = malloc(total);
    unsigned char *got = calloc(1, total);
    for (size_t i = 0; i < total; i++)
        data[i] = (unsigned char)(i * 31 + 7);

    slow_reader_t sr = {.fd = sv[1], .total = total, .got = got};
    pthread_t t;
    pthread_create(&t, NULL, slow_reader, &sr);
    CHECK_INT(send_all(sv[0], data, total, now_ms() + 10000), 0);
    pthread_join(t, NULL);
    CHECK_INT(sr.total, total);
    CHECK(memcmp(data, got, total) == 0);

    /* 아무도 읽지 않으면 send_all 은 마감 시각에 IO_TIMEOUT */
    int64_t t0 = now_ms();
    CHECK_INT(send_all(sv[0], data, total, now_ms() + 200), IO_TIMEOUT);
    CHECK(now_ms() - t0 >= 150);

    /* 상대가 닫은 뒤 쓰면 SIGPIPE 없이 -1 (EPIPE) */
    close(sv[1]);
    CHECK_INT(send_all(sv[0], data, 1024 * 1024, now_ms() + 1000), -1);
    close(sv[0]);
    free(data);
    free(got);
}

static void test_recv_timeout_and_reader(void)
{
    puts("[unit] recv_until_deadline + http_read_request over socketpair");
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    char b[16];
    int64_t t0 = now_ms();
    CHECK_INT(recv_until_deadline(sv[0], b, sizeof b, now_ms() + 100), IO_TIMEOUT);
    CHECK(now_ms() - t0 >= 90);

    /* 요청 2개 + 본문을 한 번에 써 넣고 http_read_request 로 차례로 읽는다 */
    const char *two = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                      "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(send_all(sv[1], two, strlen(two), now_ms() + 1000), 0);

    http_conn_t *c = malloc(sizeof *c);
    http_conn_init(c, sv[0]);
    http_limits_t lim = {.read_timeout_ms = 500, .max_body_bytes = 1024};
    http_request_t req;
    int st = 0;
    CHECK_INT(http_read_request(c, &req, &lim, &st), READ_OK);
    CHECK_STR(req.target, "/a");
    CHECK_INT(req.body_len, 5);
    CHECK(req.body && memcmp(req.body, "hello", 5) == 0);
    http_request_free(&req);
    http_conn_consume(c);
    CHECK_INT(http_read_request(c, &req, &lim, &st), READ_OK);
    CHECK_STR(req.target, "/b");
    http_request_free(&req);
    http_conn_consume(c);
    CHECK_INT(c->len, 0);

    /* 본문 한도 초과 → 413 */
    const char *big = "POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 4096\r\n\r\n";
    send_all(sv[1], big, strlen(big), now_ms() + 1000);
    CHECK_INT(http_read_request(c, &req, &lim, &st), READ_BAD);
    CHECK_INT(st, 413);
    http_request_free(&req);

    /* 헤더 중간에 멈추면 408 */
    http_conn_init(c, sv[0]);
    send_all(sv[1], "GET / HTTP/1.1\r\nHo", 18, now_ms() + 1000);
    CHECK_INT(http_read_request(c, &req, &lim, &st), READ_BAD);
    CHECK_INT(st, 408);
    http_request_free(&req);

    /* 본문 도중 상대 종료 → READ_ABORTED */
    http_conn_init(c, sv[0]);
    const char *partial = "POST /d HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nonly-a-bit";
    send_all(sv[1], partial, strlen(partial), now_ms() + 1000);
    shutdown(sv[1], SHUT_WR);
    CHECK_INT(http_read_request(c, &req, &lim, &st), READ_ABORTED);
    http_request_free(&req);

    close(sv[0]);
    close(sv[1]);
    free(c);
}

int main(void)
{
    test_find_head_end();
    test_request_line();
    test_headers();
    test_content_length();
    test_response_head();
    test_normalize();
    test_mime();
    test_path_within();
    test_router();
    test_config();
    test_queue_basic();
    test_queue_blocking();
    test_thread_pool();
    test_send_all_partial();
    test_recv_timeout_and_reader();

    printf("\nunit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
