/* 포워드 프록시: 클라이언트 편에서, 클라이언트가 고른 바깥 서버로 대신 접속한다.
   리버스 프록시와 다른 것은 목적지를 정하는 방법뿐이고, 중계는 upstream.c를 그대로 쓴다 */
#include "forward.h"
#include "config.h"
#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TUNNEL_IDLE_MS 300000

static struct proxy_st *px_of(struct conn *c)
{
    return &c->ex->u.proxy;
}

static void ask(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    memset(px, 0, sizeof *px);
    if (upstream_is_loop(c)) {
        exchange_error(c, 508, NULL);
        return;
    }
    snprintf(ex->served_by, sizeof ex->served_by, "%.80s:%d", ex->host, ex->port);
    px->asking = resolver_ask(c->loop, c, ex->host, ex->port);
    if (!px->asking)
        exchange_error(c, 503, "Retry-After: 1\r\n");
    ex->deadline = c->loop->now + c->loop->srv->cfg->response_ms;
}

void forward_resolved(struct resolve *r)
{
    struct conn *c = r->c;
    if (!c)
        return; /* 기다리던 손님이 떠났다 */
    struct proxy_st *px = px_of(c);
    const struct config *cfg = c->loop->srv->cfg;
    px->asking = NULL;
    /* 이름이 아니라 실제로 접속할 주소를 검사한다. 이름만 보면 DNS로 내부 주소를 돌려주어 우회할 수 있다.
       내 컴퓨터에서만 쓰는 프록시라면 내부 주소도 허용한다 */
    for (int i = 0; i < r->n; i++)
        if (net_is_loopback(&cfg->forward) || !net_is_private(&r->addr[i]))
            px->addr[px->naddr++] = r->addr[i];
    if (!r->ok)
        exchange_error(c, 502, NULL);
    else if (px->naddr == 0)
        exchange_error(c, 403, NULL);
    else
        upstream_connect_addrs(c);
    conn_run(c);
}

static void forward_timeout(struct conn *c)
{
    if (px_of(c)->asking)
        exchange_error(c, 504, NULL);
    else
        proxy_handler.timeout(c);
}

static void forward_stop(struct conn *c)
{
    if (px_of(c)->asking)
        px_of(c)->asking->c = NULL; /* 쪽지가 도착하면 그때 해제된다 */
    px_of(c)->asking = NULL;
    proxy_handler.stop(c);
}

static bool forward_pump(struct conn *c)
{
    return px_of(c)->up && proxy_handler.pump(c);
}

static void forward_interest(struct conn *c)
{
    proxy_handler.interest(c);
}

/* ---- CONNECT 터널 ---- */

/* 한 방향: 받은 쪽 버퍼 → 보낼 쪽 소켓 */
static bool push(struct buf *from, int fd, bool *broken)
{
    bool moved = false;
    while (buf_len(from) && !*broken) {
        ssize_t w = write(fd, buf_head(from), buf_len(from));
        if (w > 0) {
            buf_consume(from, (size_t)w);
            moved = true;
        } else if (errno == EAGAIN || errno == EINTR) {
            break;
        } else {
            *broken = true;
        }
    }
    return moved;
}

static bool tunnel_pump(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    struct upconn *up = px->up;
    if (!up)
        return false;
    bool moved = false;
    if (px->connecting) {
        if (!(up->io.ready & IO_W))
            return false;
        int err = net_error(up->io.fd);
        if (err) {
            upstream_next_addr(c, err);
            return true;
        }
        px->connecting = false;
        buf_consume(&up->out, buf_len(&up->out));
        buf_append(&c->out, "HTTP/1.1 200 Connection Established\r\n\r\n", 39);
        ex->reply = (struct reply){.started = true, .status = 200, .framing = FRAME_CLOSE, .minor = 1};
        moved = true;
    }
    moved |= push(&c->in, up->io.fd, &px->send_broken);
    if (c->rd_eof && !buf_len(&c->in) && !px->half_closed) {
        shutdown(up->io.fd, SHUT_WR); /* 클라이언트가 보내기를 끝냈다고 상대에게도 알린다 */
        px->half_closed = true;
        moved = true;
    }
    if (!px->eof && (up->io.ready & IO_R) && buf_room(&c->out) > 32768) {
        char *p = buf_reserve(&c->out, 32768);
        ssize_t r = p ? read(up->io.fd, p, 32768) : -1;
        if (r > 0) {
            buf_commit(&c->out, (size_t)r);
            ex->reply.sent += r;
            if (r < 32768)
                up->io.ready &= ~IO_R;
            moved = true;
        } else if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            up->io.ready &= ~IO_R;
        } else {
            px->eof = true;
            moved = true;
        }
    }
    if (px->eof || px->send_broken) {
        ex->reply.finished = true;
        ex->reply.keep = false;
        forward_stop(c);
        ex->h = NULL;
        ex->deadline = 0;
        conn_set_state(c, CS_FLUSH);
        return true;
    }
    if (moved)
        ex->deadline = c->loop->now + TUNNEL_IDLE_MS;
    return moved;
}

static void tunnel_interest(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    if (!px->up)
        return;
    unsigned want = 0;
    if (px->connecting)
        want = IO_W;
    else {
        if (!px->eof && buf_room(&c->out) > 32768)
            want |= IO_R;
        if (buf_len(&c->in))
            want |= IO_W;
    }
    loop_watch(c->loop, &px->up->io, want);
}

static void tunnel_timeout(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    if (px->asking)
        exchange_error(c, 504, NULL);
    else if (px->connecting)
        upstream_next_addr(c, ETIMEDOUT);
    else
        exchange_abort(c);
}

const struct handler forward_handler = {
    .name = "forward", .start = ask, .pump = forward_pump, .interest = forward_interest,
    .timeout = forward_timeout, .stop = forward_stop,
};
const struct handler tunnel_handler = {
    .name = "tunnel", .start = ask, .pump = tunnel_pump, .interest = tunnel_interest,
    .timeout = tunnel_timeout, .stop = forward_stop,
};
