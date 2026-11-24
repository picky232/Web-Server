#ifndef UPSTREAM_H
#define UPSTREAM_H

#include "balance.h"
#include "body.h"
#include "buf.h"
#include "ev.h"
#include "resolver.h"

struct conn;
struct loop;

/* 백엔드와의 연결 하나 */
struct upconn {
    struct io io; /* 맨 앞 */
    struct loop *loop;
    struct conn *c; /* 지금 이 연결을 쓰는 클라이언트. 보관함에서 쉬는 중이면 NULL */
    struct backend *be;
    struct buf in, out;
    bool reused; /* 보관함에서 꺼낸 연결. 막 닫히는 중일 수 있다 */
    long long idle_since;
    struct upconn *prev, *next; /* 보관함 목록 */
};

/* 루프마다 백엔드별로 쉬는 연결을 보관한다. 루프끼리 나누지 않으므로 락이 없다 */
struct idle_list {
    struct upconn *head;
    int n;
};

struct proxy_st {
    bool connecting;
    struct pool *pool;
    struct backend *be;
    struct upconn *up;
    unsigned tried;        /* 이번 요청에서 써 본 백엔드 (비트) */
    bool retried;          /* 요청을 보낸 뒤 실패해서 다시 시도했다 (한 번만) */
    bool fresh;            /* 보관된 연결을 쓰지 말 것 */
    int fail_status;       /* 더 시도할 백엔드가 없을 때 보낼 코드 */
    long long give_up;     /* 연결 시도 전체의 마감 */
    long long touched;     /* 백엔드와 마지막으로 바이트를 주고받은 시각 */
    long long body_off;    /* 보낸 요청 본문 바이트 */
    bool send_broken;      /* 백엔드가 더 받지 않는다. 응답은 이미 와 있을 수 있다 */
    size_t scanned;
    int interim;           /* 지나간 1xx 응답 수 */
    bool head_done;
    bool eof;
    bool reusable, bad_status;
    struct body_dec dec;
    struct cached *saving; /* 응답을 중계하면서 같이 모으는 캐시 사본 */
    size_t saving_cap;
    /* 포워드 프록시: 백엔드 묶음 대신 이름 해석 결과로 접속한다 */
    struct resolve *asking; /* 이름 해석을 기다리는 중 */
    struct addr addr[RESOLVE_MAX];
    int naddr, next_addr;
    bool half_closed;
};

extern const struct handler proxy_handler;
extern const struct handler cache_handler; /* 저장해 둔 응답을 내보낸다 */

bool upstream_is_loop(const struct conn *c);       /* Via에 내 이름이 있다 = 나에게 되돌아온 요청 */
void upstream_connect_addrs(struct conn *c);      /* px->addr의 주소들에 차례로 접속 */
void upstream_next_addr(struct conn *c, int err); /* 이번 주소는 실패. 다음 주소로 */

void upstream_event(struct upconn *up); /* 백엔드 소켓에 일이 생겼다 */
void upstream_scan(struct loop *l);     /* 오래 쉰 연결 정리 */
void upstream_close_all(struct loop *l);
void upconn_free(struct upconn *up);

/* 시험할 수 있게 떼어 둔 순수 함수: 클라이언트 요청 → 백엔드로 보낼 요청 머리.
   ip가 NULL이면 포워드 프록시: X-Forwarded-*를 더하지도 지우지도 않는다 */
struct exchange;
bool upstream_request_head(const struct exchange *ex, const char *ip, const char *via,
                           const char *default_host, struct buf *out);

#endif
