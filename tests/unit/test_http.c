#include "http.h"
#include "test.h"

#include <stdlib.h>

/* 머리를 끝까지 스캔한 뒤 해석한 결과. 스캔에서 걸리면 400 */
static int parse(const char *s, struct http_req *r)
{
    size_t pos = 0;
    enum scan st = http_scan_head(s, strlen(s), &pos);
    if (st != SCAN_END)
        return st == SCAN_BAD ? 400 : -1;
    return http_parse_request(s, pos, r);
}

#define H "Host: x\r\n"

void test_str(void)
{
    long long v;
    CHECK(sl_to_ll((struct sl){"123", 3}, &v) && v == 123);
    CHECK(sl_to_ll((struct sl){"999999999999999999", 18}, &v) && v == 999999999999999999LL);
    const char *bad[] = {"", "-1", "+1", " 1", "1 ", "0x1", "1a", "9999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(!sl_to_ll((struct sl){bad[i], strlen(bad[i])}, &v), bad[i]);
    struct sl l = {"a, Close ,b", 11};
    CHECK(sl_list_has(l, "close"));
    CHECK(sl_list_has(l, "b"));
    CHECK(!sl_list_has(l, "clos"));
    CHECK(!sl_list_has((struct sl){"", 0}, "a"));
}

static void requests(void)
{
    static const struct {
        const char *s;
        int want;
    } t[] = {
        {"GET / HTTP/1.1\r\n" H "\r\n", 0},
        {"GET / HTTP/1.0\r\n\r\n", 0}, /* 1.0은 Host가 없어도 된다 */
        {"GET / HTTP/1.7\r\n" H "\r\n", 0},
        {"GET http://x/a HTTP/1.1\r\n" H "\r\n", 0},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" H H "\r\n", 400},
        {"GET / HTTP/2.0\r\n" H "\r\n", 505},
        {"GET / HTTP/1.1x\r\n" H "\r\n", 400},
        {"GET / http/1.1\r\n" H "\r\n", 400},
        {"GET  / HTTP/1.1\r\n" H "\r\n", 400},
        {"GET /\r\n" H "\r\n", 400},
        {"G@T / HTTP/1.1\r\n" H "\r\n", 400},   /* 글자부터 틀린 메서드 */
        {"BREW / HTTP/1.1\r\n" H "\r\n", 501},  /* 모르는 메서드 */
        {"get / HTTP/1.1\r\n" H "\r\n", 501},
        {"TRACE / HTTP/1.1\r\n" H "\r\n", 501},
        {"GET / HTTP/1.1\r\n" H "Bad Name: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" H "X: a\r\n b\r\n\r\n", 400}, /* 줄 이어 쓰기 */
        {"GET / HTTP/1.1\r\n" H "NoColon\r\n\r\n", 400},
        {"GET / HTTP/1.1\n" H "\r\n", 400},                 /* 홑 LF */
        {"GET / HTTP/1.1\r\nHost: x\ry\r\n\r\n", 400},      /* 홑 CR */
        {"GET /\x01 HTTP/1.1\r\n" H "\r\n", 400},
        {"GET /\xc3\xa9 HTTP/1.1\r\n" H "\r\n", 400},
        {"\x16\x03\x01", 400}, /* TLS 첫 바이트 */
        {"POST / HTTP/1.1\r\n" H "Content-Length: 5\r\n\r\n", 0},
        {"POST / HTTP/1.1\r\n" H "Content-Length: -5\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Content-Length: 5x\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Content-Length:\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Content-Length: 99999999999999999999\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Content-Length: 5\r\nContent-Length: 5\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: Chunked\r\n\r\n", 0},
        {"POST / HTTP/1.0\r\n" H "Transfer-Encoding: chunked\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: gzip\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: chunked, gzip\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: xchunked\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
    };
    struct http_req *r = malloc(sizeof *r);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        CHECK_MSG(parse(t[i].s, r) == t[i].want, t[i].s);

    CHECK(parse("GET /a?b HTTP/1.1\r\nhOsT:  Ex.com \r\nConnection: x, Close\r\n\r\n", r) == 0);
    CHECK(sl_eq(r->target, "/a?b") && sl_eq(r->host, "Ex.com") && !r->keep && r->minor == 1);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", r) == 0 && r->keep);
    CHECK(parse("GET / HTTP/1.1\r\n" H "Connection: a\r\nConnection: close\r\n\r\n", r) == 0 && !r->keep);
    CHECK(parse("POST / HTTP/1.1\r\n" H "Expect: 100-Continue\r\n\r\n", r) == 0 && r->expect_100);
    CHECK(parse("POST / HTTP/1.1\r\n" H "Expect: other\r\n\r\n", r) == 0 && r->expect_other);

    char big[HTTP_HEAD_MAX];
    int n = snprintf(big, sizeof big, "GET / HTTP/1.1\r\n" H);
    for (int i = 0; i < HTTP_MAX_FIELDS; i++)
        n += snprintf(big + n, sizeof big - (size_t)n, "X%d: y\r\n", i);
    snprintf(big + n, sizeof big - (size_t)n, "\r\n");
    CHECK(parse(big, r) == 431);
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /", 5);
    snprintf(big + HTTP_URI_MAX + 100, 64, " HTTP/1.1\r\n" H "\r\n");
    CHECK(parse(big, r) == 414);
    free(r);
}

/* TCP는 어디서든 끊어 준다. 모든 위치에서 잘라 넣어도 결과가 같아야 한다 */
static void every_split(void)
{
    const char *s = "POST /a HTTP/1.1\r\nHost: x\r\nX-Long:  value here \r\nContent-Length: 3\r\n\r\nabc";
    size_t total = strlen(s), head = total - 3;
    for (size_t cut = 0; cut <= total; cut++) {
        size_t pos = 0;
        enum scan st = http_scan_head(s, cut, &pos);
        if (st != SCAN_END)
            st = http_scan_head(s, total, &pos);
        CHECK(st == SCAN_END && pos == head);
    }
    size_t pos = 0;
    enum scan st = SCAN_MORE;
    size_t i = 0;
    while (st == SCAN_MORE && i < total)
        st = http_scan_head(s, ++i, &pos);
    CHECK(st == SCAN_END && pos == head && i == head);
}

static bool res(const char *s, struct http_res *r)
{
    size_t pos = 0;
    return http_scan_head(s, strlen(s), &pos) == SCAN_END && http_parse_response(s, pos, r);
}

static void responses(void)
{
    struct http_res *r = malloc(sizeof *r);
    CHECK(res("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n", r) && r->status == 200 && r->length == 3 && r->keep);
    CHECK(res("HTTP/1.1 204\r\n\r\n", r) && r->status == 204);
    CHECK(res("HTTP/1.1 404 Not Found At All\r\n\r\n", r) && sl_eq(r->reason, "Not Found At All"));
    CHECK(res("HTTP/1.0 200 OK\r\n\r\n", r) && !r->keep && r->length == -1);
    /* 둘 다 있으면 Transfer-Encoding이 이긴다 */
    CHECK(res("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n", r) && r->chunked && r->length == -1);
    CHECK(!res("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", r));
    CHECK(!res("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", r));
    CHECK(res("HTTP/1.1 200 OK\r\nX-A : b\r\n\r\n", r) && fields_find(&r->h, "x-a"));
    CHECK(res("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n", r) && r->length == 3);
    const char *bad[] = {"HTTP/1.1 20 OK\r\n\r\n", "HTTP/2.0 200 OK\r\n\r\n", "HTTP/1.1 2x0 OK\r\n\r\n",
                         "HTTP/1.1 200OK\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n",
                         "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", "HTTP/1.1 200 OK\r\n x\r\n\r\n",
                         "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", "hello\r\n\r\n", "HTTP/1.1 099 x\r\n\r\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(!res(bad[i], r), bad[i]);
    CHECK(res("HTTP/1.1 200 OK\r\nConnection: X-Secret, close\r\nX-Secret: 1\r\nX-Ok: 2\r\n\r\n", r));
    CHECK(fields_hop_by_hop(&r->h, (struct sl){"x-secret", 8}));
    CHECK(fields_hop_by_hop(&r->h, (struct sl){"Transfer-Encoding", 17}));
    CHECK(!fields_hop_by_hop(&r->h, (struct sl){"X-Ok", 4}));
    free(r);
}

void test_http(void)
{
    requests();
    every_split();
    responses();
}
