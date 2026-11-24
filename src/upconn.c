/* 백엔드와의 연결: 열기, 쉬는 동안 보관하기, 다시 꺼내기 */
#include "upstream.h"
#include "config.h"
#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define UP_IN_MAX 65536
#define UP_OUT_MAX 65536
#define IDLE_MAX 16  /* 루프·백엔드마다 보관하는 연결 수 */
#define IDLE_MS 4000 /* 백엔드가 먼저 닫기 전에 버린다 */

struct upconn *upconn_open(struct loop *l, const struct addr *a, struct backend *b)
{
    int fd = net_connect(a);
    struct upconn *up = fd >= 0 ? calloc(1, sizeof *up) : NULL;
    if (!up) {
        int e = fd < 0 ? errno : ENOMEM;
        if (fd >= 0)
            close(fd);
        errno = e;
        return NULL;
    }
    up->io = (struct io){.fd = fd, .kind = IO_UPSTREAM};
    up->loop = l;
    up->be = b;
    buf_init(&up->in, UP_IN_MAX);
    buf_init(&up->out, UP_OUT_MAX);
    return up;
}

static void idle_remove(struct upconn *up)
{
    struct idle_list *l = &up->loop->idle[up->be->index];
    if (up->prev)
        up->prev->next = up->next;
    else
        l->head = up->next;
    if (up->next)
        up->next->prev = up->prev;
    up->prev = up->next = NULL;
    l->n--;
}

void upconn_keep(struct upconn *up)
{
    struct idle_list *l = &up->loop->idle[up->be->index];
    if (l->n >= IDLE_MAX) {
        loop_bury(up->loop, &up->io);
        return;
    }
    up->c = NULL;
    up->reused = true;
    up->idle_since = up->loop->now;
    up->io.ready = 0;
    buf_free(&up->in);
    buf_free(&up->out);
    up->next = l->head;
    if (l->head)
        l->head->prev = up;
    l->head = up;
    l->n++;
    loop_watch(up->loop, &up->io, IO_R); /* 쉬는 동안 백엔드가 닫으면 바로 알아채려고 */
}

struct upconn *upconn_reuse(struct loop *l, struct backend *b)
{
    struct upconn *up = l->idle[b->index].head; /* 가장 최근에 쓴 것 */
    if (!up || l->now - up->idle_since > IDLE_MS)
        return NULL;
    idle_remove(up);
    return up;
}

void upstream_scan(struct loop *l)
{
    for (int i = 0; i < l->srv->cfg->nbackends; i++)
        for (struct upconn *up = l->idle[i].head, *next; up; up = next) {
            next = up->next;
            if (l->draining || l->now - up->idle_since > IDLE_MS) {
                idle_remove(up);
                loop_bury(l, &up->io);
            }
        }
}

void upstream_close_all(struct loop *l)
{
    l->draining = true;
    upstream_scan(l);
}

void upconn_free(struct upconn *up)
{
    buf_free(&up->in);
    buf_free(&up->out);
    free(up);
}

void upstream_event(struct upconn *up)
{
    if (up->c) {
        conn_run(up->c);
        return;
    }
    /* 쉬는 연결에 온 것은 "닫힘"이거나 요청하지도 않은 바이트다. 둘 다 다시 쓰면 안 된다 */
    idle_remove(up);
    loop_bury(up->loop, &up->io);
}

