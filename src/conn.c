#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OUT_MAX (65536 + 32768) /* 본문 64KB + 머리가 들어갈 여유 */
#define RUN_BUDGET 16           /* 한 번에 이만큼만 나아가고 다른 연결에 차례를 넘긴다 */
#define LINGER_MAX 262144
#define IDLE_MS 15000
#define HEAD_MS 10000
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
    buf_init(&c->in, HTTP_HEAD_MAX);
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

/* 연결을 없애는 유일한 곳 */
static void destroy(struct conn *c)
{
    struct loop *l = c->loop;
    exchange_free(c);
    if (c->prev)
        c->prev->next = c->next;
    else
        l->conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (c->reset) {
        struct linger lg = {.l_onoff = 1, .l_linger = 0};
        setsockopt(c->io.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    }
    atomic_fetch_sub(&l->nconn, 1);
    loop_bury(l, &c->io);
}

void conn_set_state(struct conn *c, enum conn_state s)
{
    c->state = s;
    c->since = c->loop->now;
}

static void fill(struct conn *c)
{
    if (!(c->io.ready & IO_R) || c->rd_eof || !buf_room(&c->in))
        return;
    size_t n = buf_room(&c->in);
    char *p = buf_reserve(&c->in, n);
    ssize_t r = p ? read(c->io.fd, p, n) : -1;
    if (r > 0) {
        buf_commit(&c->in, (size_t)r);
        if ((size_t)r < n)
            c->io.ready &= ~IO_R; /* 다 비웠다. 더 오면 커널이 다시 알려 준다 */
    } else if (r == 0) {
        c->rd_eof = true;
    } else if (errno == EAGAIN || errno == EINTR) {
        c->io.ready &= ~IO_R;
    } else {
        c->state = CS_DEAD; /* 클라이언트가 끊었다 (RST) */
    }
}

static bool flush(struct conn *c)
{
    bool moved = false;
    while (buf_len(&c->out)) {
        ssize_t r = write(c->io.fd, buf_head(&c->out), buf_len(&c->out));
        if (r > 0) {
            buf_consume(&c->out, (size_t)r);
            moved = true;
        } else if (errno == EAGAIN || errno == EINTR) {
            break;
        } else {
            c->state = CS_DEAD;
            break;
        }
    }
    return moved;
}

static bool step_flush(struct conn *c)
{
    if (buf_len(&c->out))
        return false;
    bool keep = c->ex->reply.keep && !c->loop->draining;
    exchange_free(c);
    if (keep) {
        if (!buf_len(&c->in))
            buf_free(&c->in); /* 쉬는 연결은 버퍼를 돌려준다 */
        buf_free(&c->out);
        conn_set_state(c, CS_HEAD);
        return true;
    }
    /* 바로 close하면 커널이 RST를 보내, 클라이언트가 방금 보낸 응답을 못 읽을 수 있다.
       쓰기만 먼저 닫고 클라이언트가 닫기를 잠깐 기다린다 */
    shutdown(c->io.fd, SHUT_WR);
    conn_set_state(c, CS_LINGER);
    return true;
}

static bool step_linger(struct conn *c)
{
    size_t n = buf_len(&c->in);
    c->lingered += n;
    buf_consume(&c->in, n);
    if (c->rd_eof || c->lingered > LINGER_MAX)
        conn_set_state(c, CS_DEAD);
    return n > 0;
}

static bool step(struct conn *c)
{
    switch (c->state) {
    case CS_HEAD: return exchange_head(c);
    case CS_BODY: return exchange_body(c);
    case CS_RESPOND: return c->ex->h->pump && c->ex->h->pump(c);
    case CS_FLUSH: return step_flush(c);
    case CS_LINGER: return step_linger(c);
    case CS_DEAD: break;
    }
    return false;
}

static void update_interest(struct conn *c)
{
    unsigned want = 0;
    if (!c->rd_eof && buf_room(&c->in))
        want |= IO_R;
    if (buf_len(&c->out))
        want |= IO_W;
    loop_watch(c->loop, &c->io, want);
    if (c->state == CS_RESPOND && c->ex->h->interest)
        c->ex->h->interest(c);
}

void conn_run(struct conn *c)
{
    bool moved = true;
    for (int i = 0; moved && c->state != CS_DEAD; i++) {
        if (i == RUN_BUDGET) { /* 아직 할 일이 있지만 다른 연결도 돌봐야 한다 */
            loop_ready(c->loop, c);
            break;
        }
        fill(c);
        if (c->state == CS_DEAD)
            break;
        moved = step(c);
        if (c->state != CS_DEAD)
            moved |= flush(c);
    }
    if (c->state == CS_DEAD)
        destroy(c);
    else
        update_interest(c);
}

/* 제한 시간이 지난 연결을 정리한다 */
static void check_time(struct conn *c)
{
    long long now = c->loop->now;
    bool late = false;
    switch (c->state) {
    case CS_HEAD:
        if (c->first_byte) /* 첫 바이트부터 재므로 1바이트씩 보내도 늘어나지 않는다 */
            late = now - c->first_byte > HEAD_MS;
        else
            late = now - c->since > IDLE_MS || c->loop->draining;
        break;
    case CS_LINGER: late = now - c->since > LINGER_MS; break;
    case CS_RESPOND:
        if (c->ex->deadline && now >= c->ex->deadline && c->ex->h->timeout) {
            c->ex->h->timeout(c);
            conn_run(c);
            return;
        }
        break;
    default: late = now - c->since > 60000; break;
    }
    if (late) {
        c->state = CS_DEAD;
        conn_run(c);
    }
}

void conn_scan(struct loop *l)
{
    for (struct conn *c = l->conns, *next; c; c = next) {
        next = c->next;
        check_time(c);
    }
}

void conn_close_all(struct loop *l)
{
    while (l->conns)
        destroy(l->conns);
}
