#include "loop.h"
#include "cgi.h"
#include "conn.h"
#include "forward.h"
#include "log.h"
#include "server.h"
#include "str.h"
#include "upstream.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_EVENTS 256
#define SCAN_MS 250 /* 제한 시간 검사 간격 */
#define REAP_MS 5

void loop_watch(struct loop *l, struct io *io, unsigned want)
{
    if (!io->dead && want != io->want && ev_set(l->evfd, io, want) < 0)
        perror("ev_set");
}

void loop_bury(struct loop *l, struct io *io)
{
    if (io->dead)
        return;
    /* 이번 묶음에 이 io의 이벤트가 또 들어 있을 수 있다.
       바로 free하면 그 이벤트가 해제된 메모리를 가리키므로 바퀴 끝까지 미룬다 */
    loop_watch(l, io, 0);
    close(io->fd);
    io->dead = true;
    io->next_dead = l->dead;
    l->dead = io;
}

static void reap_dead(struct loop *l)
{
    while (l->dead) {
        struct io *io = l->dead;
        l->dead = io->next_dead;
        switch (io->kind) {
        case IO_CLIENT: conn_free((struct conn *)io); break;
        case IO_UPSTREAM: upconn_free((struct upconn *)io); break;
        case IO_CGI: cgi_proc_free((struct cgi_proc *)io); break;
        default: free(io); break;
        }
    }
}

void loop_ready(struct loop *l, struct conn *c)
{
    if (c->in_ready)
        return;
    c->in_ready = true;
    c->next_ready = l->ready;
    l->ready = c;
}

static void run_ready(struct loop *l)
{
    struct conn *c = l->ready;
    l->ready = NULL;
    while (c) {
        struct conn *next = c->next_ready;
        c->in_ready = false;
        if (!c->io.dead)
            conn_run(c);
        c = next;
    }
}

bool loop_send(struct loop *l, const struct msg *m)
{
    return write(l->pipe[1], m, sizeof *m) == (ssize_t)sizeof *m;
}

static void read_mail(struct loop *l)
{
    struct msg m[32];
    ssize_t n;
    while ((n = read(l->pipe[0], m, sizeof m)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof *m; i++) {
            switch (m[i].type) {
            case MSG_CONN: conn_open(l, m[i].fd, m[i].origin, &m[i].peer); break;
            case MSG_RESOLVED:
                forward_resolved(m[i].ptr);
                free(m[i].ptr);
                break;
            case MSG_DRAIN: l->draining = true; break;
            case MSG_KILL: l->draining = l->quit = true; break;
            }
        }
    }
}

static void *loop_main(void *arg)
{
    struct loop *l = arg;
    struct ev_event ev[MAX_EVENTS];
    while (!l->quit) {
        /* 루프 스레드가 잠드는 곳은 프로그램 전체에서 이 한 줄뿐이다 */
        int n = ev_wait(l->evfd, ev, MAX_EVENTS, l->ready ? 0 : l->reaping ? REAP_MS : SCAN_MS);
        long long woke = now_us();
        l->now = woke / 1000;
        for (int i = 0; i < n; i++) {
            struct io *io = ev[i].io;
            if (io->dead)
                continue;
            io->ready |= ev[i].ready;
            switch (io->kind) {
            case IO_PIPE: read_mail(l); break;
            case IO_CLIENT: conn_run((struct conn *)io); break;
            case IO_UPSTREAM: upstream_event((struct upconn *)io); break;
            case IO_CGI: cgi_event((struct cgi_proc *)io); break;
            default: break;
            }
        }
        run_ready(l);
        if (l->now >= l->next_scan || l->draining) {
            conn_scan(l);
            upstream_scan(l);
            l->reaping = l->children != NULL;
            l->next_scan = l->now + SCAN_MS;
        }
        if (l->reaping)
            cgi_reap(l, false);
        reap_dead(l);
        log_flush(l);
        long long busy = now_us() - woke;
        if (busy > atomic_load_explicit(&l->max_busy_us, memory_order_relaxed))
            atomic_store_explicit(&l->max_busy_us, busy, memory_order_relaxed);
        if (l->draining && !l->conns)
            break;
    }
    conn_close_all(l);
    upstream_close_all(l);
    cgi_reap(l, true);
    reap_dead(l);
    log_flush(l);
    buf_free(&l->log);
    atomic_fetch_sub(&l->srv->loops_alive, 1);
    return NULL;
}

int loop_start(struct loop *l, int id, struct server *srv)
{
    memset(l, 0, sizeof *l);
    l->id = id;
    l->srv = srv;
    buf_init(&l->log, 65536);
    l->evfd = ev_open();
    l->idle = calloc((size_t)srv->cfg->nbackends + 1, sizeof *l->idle);
    if (l->evfd < 0 || !l->idle || net_pipe(l->pipe) < 0)
        return -1;
    l->pipe_io = (struct io){.fd = l->pipe[0], .kind = IO_PIPE};
    loop_watch(l, &l->pipe_io, IO_R);

    /* 시그널은 메인 스레드만 받게, 새 스레드는 전부 막고 시작한다 (마스크는 물려받는다) */
    atomic_fetch_add(&srv->loops_alive, 1);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int err = pthread_create(&l->thread, NULL, loop_main, l);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = err;
    return err ? -1 : 0;
}

void loop_join(struct loop *l)
{
    pthread_join(l->thread, NULL);
    close(l->pipe[0]);
    close(l->pipe[1]);
    close(l->evfd);
    free(l->idle);
}
