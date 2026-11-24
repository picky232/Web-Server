/* Linux: epoll. 한 fd의 읽기·쓰기를 한 번에 등록한다 */
#include "ev.h"

#include <sys/epoll.h>

#define EV_MAX 256

int ev_open(void)
{
    return epoll_create1(EPOLL_CLOEXEC);
}

int ev_set(int evfd, struct io *io, unsigned want)
{
    if (want == io->want)
        return 0;
    struct epoll_event e = {.data.ptr = io};
    if (want & IO_R)
        e.events |= EPOLLIN;
    if (want & IO_W)
        e.events |= EPOLLOUT;
    /* 볼 것이 없으면 아예 뺀다. 등록만 남겨 두면 epoll은 끊김(HUP)을 계속 알려 루프가 헛돈다 */
    int op = io->want == 0 ? EPOLL_CTL_ADD : want == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
    if (epoll_ctl(evfd, op, io->fd, &e) < 0)
        return -1;
    io->want = want;
    return 0;
}

int ev_wait(int evfd, struct ev_event *out, int max, int timeout_ms)
{
    struct epoll_event ev[EV_MAX];
    int n = epoll_wait(evfd, ev, max < EV_MAX ? max : EV_MAX, timeout_ms);
    for (int i = 0; i < n; i++) {
        struct io *io = ev[i].data.ptr;
        unsigned ready = 0;
        if (ev[i].events & EPOLLIN)
            ready |= IO_R;
        if (ev[i].events & EPOLLOUT)
            ready |= IO_W;
        if (ev[i].events & (EPOLLERR | EPOLLHUP))
            ready |= io->want; /* 오류의 정체는 실제 read/write가 errno로 알려 준다 */
        out[i] = (struct ev_event){io, ready};
    }
    return n;
}
