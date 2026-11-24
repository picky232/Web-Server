#include "body.h"
#include "test.h"

/* 입력을 cut 위치에서 둘로 잘라 넣고, 나온 본문을 모은다. 반환: 소비한 총 바이트, 오류면 -1 */
static long run(enum body_kind k, long long len, const char *s, size_t n, size_t cut, char *out, size_t *on, bool *done)
{
    struct body_dec d;
    body_init(&d, k, len);
    size_t pos = 0;
    *on = 0;
    for (int part = 0; part < 2; part++) {
        size_t end = part == 0 ? cut : n;
        while (pos < end && !d.done) {
            struct sl data;
            long used = body_next(&d, s + pos, end - pos, &data);
            if (used < 0)
                return -1;
            memcpy(out + *on, data.p, data.n);
            *on += data.n;
            pos += (size_t)used;
        }
    }
    *done = d.done;
    return (long)pos;
}

void test_body(void)
{
    char out[256];
    size_t on;
    bool done;

    /* 길이 방식: 뒤에 붙은 다음 요청은 건드리지 않는다 */
    CHECK(run(BODY_LENGTH, 3, "abcGET", 6, 2, out, &on, &done) == 3 && done && on == 3 && !memcmp(out, "abc", 3));
    CHECK(run(BODY_LENGTH, 0, "GET", 3, 0, out, &on, &done) == 0 && done);
    CHECK(run(BODY_LENGTH, 5, "abc", 3, 1, out, &on, &done) == 3 && !done);
    CHECK(run(BODY_EOF, 0, "abc", 3, 1, out, &on, &done) == 3 && !done && on == 3);

    /* chunked: 모든 위치에서 잘라도 결과가 같아야 한다 */
    const char *s = "4\r\nWiki\r\n6;ext=1\r\npedia \r\nB\r\nin chunks!!\r\n0\r\nX-T: 1\r\n\r\nNEXT";
    size_t n = strlen(s);
    for (size_t cut = 0; cut <= n; cut++) {
        CHECK(run(BODY_CHUNKED, 0, s, n, cut, out, &on, &done) == (long)n - 4);
        CHECK(done && on == 21 && !memcmp(out, "Wikipedia in chunks!!", 21));
    }
    const char *bad[] = {"\r\n", "x\r\n", "4\nWiki", "4\r\nWikiX", "4\r\nWiki\rX", "4 x\n", "-4\r\n", "0x4\r\n",
                         "fffffffffffffffff\r\n", "0\r\nX\n\r\n", "4\rX", "0\r\n\rX"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(run(BODY_CHUNKED, 0, bad[i], strlen(bad[i]), 0, out, &on, &done) == -1, bad[i]);
    /* 조각 확장을 끝없이 보내는 공격 */
    char ext[400];
    memset(ext, 'a', sizeof ext);
    memcpy(ext, "4;", 2);
    CHECK(run(BODY_CHUNKED, 0, ext, sizeof ext, 0, out, &on, &done) == -1);
    CHECK(run(BODY_CHUNKED, 0, "0\r\n\r\n", 5, 3, out, &on, &done) == 5 && done && on == 0);
}
