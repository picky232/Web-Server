#ifndef LOOP_H
#define LOOP_H

#include "buf.h"
#include "ev.h"
#include "net.h"

#include <pthread.h>
#include <stdatomic.h>

struct server;
struct conn;
struct idle_list;
struct child;

/* 메인 스레드가 루프 스레드에 보내는 쪽지. 파이프로 통째로 전달된다 (512바이트 이하는 쪼개지지 않음) */
enum msg_type { MSG_CONN, MSG_WAKE };
struct msg {
    enum msg_type type;
    int fd;
    int origin; /* 어느 listen 소켓으로 들어왔나 (enum origin) */
    struct addr peer;
};

/* 이벤트 루프 하나 = 스레드 하나. 이 안의 값은 atomic을 빼면 그 스레드만 만진다 → 락이 없다 */
struct loop {
    int id;
    struct server *srv;
    pthread_t thread;
    int evfd;
    int pipe[2];
    struct io pipe_io;

    struct conn *conns;  /* 이 루프가 맡은 연결 전부 */
    atomic_int nconn;    /* 메인 스레드가 가장 한가한 루프를 고를 때 읽는다 */
    struct conn *ready;  /* 커널은 조용하지만 이어서 할 일이 남은 연결 */
    struct io *dead;     /* 이번 바퀴 끝에 free할 것 */
    struct idle_list *idle; /* 백엔드별로 쉬는 연결 */
    struct child *children; /* 실행 중이거나 아직 회수하지 않은 CGI 자식 */
    bool reaping;           /* 곧 끝날 자식이 있다 → 짧은 간격으로 회수를 시도 */
    struct buf log;         /* 바퀴 끝에 한 번에 쓰는 접근 로그 */

    /* 상태 페이지가 다른 스레드에서 읽는 숫자들 */
    atomic_llong accepted, requests, bytes, log_dropped;
    atomic_llong by_class[6]; /* 1xx~5xx, 0 = 응답 전에 떠남 */
    atomic_llong max_busy_us; /* 한 바퀴가 가장 오래 걸린 시간: 그동안 다른 연결은 기다렸다 */
    long long now;       /* 이번 바퀴의 시각 */
    unsigned seen_beats; /* 메인 스레드만 쓴다: 지난번에 본 값 */
    bool stuck;          /* 메인 스레드만 쓴다 */
    atomic_uint beats;   /* 돌 때마다 +1. 쉬어도 0.25초에 한 번은 돈다 → 안 늘면 멈춘 것 */
    _Atomic(struct conn *) current; /* 지금 진행 중인 연결 (죽을 때 기록용) */
    long long next_scan;
    bool draining, quit;
};

int loop_start(struct loop *l, int id, struct server *srv);
bool loop_send(struct loop *l, const struct msg *m); /* 다른 스레드에서 호출. 파이프가 차면 false */
void loop_join(struct loop *l);

void loop_watch(struct loop *l, struct io *io, unsigned want);
void loop_bury(struct loop *l, struct io *io); /* 감시 해제 → close → 바퀴 끝에 free */
void loop_ready(struct loop *l, struct conn *c);

#endif
