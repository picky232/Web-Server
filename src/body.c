#include "body.h"

#include <limits.h>

#define CHUNK_LINE_MAX 256  /* 조각 크기 줄 하나의 상한 (확장을 끝없이 보내는 공격 차단) */
#define TRAILER_MAX 8192

/* chunked 한 조각: "1a3;확장\r\n" + 데이터 + "\r\n", 마지막은 "0\r\n" + 트레일러들 + "\r\n" */
enum { C_SIZE, C_SIZE_MORE, C_EXT, C_SIZE_LF, C_DATA, C_DATA_CR, C_DATA_LF, C_TRAIL, C_TRAIL_LINE,
       C_TRAIL_LF, C_END_LF };

void body_init(struct body_dec *d, enum body_kind kind, long long length)
{
    *d = (struct body_dec){.kind = kind, .left = kind == BODY_LENGTH ? length : 0};
    d->done = kind == BODY_NONE || (kind == BODY_LENGTH && length == 0);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* 데이터가 아닌 글자 하나. false = 형식 오류 */
static bool chunk_char(struct body_dec *d, char ch)
{
    if (++d->line > (d->state >= C_TRAIL ? TRAILER_MAX : CHUNK_LINE_MAX))
        return false;
    int h;
    switch (d->state) {
    case C_SIZE:
        d->left = 0;
        /* fall through */
    case C_SIZE_MORE:
        if ((h = hexval(ch)) >= 0) {
            if (d->left > (LLONG_MAX >> 4)) /* 아주 큰 16진수로 넘치게 하는 공격 */
                return false;
            d->left = d->left * 16 + h;
            d->state = C_SIZE_MORE;
            return true;
        }
        if (d->state == C_SIZE) /* 숫자가 하나도 없다 */
            return false;
        if (ch == ';' || ch == ' ' || ch == '\t')
            d->state = C_EXT;
        else if (ch == '\r')
            d->state = C_SIZE_LF;
        else
            return false;
        return true;
    case C_EXT: /* 모르는 확장은 무시한다 */
        if (ch == '\r')
            d->state = C_SIZE_LF;
        else if (ch == '\n')
            return false;
        return true;
    case C_SIZE_LF:
        if (ch != '\n')
            return false;
        d->line = 0;
        d->state = d->left ? C_DATA : C_TRAIL;
        return true;
    case C_DATA_CR:
        d->state = C_DATA_LF;
        return ch == '\r';
    case C_DATA_LF:
        d->line = 0;
        d->state = C_SIZE;
        return ch == '\n';
    case C_TRAIL: /* 트레일러는 읽어서 버린다 */
        d->state = ch == '\r' ? C_END_LF : C_TRAIL_LINE;
        return ch != '\n';
    case C_TRAIL_LINE:
        if (ch == '\r')
            d->state = C_TRAIL_LF;
        return ch != '\n';
    case C_TRAIL_LF:
        d->state = C_TRAIL;
        return ch == '\n';
    case C_END_LF:
        d->done = true;
        return ch == '\n';
    }
    return false;
}

long body_next(struct body_dec *d, const char *p, size_t n, struct sl *out)
{
    *out = (struct sl){p, 0};
    if (d->done)
        return 0;
    if (d->kind == BODY_EOF) {
        out->n = n;
        return (long)n;
    }
    size_t i = 0;
    if (d->kind == BODY_CHUNKED) {
        while (i < n && d->state != C_DATA && !d->done)
            if (!chunk_char(d, p[i++]))
                return -1;
        if (d->done || d->state != C_DATA)
            return (long)i;
    }
    size_t take = n - i;
    if ((long long)take > d->left)
        take = (size_t)d->left;
    *out = (struct sl){p + i, take};
    d->left -= (long long)take;
    if (d->left == 0) {
        if (d->kind == BODY_CHUNKED)
            d->state = C_DATA_CR;
        else
            d->done = true;
    }
    return (long)(i + take);
}
