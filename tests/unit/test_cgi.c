#include "cgi.h"
#include "exchange.h"
#include "test.h"

#include <stdlib.h>

static bool head(const char *s, int *status, long long *len, struct buf *x)
{
    buf_free(x);
    size_t n = cgi_head_end(s, strlen(s));
    return n && cgi_parse_head(s, n, status, len, x);
}

void test_cgi(void)
{
    CHECK(cgi_head_end("A: b\n\nx", 8) == 6);
    CHECK(cgi_head_end("A: b\r\n\r\nx", 9) == 8);
    CHECK(cgi_head_end("A: b\r\n", 6) == 0);
    CHECK(cgi_head_end("", 0) == 0);
    int st;
    long long len;
    struct buf x;
    buf_init(&x, 4096);
    CHECK(head("Content-Type: a\n\n", &st, &len, &x) && st == 200 && len == -1);
    CHECK(head("Status: 404 Nope\r\nContent-Length: 7\r\n\r\n", &st, &len, &x) && st == 404 && len == 7);
    CHECK(head("Location: /x\n\n", &st, &len, &x) && st == 302);
    CHECK(head("X: 1\nConnection: close\nTransfer-Encoding: chunked\n\n", &st, &len, &x));
    CHECK(buf_len(&x) == 6 && !memcmp(buf_head(&x), "X: 1\r\n", 6)); /* 연결에 관한 헤더는 서버가 정한다 */
    const char *bad[] = {"no colon\n\n", "Bad Name: x\n\n", "Status: 99\n\n", "Status: x\n\n", "Status: 100 Continue\n\n",
                         "Content-Length: -1\n\n", "X: a\x01\n\n", "\n\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(!head(bad[i], &st, &len, &x), bad[i]);
    buf_free(&x);

    const char *raw = "POST http://good/cgi/a HTTP/1.1\r\nHost: evil\r\nX-M: 1\r\nCookie: a=1\r\nx-m: 2\r\nCookie: b=2\r\nContent-Type: t/p\r\nProxy: evil\r\nAuthorization: s\r\nX-Y: 1\r\nX_Z: 2\r\nConnection: Q\r\nQ: 3\r\n\r\n";
    struct exchange *ex = calloc(1, sizeof *ex);
    CHECK(http_parse_request(raw, strlen(raw), &ex->req) == 0);
    CHECK(uri_split(ex->req.target, M_POST, &ex->target) == 0);
    ex->target.query = (struct sl){"?a=b", 4};
    ex->body.size = 9;
    CHECK(cgi_env(ex, "1.2.3.4", 80, "/cgi/a", "/i", &x));
    char joined[4096] = "";
    for (char *p = buf_head(&x); p < buf_head(&x) + buf_len(&x); p += strlen(p) + 1) {
        strcat(joined, p);
        strcat(joined, "\n");
    }
    const char *want[] = {"REQUEST_METHOD=POST\n", "QUERY_STRING=a=b\n", "CONTENT_LENGTH=9\n", "CONTENT_TYPE=t/p\n", "PATH_INFO=/i\n",
                          "HTTP_X_Y=1\n", "HTTP_HOST=good\n", "HTTP_X_M=1, 2\n", "HTTP_COOKIE=a=1; b=2\n", "REMOTE_ADDR=1.2.3.4\n", "SERVER_PORT=80\n"};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK_MSG(strstr(joined, want[i]), want[i]);
    const char *gone[] = {"HTTP_PROXY", "AUTHORIZATION", "X_Z", "HTTP_Q", "HTTP_CONNECTION"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++)
        CHECK_MSG(!strstr(joined, gone[i]), gone[i]);
    buf_free(&x);
    free(ex);
}
