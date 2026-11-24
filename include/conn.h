#ifndef CONN_H
#define CONN_H

#include "buf.h"
#include "ev.h"
#include "exchange.h"
#include "net.h"

struct loop;

enum conn_state {
    CS_HEAD,    /* 요청 머리를 기다림 */
    CS_BODY,    /* 요청 본문을 모으는 중 */
    CS_RESPOND, /* 핸들러가 응답을 만드는 중 */
    CS_FLUSH,   /* 남은 응답을 보냄 */
    CS_LINGER,  /* 쓰기는 닫고, 클라이언트가 보낸 나머지를 버리며 닫히기를 기다림 */
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
    bool reset;  /* 닫을 때 RST로 끊는다 (응답이 잘렸음을 알리려고) */
    struct buf in, out;
    size_t scanned;       /* in에서 빈 줄을 어디까지 찾아봤나 */
    long long since;      /* 지금 상태가 시작된 시각 */
    long long first_byte; /* 이번 요청의 첫 바이트가 온 시각. 0 = 아직 */
    size_t lingered;      /* LINGER에서 버린 바이트 */
    int served;           /* 이 연결에서 받은 요청 수 */
    struct exchange *ex;  /* 지금 처리 중인 요청. 없으면 NULL */
};

void conn_open(struct loop *l, int fd, int origin, const struct addr *peer);
/* 막힐 때까지 진행한다. 클라이언트·백엔드·CGI 어느 쪽 이벤트가 와도 항상 이것 하나만 부른다.
   닫기와 관심 방향 계산은 이 함수의 마지막에서만 일어난다 */
void conn_run(struct conn *c);
void conn_set_state(struct conn *c, enum conn_state s);
void conn_scan(struct loop *l);
void conn_close_all(struct loop *l);
void conn_free(struct conn *c); /* 루프 바퀴 끝에서만 */

#endif
