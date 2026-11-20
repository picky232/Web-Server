#include "unity.h"
#include "proxy_msg.h"

#include <stdlib.h>
#include <string.h>

static pm_resp r;

void setUp(void)    {}
void tearDown(void) {}

static int parse(const char *s) { return pm_resp_parse(s, strlen(s), &r); }

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

/* ------------------------------ 응답 파서 ------------------------------ */

void test_resp_content_length(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nServer: x\r\n\r\nhello";
    TEST_ASSERT_EQUAL_INT(1, parse(s));
    TEST_ASSERT_EQUAL_INT(200, r.status);
    TEST_ASSERT_EQUAL_INT(1, r.minor);
    TEST_ASSERT_EQUAL_UINT(5, r.content_length);
    TEST_ASSERT_EQUAL_UINT(strlen(s) - 5, r.head_len);  /* 본문 앞까지 */
    TEST_ASSERT_EQUAL_STRING_LEN("OK", s + r.reason_off, r.reason_len);
    TEST_ASSERT_EQUAL_INT(2, r.nheaders);
}

void test_resp_needs_more(void) {
    TEST_ASSERT_EQUAL_INT(0, parse("HTTP/1.1 200 OK\r\nContent-Le"));
    TEST_ASSERT_EQUAL_INT(0, parse("HTTP/1.1 200 OK\r\n\r"));
    TEST_ASSERT_EQUAL_INT(0, parse(""));
}

void test_resp_without_reason_phrase(void) {
    TEST_ASSERT_EQUAL_INT(1, parse("HTTP/1.1 204\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(204, r.status);
    TEST_ASSERT_EQUAL_UINT(0, r.reason_len);
}

void test_resp_http10(void) {
    TEST_ASSERT_EQUAL_INT(1, parse("HTTP/1.0 200 OK\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(0, r.minor);
}

void test_resp_chunked_overrides_content_length(void) {
    TEST_ASSERT_EQUAL_INT(1, parse("HTTP/1.1 200 OK\r\nContent-Length: 99\r\nTransfer-Encoding: Chunked\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(1, r.chunked);
    TEST_ASSERT_EQUAL_INT(0, r.has_cl);
    TEST_ASSERT_EQUAL_INT(BM_CHUNKED, pm_resp_body_mode(&r, 0));
}

void test_resp_connection_close_detected(void) {
    TEST_ASSERT_EQUAL_INT(1, parse("HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(1, r.conn_close);
}

void test_resp_rejects_malformed(void) {
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/2 200 OK\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 20 OK\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 abc OK\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200OK\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\n folded\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nNoColon\r\n\r\n"));
}

void test_resp_rejects_ambiguous_framing(void) {
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(-1, parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(1,  parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n"));  /* 같은 값 중복은 허용 */
}

void test_body_modes(void) {
    parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(BM_LENGTH, pm_resp_body_mode(&r, 0));
    TEST_ASSERT_EQUAL_INT(BM_NONE, pm_resp_body_mode(&r, 1));      /* HEAD */
    parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(BM_NONE, pm_resp_body_mode(&r, 0));
    parse("HTTP/1.1 204 No Content\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(BM_NONE, pm_resp_body_mode(&r, 0));
    parse("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(BM_NONE, pm_resp_body_mode(&r, 0));
    parse("HTTP/1.1 200 OK\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(BM_CLOSE, pm_resp_body_mode(&r, 0));     /* 길이 정보 없음: 연결 종료까지 */
}

/* ------------------------------ chunk 감지기 ------------------------------ */

static int feed_all(chunk_dec *d, const char *s, size_t *used) { return chunk_feed(d, s, strlen(s), used); }

void test_chunk_simple(void) {
    chunk_dec d; chunk_init(&d);
    size_t used;
    const char *s = "5\r\nhello\r\n0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, feed_all(&d, s, &used));
    TEST_ASSERT_EQUAL_UINT(strlen(s), used);
}

void test_chunk_multiple_with_extension_and_hex(void) {
    chunk_dec d; chunk_init(&d);
    size_t used;
    const char *s = "A;name=val\r\n0123456789\r\n1f\r\n0123456789012345678901234567890\r\n0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, feed_all(&d, s, &used));
    TEST_ASSERT_EQUAL_UINT(strlen(s), used);
}

void test_chunk_trailers(void) {
    chunk_dec d; chunk_init(&d);
    size_t used;
    const char *s = "3\r\nabc\r\n0\r\nX-Sum: 1\r\nX-Other: 2\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, feed_all(&d, s, &used));
    TEST_ASSERT_EQUAL_UINT(strlen(s), used);
}

void test_chunk_stops_at_end_and_leaves_rest(void) {
    chunk_dec d; chunk_init(&d);
    size_t used;
    const char *s = "0\r\n\r\nNEXT";
    TEST_ASSERT_EQUAL_INT(1, feed_all(&d, s, &used));
    TEST_ASSERT_EQUAL_UINT(5, used);  /* 뒤의 "NEXT" 는 소비하지 않음 */
}

void test_chunk_byte_by_byte(void) {
    chunk_dec d; chunk_init(&d);
    const char *s = "4;x=y\r\nwiki\r\n5\r\npedia\r\n0\r\nT: v\r\n\r\n";
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        size_t used;
        int rc = chunk_feed(&d, s + i, 1, &used);
        TEST_ASSERT_EQUAL_UINT(1, used);
        TEST_ASSERT_EQUAL_INT(i + 1 == n ? 1 : 0, rc);
    }
}

void test_chunk_split_inside_data(void) {
    chunk_dec d; chunk_init(&d);
    size_t used;
    TEST_ASSERT_EQUAL_INT(0, feed_all(&d, "a\r\n01234", &used));
    TEST_ASSERT_EQUAL_INT(0, feed_all(&d, "56789\r", &used));
    TEST_ASSERT_EQUAL_INT(1, feed_all(&d, "\n0\r\n\r\n", &used));
}

void test_chunk_errors(void) {
    chunk_dec d; size_t used;
    chunk_init(&d); TEST_ASSERT_EQUAL_INT(-1, feed_all(&d, "zz\r\n", &used));
    chunk_init(&d); TEST_ASSERT_EQUAL_INT(-1, feed_all(&d, "\r\n", &used));                 /* 크기 없음 */
    chunk_init(&d); TEST_ASSERT_EQUAL_INT(-1, feed_all(&d, "3\r\nabcXX", &used));           /* 데이터 뒤에 CRLF 없음 */
    chunk_init(&d); TEST_ASSERT_EQUAL_INT(-1, feed_all(&d, "3\nabc\r\n", &used));           /* LF 단독 */
    chunk_init(&d); TEST_ASSERT_EQUAL_INT(-1, feed_all(&d, "FFFFFFFFFFFFFFFFF\r\n", &used)); /* 17자리: 오버플로 */
}

/* ------------------------------ 헤더 재작성 ------------------------------ */

static hp_parser p;
static char req_buf[4096];

static char *build_req(const char *raw, const char *ip, size_t strip, int ka, size_t *len) {
    snprintf(req_buf, sizeof req_buf, "%s", raw);
    hp_init(&p, 16384, 8192, 1 << 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HP_COMPLETE, hp_feed(&p, req_buf, strlen(req_buf)), raw);
    return pm_build_request(&p, req_buf, ip, strip, ka, len);
}

void test_build_request_basic(void) {
    size_t len;
    char *o = build_req("GET /api/users?x=1 HTTP/1.1\r\nHost: example.com\r\nX-Foo: bar\r\n"
                        "Connection: keep-alive\r\nKeep-Alive: 5\r\nUpgrade: h2c\r\nExpect: 100-continue\r\n\r\n",
                        "1.2.3.4", 0, 0, &len);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT(strlen(o), len);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "GET /api/users?x=1 HTTP/1.1\r\n", 29));
    TEST_ASSERT_NOT_NULL(strstr(o, "Host: example.com\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "X-Foo: bar\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "X-Forwarded-For: 1.2.3.4\r\n"));
    TEST_ASSERT_NULL(strstr(o, "Keep-Alive"));
    TEST_ASSERT_NULL(strstr(o, "Upgrade"));
    TEST_ASSERT_NULL(strstr(o, "Expect"));
    TEST_ASSERT_EQUAL_INT(1, count(o, "Connection:"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Connection: close\r\n\r\n"));
    free(o);
}

void test_build_request_keepalive_flag(void) {
    size_t len;
    char *o = build_req("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", "1.1.1.1", 0, 1, &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "Connection: keep-alive\r\n"));
    TEST_ASSERT_EQUAL_INT(1, count(o, "Connection:"));
    free(o);
}

void test_build_request_strip_prefix(void) {
    size_t len;
    char *o = build_req("GET /api/users?x=1 HTTP/1.1\r\nHost: h\r\n\r\n", "1.1.1.1", 5, 0, &len);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "GET /users?x=1 HTTP/1.1\r\n", 25));
    free(o);
    o = build_req("GET /api/ HTTP/1.1\r\nHost: h\r\n\r\n", "1.1.1.1", 5, 0, &len);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "GET / HTTP/1.1\r\n", 16));  /* 전부 떼면 "/" */
    free(o);
    o = build_req("GET /api HTTP/1.1\r\nHost: h\r\n\r\n", "1.1.1.1", 4, 0, &len);  /* 접두사 "/api" (슬래시 없음) */
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "GET / HTTP/1.1\r\n", 16));
    free(o);
}

void test_build_request_appends_existing_xff(void) {
    size_t len;
    char *o = build_req("GET / HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 9.9.9.9\r\n\r\n", "1.2.3.4", 0, 0, &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "X-Forwarded-For: 9.9.9.9, 1.2.3.4\r\n"));
    TEST_ASSERT_EQUAL_INT(1, count(o, "X-Forwarded-For"));
    free(o);
}

void test_build_request_post_body(void) {
    size_t len;
    char *o = build_req("POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello", "1.1.1.1", 0, 0, &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "Content-Length: 5\r\n"));
    TEST_ASSERT_EQUAL_UINT(0, strcmp(o + len - 5, "hello"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "POST /submit HTTP/1.1\r\n", 23));
    free(o);
}

void test_build_request_absolute_form_becomes_origin_form(void) {
    size_t len;
    char *o = build_req("GET http://example.com/a/b?c=d HTTP/1.1\r\nHost: example.com\r\n\r\n", "1.1.1.1", 0, 0, &len);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "GET /a/b?c=d HTTP/1.1\r\n", 23));
    free(o);
}

void test_build_request_http10_without_host(void) {
    size_t len;
    char *o = build_req("GET / HTTP/1.0\r\n\r\n", "1.1.1.1", 0, 0, &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "Host: localhost\r\n"));
    free(o);
}

void test_build_client_head(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n"
                    "Connection: close\r\nKeep-Alive: timeout=5\r\nServer: be\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, parse(s));
    size_t len;
    char *o = pm_build_client_head(&r, s, 1, NULL, &len);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "HTTP/1.1 200 OK\r\n", 17));
    TEST_ASSERT_NOT_NULL(strstr(o, "Content-Length: 5\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Server: be\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Via: 1.1 webserver\r\n"));
    TEST_ASSERT_NULL(strstr(o, "Keep-Alive:"));
    TEST_ASSERT_EQUAL_INT(1, count(o, "Connection:"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Connection: keep-alive\r\n\r\n"));
    TEST_ASSERT_EQUAL_UINT(strlen(o), len);
    free(o);

    o = pm_build_client_head(&r, s, 0, NULL, &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "Connection: close\r\n\r\n"));
    free(o);
}

void test_build_client_head_chunked_drops_content_length(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 99\r\nTransfer-Encoding: chunked\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, parse(s));
    size_t len;
    char *o = pm_build_client_head(&r, s, 1, NULL, &len);
    TEST_ASSERT_NULL(strstr(o, "Content-Length"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Transfer-Encoding: chunked\r\n"));
    free(o);
}

void test_build_client_head_extra_header(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, parse(s));
    size_t len;
    char *o = pm_build_client_head(&r, s, 1, "X-Cache: MISS\r\n", &len);
    TEST_ASSERT_NOT_NULL(strstr(o, "X-Cache: MISS\r\nVia: 1.1 webserver\r\n"));
    TEST_ASSERT_EQUAL_UINT(strlen(o), len);
    free(o);
}

void test_build_stored_head(void) {
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nAge: 30\r\nConnection: close\r\n"
                    "Keep-Alive: timeout=5\r\nCache-Control: max-age=60\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(1, parse(s));
    size_t len;
    char *o = pm_build_stored_head(&r, s, &len);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT(strlen(o), len);
    TEST_ASSERT_EQUAL_INT(0, strncmp(o, "HTTP/1.1 200 OK\r\n", 17));
    TEST_ASSERT_NOT_NULL(strstr(o, "Content-Length: 5\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "Cache-Control: max-age=60\r\n"));
    TEST_ASSERT_NULL(strstr(o, "Age:"));          /* 꺼낼 때 새로 계산한다 */
    TEST_ASSERT_NULL(strstr(o, "Connection:"));   /* 클라이언트마다 다르게 붙인다 */
    TEST_ASSERT_NULL(strstr(o, "Keep-Alive"));
    TEST_ASSERT_NULL(strstr(o, "Via:"));
    TEST_ASSERT_EQUAL_INT(0, strcmp(o + len - 2, "\r\n"));      /* 마지막 헤더 줄바꿈으로 끝나고 */
    TEST_ASSERT_NULL(strstr(o, "\r\n\r\n"));                    /* 빈 줄은 없다 */
    free(o);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_build_client_head_extra_header);
    RUN_TEST(test_build_stored_head);
    RUN_TEST(test_resp_content_length);
    RUN_TEST(test_resp_needs_more);
    RUN_TEST(test_resp_without_reason_phrase);
    RUN_TEST(test_resp_http10);
    RUN_TEST(test_resp_chunked_overrides_content_length);
    RUN_TEST(test_resp_connection_close_detected);
    RUN_TEST(test_resp_rejects_malformed);
    RUN_TEST(test_resp_rejects_ambiguous_framing);
    RUN_TEST(test_body_modes);
    RUN_TEST(test_chunk_simple);
    RUN_TEST(test_chunk_multiple_with_extension_and_hex);
    RUN_TEST(test_chunk_trailers);
    RUN_TEST(test_chunk_stops_at_end_and_leaves_rest);
    RUN_TEST(test_chunk_byte_by_byte);
    RUN_TEST(test_chunk_split_inside_data);
    RUN_TEST(test_chunk_errors);
    RUN_TEST(test_build_request_basic);
    RUN_TEST(test_build_request_keepalive_flag);
    RUN_TEST(test_build_request_strip_prefix);
    RUN_TEST(test_build_request_appends_existing_xff);
    RUN_TEST(test_build_request_post_body);
    RUN_TEST(test_build_request_absolute_form_becomes_origin_form);
    RUN_TEST(test_build_request_http10_without_host);
    RUN_TEST(test_build_client_head);
    RUN_TEST(test_build_client_head_chunked_drops_content_length);
    return UNITY_END();
}
