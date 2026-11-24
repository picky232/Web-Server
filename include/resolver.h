#ifndef RESOLVER_H
#define RESOLVER_H

#include "net.h"

struct conn;
struct loop;

#define RESOLVE_MAX 4

/* 이름 해석(DNS)은 기다리게 만드는 함수밖에 없다. 루프에서 부르면 그 루프의 모든 손님이 멈추므로
   별도 스레드에 맡기고, 끝나면 루프에 쪽지로 돌려준다 */
struct resolve {
    char host[256], port[8];
    struct loop *loop;
    struct conn *c; /* 루프 스레드만 만진다. 그 사이 손님이 떠나면 NULL */
    bool ok;
    struct addr addr[RESOLVE_MAX];
    int n;
    struct resolve *next;
};

int resolver_start(int threads);
struct resolve *resolver_ask(struct loop *l, struct conn *c, const char *host, int port); /* 밀려 있으면 NULL */
void resolver_stop(void);

#endif
