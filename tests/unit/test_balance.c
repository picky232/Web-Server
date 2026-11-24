#include "balance.h"
#include "exchange.h"
#include "test.h"

#include <stdlib.h>

static struct pool *make(int n, enum policy policy, int max)
{
    struct pool *p = calloc(1, sizeof *p);
    p->n = n;
    p->policy = policy;
    p->max_inflight = max;
    return p;
}

static int pick(struct pool *p, unsigned tried, long long now)
{
    struct backend *b = pool_pick(p, tried, now);
    return b ? (int)(b - p->be) : -1;
}

static void pick_rules(void)
{
    struct pool *p = make(3, ROUND_ROBIN, 100);
    int seen[3] = {0};
    for (int i = 0; i < 30; i++)
        seen[pick(p, 0, 0)]++;
    CHECK(seen[0] == 10 && seen[1] == 10 && seen[2] == 10);
    CHECK(pick(p, 1 | 4, 0) == 1);      /* 써 본 것은 빼고 */
    CHECK(pick(p, 1 | 2 | 4, 0) == -1); /* 다 써 봤다 */
    free(p);

    p = make(3, LEAST_CONN, 100);
    p->be[0].inflight = 5;
    p->be[1].inflight = 2;
    p->be[2].inflight = 7;
    CHECK(pick(p, 0, 0) == 1 && p->be[1].inflight == 3);
    p->be[1].down = true;
    CHECK(pick(p, 0, 0) == 0);
    p->be[0].ejected_until = 1000;
    CHECK(pick(p, 0, 999) == 2);
    CHECK(pick(p, 0, 1000) == 0); /* 제외 시간이 끝나면 돌아온다 */
    p->be[0].down = p->be[2].down = true;
    CHECK(pick(p, 0, 5000) == 1); /* 전부 장애면 그래도 시도 (가장 한가한 것) */
    free(p);

    p = make(2, LEAST_CONN, 2); /* 동시 상한 */
    for (int i = 0; i < 4; i++)
        CHECK(pick(p, 0, 0) >= 0);
    CHECK(pick(p, 0, 0) == -1);
    p->be[1].down = true; /* 건강한 것이 꽉 찼다고 해서 죽은 것에 보내지 않는다 */
    p->be[1].inflight = 0;
    CHECK(pick(p, 0, 0) == -1);
    pool_done(p, &p->be[0], OUT_OK, 0);
    CHECK(pick(p, 0, 0) == 0);
    free(p);
}

static void exclude_rules(void)
{
    struct pool *p = make(4, LEAST_CONN, 1000);
    struct backend *b = p->be;
    CHECK(pool_done(p, &b[0], OUT_REFUSED, 0) && b[0].down); /* 거절은 바로 */
    CHECK(!pool_done(p, &b[0], OUT_REFUSED, 0));             /* 이미 빠졌으면 다시 알리지 않는다 */
    for (int i = 0; i < 2; i++)
        CHECK(!pool_done(p, &b[1], OUT_CONNECT_TIMEOUT, 0));
    CHECK(pool_done(p, &b[1], OUT_CONNECT_TIMEOUT, 0) && b[1].down);
    for (int i = 0; i < 20; i++) /* 절반이 이미 빠졌으니 더는 빼지 않는다 */
        CHECK(!pool_done(p, &b[2], OUT_BAD, 0));
    CHECK(backend_usable(&b[2], 0));
    free(p);

    p = make(2, LEAST_CONN, 1000);
    b = p->be;
    for (int i = 0; i < 4; i++)
        CHECK(!pool_done(p, &b[0], OUT_BAD, 100));
    pool_done(p, &b[0], OUT_OK, 100); /* 성공이 끼면 처음부터 다시 센다 */
    for (int i = 0; i < 4; i++)
        CHECK(!pool_done(p, &b[0], OUT_BAD, 100));
    CHECK(pool_done(p, &b[0], OUT_BAD, 100) && !backend_usable(&b[0], 101) && backend_usable(&b[0], 20000));
    for (int i = 0; i < 50; i++) /* 우리 탓, 504, 손님 이탈은 세지 않는다 */
        CHECK(!pool_done(p, &b[1], OUT_NEUTRAL, 0));
    CHECK(backend_usable(&b[1], 0));
    free(p);

    p = make(1, LEAST_CONN, 1000); /* 하나뿐이면 실패 응답으로 빼지 않는다 */
    for (int i = 0; i < 50; i++)
        CHECK(!pool_done(p, &p->be[0], OUT_BAD, 0));
    CHECK(backend_usable(&p->be[0], 0));
    free(p);
}

/* 백엔드로 보낼 요청 머리를 바이트 단위로 비교 */
static void request_head(void)
{
    const char *raw = "POST http://Ex.com:81/x HTTP/1.1\r\nHost: ignored\r\nConnection: A, close\r\nA: 1\r\nB: 2\r\n"
                      "X-Forwarded-For: 9.9.9.9\r\nX_Forwarded_For: 9.9.9.9\r\nExpect: 100-continue\r\nContent-Length: 3\r\nProxy-Connection: x\r\nVia: 1.0 p\r\n\r\n";
    struct exchange *ex = calloc(1, sizeof *ex);
    CHECK(http_parse_request(raw, strlen(raw), &ex->req) == 0);
    CHECK(uri_split(ex->req.target, ex->req.method, &ex->target) == 0);
    char path[] = "/x";
    ex->path = path;
    ex->body.size = 3;
    struct buf out;
    buf_init(&out, 4096);
    CHECK(upstream_request_head(ex, "1.2.3.4", "me", "be:1", &out));
    buf_append(&out, "", 1);
    CHECK_MSG(!strcmp(buf_head(&out), "POST /x HTTP/1.1\r\nHost: Ex.com:81\r\nB: 2\r\nVia: 1.0 p\r\nVia: 1.1 me\r\n"
                      "X-Forwarded-For: 1.2.3.4\r\nX-Forwarded-Proto: http\r\nX-Forwarded-Host: Ex.com:81\r\nContent-Length: 3\r\n\r\n"),
              buf_head(&out));
    buf_free(&out);
    raw = "GET /y?z HTTP/1.0\r\n\r\n"; /* 1.0은 Host가 없을 수 있다 */
    memset(ex, 0, sizeof *ex);
    CHECK(http_parse_request(raw, strlen(raw), &ex->req) == 0 && uri_split(ex->req.target, M_GET, &ex->target) == 0);
    char path2[] = "/y";
    ex->path = path2;
    CHECK(upstream_request_head(ex, "::1", "me", "be:1", &out));
    buf_append(&out, "", 1);
    CHECK(strstr(buf_head(&out), "GET /y?z HTTP/1.1\r\nHost: be:1\r\n") == buf_head(&out) && !strstr(buf_head(&out), "Content-Length"));
    buf_free(&out);
    struct buf tiny;
    buf_init(&tiny, 40);
    CHECK(!upstream_request_head(ex, "::1", "me", "be:1", &tiny)); /* 안 들어가면 실패 */
    buf_free(&tiny);
    free(ex);
}

void test_balance(void)
{
    pick_rules();
    exclude_rules();
    request_head();
}
