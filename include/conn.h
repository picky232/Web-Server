#ifndef CONN_H
#define CONN_H

#include "buf.h"
#include "ev.h"
#include "net.h"

struct loop;

enum conn_state {
    CS_HEAD,   /* 요청 머리를 기다림 */
    CS_FLUSH,  /* 남은 응답을 보냄 */
    CS_LINGER, /* 쓰기는 닫고, 클라이언트가 보낸 나머지를 버리며 닫히기를 기다림 */
    CS_DEAD,
};

/* 클라이언트 연결 하나 */
struct conn {
    struct io io; /* 맨 앞이어야 한다: 이벤트가 들고 온 io 포인터가 곧 conn 포인터 */
    struct loop *loop;
    struct conn *prev, *next;
    struct conn *next_ready;
    bool in_ready;
    enum conn_state state;
    int origin;
    char ip[46];
    bool rd_eof; /* 클라이언트가 보내기를 끝냈다 (FIN) */
    struct buf in, out;
    long long since; /* 지금 상태가 시작된 시각 */
};

void conn_open(struct loop *l, int fd, int origin, const struct addr *peer);
void conn_run(struct conn *c); /* 막힐 때까지 진행. 이벤트가 오면 항상 이것 하나만 부른다 */
void conn_scan(struct loop *l);
void conn_close_all(struct loop *l);
void conn_free(struct conn *c); /* 루프 바퀴 끝에서만 */

#endif
