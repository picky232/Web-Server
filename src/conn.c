#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IN_MAX 16384
#define OUT_MAX 65536
#define IDLE_MS 15000
#define LINGER_MS 2000

void conn_open(struct loop *l, int fd, int origin, const struct addr *peer)
{
    struct conn *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        atomic_fetch_sub(&l->nconn, 1);
        return;
    }
    c->io = (struct io){.fd = fd, .kind = IO_CLIENT};
    c->loop = l;
    c->origin = origin;
    c->since = l->now;
    net_ip(peer, c->ip, sizeof c->ip);
    buf_init(&c->in, IN_MAX);
    buf_init(&c->out, OUT_MAX);
    c->next = l->conns;
    if (l->conns)
        l->conns->prev = c;
    l->conns = c;
    loop_watch(l, &c->io, IO_R);
}

void conn_free(struct conn *c)
{
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);
}

static void destroy(struct conn *c)
{
    struct loop *l = c->loop;
    if (c->prev)
        c->prev->next = c->next;
    else
        l->conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    atomic_fetch_sub(&l->nconn, 1);
    loop_bury(l, &c->io);
}

static void set_state(struct conn *c, enum conn_state s)
{
    c->state = s;
    c->since = c->loop->now;
}

static void fill(struct conn *c)
{
    while ((c->io.ready & IO_R) && !c->rd_eof && buf_room(&c->in)) {
        size_t n = buf_room(&c->in);
        char *p = buf_reserve(&c->in, n);
        ssize_t r = p ? read(c->io.fd, p, n) : -1;
        if (r > 0) {
            buf_commit(&c->in, (size_t)r);
        } else if (r == 0) {
            c->rd_eof = true;
        } else if (errno == EAGAIN || errno == EINTR) {
            c->io.ready &= ~IO_R;
        } else {
            c->state = CS_DEAD; /* 클라이언트가 끊었다 (RST) */
            return;
        }
    }
}

static void flush(struct conn *c)
{
    while (buf_len(&c->out)) {
        ssize_t r = write(c->io.fd, buf_head(&c->out), buf_len(&c->out));
        if (r > 0)
            buf_consume(&c->out, (size_t)r);
        else if (errno == EAGAIN || errno == EINTR)
            return;
        else {
            c->state = CS_DEAD;
            return;
        }
    }
}

void conn_run(struct conn *c)
{
    fill(c);
    if (c->state == CS_HEAD) {
        char *p = buf_head(&c->in);
        size_t n = buf_len(&c->in);
        bool end = false;
        for (size_t i = 3; i < n && !end; i++)
            end = !memcmp(p + i - 3, "\r\n\r\n", 4);
        if (end) {
            buf_consume(&c->in, n);
            buf_printf(&c->out, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nhello\n");
            set_state(c, CS_FLUSH);
        } else if (c->rd_eof || n == IN_MAX) {
            c->state = CS_DEAD;
        }
    }
    if (c->state != CS_DEAD)
        flush(c);
    if (c->state == CS_FLUSH && !buf_len(&c->out)) {
        /* 바로 close하면 커널이 RST를 보내 클라이언트가 응답을 못 읽을 수 있다. 쓰기만 먼저 닫는다 */
        shutdown(c->io.fd, SHUT_WR);
        set_state(c, CS_LINGER);
    }
    if (c->state == CS_LINGER) {
        buf_consume(&c->in, buf_len(&c->in));
        if (c->rd_eof)
            c->state = CS_DEAD;
    }
    if (c->state == CS_DEAD) {
        destroy(c);
        return;
    }
    loop_watch(c->loop, &c->io, (c->rd_eof ? 0 : IO_R) | (buf_len(&c->out) ? IO_W : 0));
}

void conn_scan(struct loop *l)
{
    for (struct conn *c = l->conns, *next; c; c = next) {
        next = c->next;
        long long limit = c->state == CS_LINGER ? LINGER_MS : IDLE_MS;
        if (l->now - c->since > limit || (l->draining && c->state == CS_HEAD))
            destroy(c);
    }
}

void conn_close_all(struct loop *l)
{
    while (l->conns)
        destroy(l->conns);
}
