#include "unity.h"
#include "http_parser.h"

#include <stdio.h>
#include <string.h>

/* 테스트용 작은 한도: head 1024, uri 128, body 64 */
#define MAX_HEAD 1024
#define MAX_URI  128
#define MAX_BODY 64

static hp_parser p;

void setUp(void)    { hp_init(&p, MAX_HEAD, MAX_URI, MAX_BODY); }
void tearDown(void) {}

static hp_status feed_all(const char *s) { return hp_feed(&p, s, strlen(s)); }

static void assert_error(const char *req, int status) {
    TEST_ASSERT_EQUAL_INT(HP_ERROR, feed_all(req));
    TEST_ASSERT_EQUAL_INT(status, p.err_status);
}

/* ---------------------------- 정상 케이스 ---------------------------- */

void test_simple_get(void) {
    const char *r = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all(r));
    TEST_ASSERT_EQUAL_INT(HP_GET, p.method);
    TEST_ASSERT_EQUAL_UINT(11, p.uri_len);
    TEST_ASSERT_EQUAL_STRING_LEN("/index.html", r + p.uri_off, p.uri_len);
    TEST_ASSERT_EQUAL_INT(1, p.version_minor);
    TEST_ASSERT_EQUAL_INT(1, p.keep_alive);
    TEST_ASSERT_EQUAL_UINT(strlen(r), p.total_len);
}

void test_header_lookup_case_insensitive_and_trimmed(void) {
    const char *r = "GET / HTTP/1.1\r\nhOsT:   example.com \t\r\nX-A: 1\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all(r));
    size_t n = 0;
    const char *v = hp_header_get(&p, r, "Host", &n);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING_LEN("example.com", v, n);
    TEST_ASSERT_NULL(hp_header_get(&p, r, "Missing", &n));
}

void test_head_method(void) {
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(HP_HEAD, p.method);
}

void test_leading_crlf_is_ignored(void) {
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all("\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"));
}

void test_absolute_form_uri_accepted(void) {
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all("GET http://a.com/x HTTP/1.1\r\nHost: a.com\r\n\r\n"));
}

void test_http10_without_host_ok(void) {
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all("GET / HTTP/1.0\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(0, p.version_minor);
}

/* --------------------------- keep-alive 판정 -------------------------- */

void test_keepalive_defaults(void) {
    feed_all("GET / HTTP/1.0\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(0, p.keep_alive);

    hp_init(&p, MAX_HEAD, MAX_URI, MAX_BODY);
    feed_all("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(1, p.keep_alive);

    hp_init(&p, MAX_HEAD, MAX_URI, MAX_BODY);
    feed_all("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(0, p.keep_alive);

    hp_init(&p, MAX_HEAD, MAX_URI, MAX_BODY);
    feed_all("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, Close\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(0, p.keep_alive);
}

/* ------------------------------ 본문 처리 ----------------------------- */

void test_post_with_body_complete(void) {
    const char *r = "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, feed_all(r));
    TEST_ASSERT_EQUAL_UINT(5, p.content_length);
    TEST_ASSERT_EQUAL_UINT(strlen(r) - 5, p.header_end);
    TEST_ASSERT_EQUAL_UINT(strlen(r), p.total_len);
}

void test_post_waits_for_body(void) {
    const char *r = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel";
    TEST_ASSERT_EQUAL_INT(HP_NEED_MORE, feed_all(r));
    const char *full = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, hp_feed(&p, full, strlen(full)));
}

void test_pipelined_requests_report_first_length(void) {
    const char *a = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *b = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    char buf[256];
    snprintf(buf, sizeof buf, "%s%s", a, b);
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, hp_feed(&p, buf, strlen(buf)));
    TEST_ASSERT_EQUAL_UINT(strlen(a), p.total_len);

    /* 남은 바이트로 두 번째 요청 파싱 */
    hp_init(&p, MAX_HEAD, MAX_URI, MAX_BODY);
    const char *rest = buf + strlen(a);
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, hp_feed(&p, rest, strlen(rest)));
    TEST_ASSERT_EQUAL_STRING_LEN("/b", rest + p.uri_off, p.uri_len);
}

/* --------------------------- 증분(분할) 입력 -------------------------- */

void test_byte_by_byte_feeding(void) {
    const char *r = "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
    size_t n = strlen(r);
    for (size_t i = 1; i < n; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(HP_NEED_MORE, hp_feed(&p, r, i), "prefix must need more");
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, hp_feed(&p, r, n));
    TEST_ASSERT_EQUAL_STRING_LEN("/p", r + p.uri_off, p.uri_len);
    TEST_ASSERT_EQUAL_UINT(3, p.content_length);
}

void test_split_inside_crlf(void) {
    const char *r = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t cut = strlen("GET / HTTP/1.1\r");  /* CR 과 LF 사이 */
    TEST_ASSERT_EQUAL_INT(HP_NEED_MORE, hp_feed(&p, r, cut));
    TEST_ASSERT_EQUAL_INT(HP_COMPLETE, hp_feed(&p, r, strlen(r)));
}

/* ------------------------------ 오류 케이스 --------------------------- */

void test_bare_lf_rejected(void)               { assert_error("GET / HTTP/1.1\nHost: x\n\n", 400); }
void test_missing_host_http11(void)            { assert_error("GET / HTTP/1.1\r\n\r\n", 400); }
void test_duplicate_host(void)                 { assert_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400); }
void test_space_before_colon(void)             { assert_error("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400); }
void test_obs_fold_rejected(void)              { assert_error("GET / HTTP/1.1\r\nHost: x\r\n y\r\n\r\n", 400); }
void test_header_without_colon(void)           { assert_error("GET / HTTP/1.1\r\nHost x\r\n\r\n", 400); }
void test_control_char_in_value(void)          { assert_error("GET / HTTP/1.1\r\nHost: x\x01y\r\n\r\n", 400); }
void test_missing_version(void)                { assert_error("GET /\r\n\r\n", 400); }
void test_extra_space_in_request_line(void)    { assert_error("GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400); }
void test_malformed_version(void)              { assert_error("GET / HTTP/1\r\nHost: x\r\n\r\n", 400); }
void test_lowercase_http_version(void)         { assert_error("GET / http/1.1\r\nHost: x\r\n\r\n", 400); }
void test_invalid_method_chars(void)           { assert_error("G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400); }
void test_uri_not_origin_or_absolute(void)     { assert_error("GET index.html HTTP/1.1\r\nHost: x\r\n\r\n", 400); }

void test_unsupported_version(void)            { assert_error("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505); }
void test_unsupported_minor_version(void)      { assert_error("GET / HTTP/1.2\r\nHost: x\r\n\r\n", 505); }
void test_unsupported_method(void)             { assert_error("DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", 501); }
void test_lowercase_method_unsupported(void)   { assert_error("get / HTTP/1.1\r\nHost: x\r\n\r\n", 501); }

void test_post_without_content_length(void)    { assert_error("POST / HTTP/1.1\r\nHost: x\r\n\r\n", 411); }
void test_duplicate_content_length(void)       { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na", 400); }
void test_content_length_not_digits(void)      { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n", 400); }
void test_content_length_negative(void)        { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400); }
void test_content_length_empty(void)           { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: \r\n\r\n", 400); }
void test_content_length_too_big(void)         { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 65\r\n\r\n", 413); }
void test_content_length_overflow(void)        { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999999\r\n\r\n", 413); }
void test_chunked_not_supported(void)          { assert_error("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501); }
void test_te_and_cl_together(void)             { assert_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\na", 400); }

/* ------------------------------ 크기 한도 ----------------------------- */

void test_uri_too_long_complete_line(void) {
    char r[512];
    char path[300];
    memset(path, 'a', 200); path[200] = '\0';
    snprintf(r, sizeof r, "GET /%s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    assert_error(r, 414);
}

void test_uri_too_long_without_newline(void) {
    char r[400];
    memset(r, 'a', sizeof r - 1);
    memcpy(r, "GET /", 5);
    r[sizeof r - 1] = '\0';
    assert_error(r, 414);  /* 줄이 끝나기 전에도 메모리 폭주를 막아야 한다 */
}

void test_header_section_too_big(void) {
    char r[MAX_HEAD + 512];
    int n = snprintf(r, sizeof r, "GET / HTTP/1.1\r\nHost: x\r\nX: ");
    memset(r + n, 'a', MAX_HEAD + 100);
    r[n + MAX_HEAD + 100] = '\0';
    assert_error(r, 431);
}

void test_too_many_headers(void) {
    char r[4096];
    int n = snprintf(r, sizeof r, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < HP_MAX_HEADERS + 1; i++) n += snprintf(r + n, sizeof r - (size_t)n, "A: b\r\n");
    snprintf(r + n, sizeof r - (size_t)n, "\r\n");
    hp_init(&p, 8192, MAX_URI, MAX_BODY);
    assert_error(r, 431);
}

void test_error_state_is_sticky(void) {
    TEST_ASSERT_EQUAL_INT(HP_ERROR, feed_all("GET / HTTP/9.9\r\n"));
    TEST_ASSERT_EQUAL_INT(HP_ERROR, feed_all("GET / HTTP/9.9\r\nHost: x\r\n\r\n"));
    TEST_ASSERT_EQUAL_INT(505, p.err_status);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_get);
    RUN_TEST(test_header_lookup_case_insensitive_and_trimmed);
    RUN_TEST(test_head_method);
    RUN_TEST(test_leading_crlf_is_ignored);
    RUN_TEST(test_absolute_form_uri_accepted);
    RUN_TEST(test_http10_without_host_ok);
    RUN_TEST(test_keepalive_defaults);
    RUN_TEST(test_post_with_body_complete);
    RUN_TEST(test_post_waits_for_body);
    RUN_TEST(test_pipelined_requests_report_first_length);
    RUN_TEST(test_byte_by_byte_feeding);
    RUN_TEST(test_split_inside_crlf);
    RUN_TEST(test_bare_lf_rejected);
    RUN_TEST(test_missing_host_http11);
    RUN_TEST(test_duplicate_host);
    RUN_TEST(test_space_before_colon);
    RUN_TEST(test_obs_fold_rejected);
    RUN_TEST(test_header_without_colon);
    RUN_TEST(test_control_char_in_value);
    RUN_TEST(test_missing_version);
    RUN_TEST(test_extra_space_in_request_line);
    RUN_TEST(test_malformed_version);
    RUN_TEST(test_lowercase_http_version);
    RUN_TEST(test_invalid_method_chars);
    RUN_TEST(test_uri_not_origin_or_absolute);
    RUN_TEST(test_unsupported_version);
    RUN_TEST(test_unsupported_minor_version);
    RUN_TEST(test_unsupported_method);
    RUN_TEST(test_lowercase_method_unsupported);
    RUN_TEST(test_post_without_content_length);
    RUN_TEST(test_duplicate_content_length);
    RUN_TEST(test_content_length_not_digits);
    RUN_TEST(test_content_length_negative);
    RUN_TEST(test_content_length_empty);
    RUN_TEST(test_content_length_too_big);
    RUN_TEST(test_content_length_overflow);
    RUN_TEST(test_chunked_not_supported);
    RUN_TEST(test_te_and_cl_together);
    RUN_TEST(test_uri_too_long_complete_line);
    RUN_TEST(test_uri_too_long_without_newline);
    RUN_TEST(test_header_section_too_big);
    RUN_TEST(test_too_many_headers);
    RUN_TEST(test_error_state_is_sticky);
    return UNITY_END();
}
