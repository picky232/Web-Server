#include "event.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__linux__)
/* ---------------------------- epoll ---------------------------- */
#include <sys/epoll.h>

struct ev_loop { int fd; };

ev_loop *ev_create(void) {
    ev_loop *l = malloc(sizeof *l);
    if (!l) return NULL;
    l->fd = epoll_create1(EPOLL_CLOEXEC);
    if (l->fd < 0) { free(l); return NULL; }
    return l;
}

void ev_destroy(ev_loop *l) {
    if (!l) return;
    close(l->fd);
    free(l);
}

int ev_update(ev_loop *l, int fd, int old_mask, int new_mask, void *udata) {
    if (old_mask == new_mask) return 0;
    if (new_mask == 0) return epoll_ctl(l->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event e = {0};
    if (new_mask & EV_READ)  e.events |= EPOLLIN;
    if (new_mask & EV_WRITE) e.events |= EPOLLOUT;
    e.data.ptr = udata;
    return epoll_ctl(l->fd, old_mask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &e);
}

int ev_wait(ev_loop *l, ev_event *out, int max, int timeout_ms) {
    struct epoll_event evs[256];
    if (max > 256) max = 256;
    int n = epoll_wait(l->fd, evs, max, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        out[i].udata = evs[i].data.ptr;
        out[i].fd    = -1; /* epoll 은 fd 를 돌려주지 않으므로 udata 로 식별 */
        out[i].mask  = 0;
        if (evs[i].events & EPOLLIN)  out[i].mask |= EV_READ;
        if (evs[i].events & EPOLLOUT) out[i].mask |= EV_WRITE;
        out[i].eof = (evs[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0;
    }
    return n;
}

#else
/* ---------------------------- kqueue --------------------------- */
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

struct ev_loop { int fd; };

ev_loop *ev_create(void) {
    ev_loop *l = malloc(sizeof *l);
    if (!l) return NULL;
    l->fd = kqueue();
    if (l->fd < 0) { free(l); return NULL; }
    return l;
}

void ev_destroy(ev_loop *l) {
    if (!l) return;
    close(l->fd);
    free(l);
}

int ev_update(ev_loop *l, int fd, int old_mask, int new_mask, void *udata) {
    struct kevent ch[2];
    int n = 0;
    int add = new_mask & ~old_mask;
    int del = old_mask & ~new_mask;
    if (add & EV_READ)  EV_SET(&ch[n++], fd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, udata);
    if (add & EV_WRITE) EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, udata);
    if (del & EV_READ)  EV_SET(&ch[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    if (del & EV_WRITE) EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (n == 0) return 0;
    return kevent(l->fd, ch, n, NULL, 0, NULL);
}

int ev_wait(ev_loop *l, ev_event *out, int max, int timeout_ms) {
    struct kevent evs[256];
    if (max > 256) max = 256;
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(l->fd, NULL, 0, evs, max, tp);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        out[i].udata = evs[i].udata;
        out[i].fd    = (int)evs[i].ident;
        out[i].mask  = evs[i].filter == EVFILT_READ ? EV_READ : EV_WRITE;
        out[i].eof   = (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
    }
    return n;
}
#endif
