#ifndef BODY_H
#define BODY_H

#include "str.h"

/* 본문이 어디서 끝나는지 정하는 방법 */
enum body_kind {
    BODY_NONE,
    BODY_LENGTH,  /* Content-Length만큼 */
    BODY_CHUNKED, /* 조각마다 크기를 적고, 크기 0인 조각으로 끝냄 */
    BODY_EOF,     /* 상대가 연결을 닫을 때까지 (응답만) */
};

/* 점진 해독기. 어느 바이트에서 끊겨 와도 이어서 푼다 */
struct body_dec {
    enum body_kind kind;
    long long left; /* 지금 조각(또는 본문)에서 남은 바이트 */
    int state;
    unsigned line; /* 조각 크기 줄·트레일러 길이 상한 검사용 */
    bool done;
};

void body_init(struct body_dec *d, enum body_kind kind, long long length);

/* p[0..n)에서 앞부분을 소비하고 그 바이트 수를 돌려준다. 형식 오류면 -1.
   본문 바이트를 만나면 *out이 p 안의 그 구간을 가리킨다 (복사 없음). 없으면 out->n = 0 */
long body_next(struct body_dec *d, const char *p, size_t n, struct sl *out);

#endif
