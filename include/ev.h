#ifndef EV_H
#define EV_H

#include <stdbool.h>

#define IO_R 1u
#define IO_W 2u

enum io_kind {
    IO_PIPE,     /* 다른 스레드가 보낸 쪽지 / 시그널 */
    IO_LISTEN,   /* 새 연결을 받는 소켓 */
    IO_CLIENT,   /* struct conn */
    IO_UPSTREAM, /* struct upconn: 백엔드와의 연결 */
    IO_CGI,      /* struct cgi_proc: CGI 프로그램의 출력 */
    IO_HEALTH,   /* 헬스 체크용 연결 */
};

/* 커널에 지켜봐 달라고 맡긴 fd 하나. 이벤트는 fd 번호가 아니라 이 포인터를 들고 온다.
   fd 번호는 닫으면 바로 재사용되므로, 번호로 찾으면 남의 연결을 건드릴 수 있다 */
struct io {
    int fd;
    unsigned want;  /* 지금 커널에 등록해 둔 방향 (IO_R | IO_W) */
    unsigned ready; /* 커널이 준비됐다고 알려 준 방향. EAGAIN을 만나면 지운다 */
    bool dead;      /* 닫힘. 같은 묶음에 남은 낡은 이벤트를 거른다 */
    enum io_kind kind;
    struct io *next_dead;
};

struct ev_event {
    struct io *io;
    unsigned ready;
};

/* 이벤트 장치(macOS kqueue, Linux epoll)를 감싼 얇은 층. 둘 다 "조건이 남아 있으면 매번 알려 주는" 방식 */
int ev_open(void);
int ev_set(int evfd, struct io *io, unsigned want); /* io가 지켜볼 방향을 want로 바꾼다 */
int ev_wait(int evfd, struct ev_event *out, int max, int timeout_ms);

#endif
