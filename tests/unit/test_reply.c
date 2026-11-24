#include "reply.h"
#include "spool.h"
#include "test.h"

#include <stdlib.h>

static char *text(struct buf *b)
{
    buf_append(b, "", 1);
    return buf_head(b);
}

void test_reply(void)
{
    struct buf out;
    buf_init(&out, 100000);

    struct reply r = {.minor = 1, .keep = true};
    CHECK(reply_start(&r, &out, 200, 5, "X: y\r\n", 6, 0));
    reply_write(&r, &out, "hello world", 11); /* 약속은 5바이트 */
    CHECK(reply_finish(&r, &out) && r.sent == 5);
    char *s = text(&out);
    CHECK(strstr(s, "HTTP/1.1 200 OK\r\n") == s && strstr(s, "Content-Length: 5\r\n") && strstr(s, "X: y\r\n"));
    CHECK(strstr(s, "Connection: keep-alive\r\n") && strstr(s, "Date: ") && strstr(s, "Server: "));
    CHECK(!strcmp(strstr(s, "\r\n\r\n") + 4, "hello"));

    buf_free(&out); /* 약속보다 적게 보냄 */
    r = (struct reply){.minor = 1, .keep = true};
    reply_start(&r, &out, 200, 5, "", 0, 0);
    reply_write(&r, &out, "abc", 3);
    CHECK(!reply_finish(&r, &out));

    buf_free(&out); /* 길이를 모르면 1.1은 chunked */
    r = (struct reply){.minor = 1, .keep = true};
    reply_start(&r, &out, 200, -1, "", 0, 0);
    reply_write(&r, &out, "abc", 3);
    reply_write(&r, &out, "", 0); /* 빈 조각은 끝이라는 뜻이라 쓰면 안 된다 */
    reply_write(&r, &out, "de", 2);
    CHECK(reply_finish(&r, &out));
    s = text(&out);
    CHECK(strstr(s, "Transfer-Encoding: chunked") && !strstr(s, "Content-Length"));
    CHECK(!strcmp(strstr(s, "\r\n\r\n") + 4, "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"));

    buf_free(&out); /* 1.0에는 chunked를 보내면 안 된다. 닫힘이 끝 */
    r = (struct reply){.minor = 0, .keep = true};
    reply_start(&r, &out, 200, -1, "", 0, 0);
    reply_write(&r, &out, "abc", 3);
    CHECK(reply_finish(&r, &out) && !r.keep);
    s = text(&out);
    CHECK(!strstr(s, "chunked") && strstr(s, "Connection: close") && !strcmp(strstr(s, "\r\n\r\n") + 4, "abc"));

    buf_free(&out); /* HEAD: 머리는 GET과 같고 본문만 없다 */
    r = (struct reply){.minor = 1, .keep = true, .head_only = true};
    reply_start(&r, &out, 200, 5, "", 0, 0);
    reply_write(&r, &out, "hello", 5);
    CHECK(reply_finish(&r, &out));
    s = text(&out);
    CHECK(strstr(s, "Content-Length: 5") && !strcmp(strstr(s, "\r\n\r\n") + 4, ""));

    int no_body[] = {204, 304};
    for (int i = 0; i < 2; i++) {
        buf_free(&out);
        r = (struct reply){.minor = 1, .keep = true};
        reply_start(&r, &out, no_body[i], 5, "", 0, 0);
        reply_write(&r, &out, "hello", 5);
        CHECK(reply_finish(&r, &out));
        s = text(&out);
        CHECK(!strstr(s, "Content-Length") && !strstr(s, "chunked") && !strcmp(strstr(s, "\r\n\r\n") + 4, ""));
    }

    buf_free(&out); /* 쌓인 만큼 자리가 줄어든다 */
    r = (struct reply){.minor = 1};
    reply_start(&r, &out, 200, 1000000, "", 0, 0);
    size_t room = reply_room(&r, &out);
    char *big = calloc(1, room);
    reply_write(&r, &out, big, room);
    CHECK(room > 60000 && reply_room(&r, &out) == 0);
    free(big);

    struct buf small; /* 메모리(여기서는 자리)가 모자라 일부를 못 썼으면 끝까지 보낸 척하지 않는다 */
    buf_init(&small, 400);
    r = (struct reply){.minor = 1, .keep = true};
    CHECK(reply_start(&r, &small, 200, 1000, "", 0, 0));
    char thousand[1000] = {0};
    reply_write(&r, &small, thousand, 1000);
    CHECK(!reply_finish(&r, &small));
    buf_free(&small);

    buf_free(&out); /* 머리가 안 들어가면 실패하고 아무것도 쓰지 않는다 */
    struct buf tiny;
    buf_init(&tiny, 50);
    r = (struct reply){.minor = 1};
    CHECK(!reply_start(&r, &tiny, 200, 5, "", 0, 0) && !r.started && buf_len(&tiny) == 0);
    buf_free(&tiny);
}

void test_spool(void)
{
    atomic_llong disk = 0;
    struct spool s;
    char a[40000], b[90000];
    memset(a, 'a', sizeof a);
    spool_init(&s, &disk);
    CHECK(spool_append(&s, a, sizeof a, 1000000) == 0 && s.fd < 0 && disk == 0);
    memset(a, 'b', sizeof a);
    CHECK(spool_append(&s, a, sizeof a, 1000000) == 0 && s.fd >= 0 && disk == 80000); /* 64KB를 넘어 파일로 */
    CHECK(spool_read(&s, 39999, b, 2) == 2 && b[0] == 'a' && b[1] == 'b');
    CHECK(spool_read(&s, 0, b, sizeof b) == 80000);
    CHECK(spool_append(&s, a, sizeof a, 100000) == 503 && disk == 80000); /* 서버 전체 한도 */
    spool_free(&s);
    CHECK(disk == 0);

    spool_init(&s, &disk);
    CHECK(spool_append(&s, "xyz", 3, 1000) == 0 && spool_read(&s, 1, b, 10) == 2 && b[0] == 'y');
    spool_free(&s);
}
