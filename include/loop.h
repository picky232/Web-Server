#ifndef LOOP_H
#define LOOP_H

#include "ev.h"
#include "net.h"

#include <pthread.h>
#include <stdatomic.h>

struct server;
struct conn;

/* 메인 스레드가 루프 스레드에 보내는 쪽지. 파이프로 통째로 전달된다 (512바이트 이하는 쪼개지지 않음) */
enum msg_type { MSG_CONN, MSG_DRAIN, MSG_KILL };
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
    long long now;       /* 이번 바퀴의 시각 */
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
