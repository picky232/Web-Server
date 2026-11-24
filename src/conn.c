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
#define GATHER 16384
#define LINGER_MAX 262144
#define LINGER_MS 2000

static void destroy(struct conn *c);

/* 자리가 없을 때 누구를 먼저 내보낼까. 작을수록 먼저, -1 = 내보내지 않음 */
static int evict_class(const struct conn *c)
{
    switch (c->state) {
    case CS_HEAD:
        if (c->first_byte)
            return 2;                  /* 머리를 보내다 만 연결 */
        return c->served ? 0 : 1;      /* 쉬는 연결, 아무것도 안 보낸 연결 */
    case CS_BODY:                      /* 본문을 보내다 멈춘 연결. 잘 올라오는 업로드는 건드리지 않는다 */
        return c->loop->now - c->last_in > 1000 ? 3 : -1;
    case CS_LINGER: return 0;
    default:                           /* 응답을 1초 넘게 받아 가지 않는 연결 */
        return c->send_since && c->loop->now - c->send_since > 1000 ? 4 : -1;
    }
}

static bool evict_one(struct loop *l)
{
    struct conn *victim = NULL;
    int best = 99;
    for (struct conn *c = l->conns; c; c = c->next) {
        int k = evict_class(c);
        if (k < 0 || k > best || (k == best && c->since >= victim->since))
            continue;
        victim = c;
        best = k;
    }
    if (!victim)
        return false;
    destroy(victim);
    return true;
}

void conn_open(struct loop *l, int fd, int origin, const struct addr *peer)
{
    const struct config *cfg = l->srv->cfg;
    int share = (cfg->max_connections + l->srv->nloops - 1) / l->srv->nloops;
    struct conn *c = NULL;
    if (atomic_load(&l->nconn) <= share || evict_one(l))
        c = calloc(1, sizeof *c);
    if (!c) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                   "Retry-After: 1\r\nConnection: close\r\n\r\n";
        (void)!write(fd, busy, sizeof busy - 1);
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
    atomic_fetch_add(&l->accepted, 1);
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
    if (c->in_ready) { /* 곧 해제되므로 "이어서 할 일" 목록에서도 뺀다 */
        struct conn **p = &l->ready;
        while (*p && *p != c)
            p = &(*p)->next_ready;
        if (*p)
            *p = c->next_ready;
    }
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
        c->last_in = c->loop->now;
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
            c->send_bytes += r;
            c->last_out = c->loop->now;
            moved = true;
        } else if (errno == EAGAIN || errno == EINTR) {
            break;
        } else {
            c->state = CS_DEAD;
            break;
        }
    }
    if (!buf_len(&c->out)) {
        c->send_since = 0;
    } else if (!c->send_since) {
        c->send_since = c->last_out = c->loop->now;
        c->send_bytes = 0;
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
        /* 머리만 먼저 보내지 않고, 이어서 나올 본문과 모아 한 번에 보낸다 (시스템 콜과 패킷이 절반) */
        bool more = moved && c->state == CS_RESPOND && buf_len(&c->out) < GATHER;
        if (c->state != CS_DEAD && !more)
            moved |= flush(c);
    }
    if (c->state == CS_DEAD)
        destroy(c);
    else
        update_interest(c);
}

/* 두 가지를 같이 본다.
   평균: 처음 grace 동안은 봐주고, 그 뒤로는 옮긴 바이트만큼만 시간을 준다 (29초마다 1바이트씩 보내는 수법을 막음)
   멈춤: 마지막으로 움직인 지 grace가 지남 (앞에서 빨리 보내 시간을 벌어 놓고 멈추는 수법을 막음) */
static bool too_slow(const struct conn *c, long long since, long long bytes, long long last, long long grace)
{
    long long now = c->loop->now;
    return now - since > grace + bytes * 1000 / c->loop->srv->cfg->min_rate || now - last > grace;
}

/* 모든 대기에는 끝이 있다 */
static void check_time(struct conn *c)
{
    const struct config *cfg = c->loop->srv->cfg;
    long long now = c->loop->now;
    bool act = false;
    if (c->send_since && too_slow(c, c->send_since, c->send_bytes, c->last_out, cfg->stall_ms)) {
        c->reset = true; /* 받아 가지 않는 클라이언트. 남은 응답을 커널이 계속 들고 있지 않게 끊는다 */
        c->state = CS_DEAD;
        act = true;
    } else if (c->state == CS_HEAD && c->first_byte) {
        act = now - c->first_byte > cfg->head_ms; /* 첫 바이트부터 재므로 조금씩 보내도 늘어나지 않는다 */
        if (act)
            exchange_reject(c, 408);
    } else if (c->state == CS_HEAD) {
        /* 종료 중이면 쉬는 연결은 바로 닫는다. 막 받은 연결은 요청이 이미 와 있을 수 있어 1초 준다 */
        long long limit = c->served ? cfg->idle_ms : cfg->head_ms;
        if (c->loop->draining)
            limit = c->served ? -1 : 1000;
        act = now - c->since > limit;
        if (act)
            c->state = CS_DEAD;
    } else if (c->state == CS_BODY) {
        act = too_slow(c, c->since, c->ex->body.size, c->last_in > c->since ? c->last_in : c->since, cfg->head_ms);
        if (act)
            exchange_error(c, 408, NULL);
    } else if (c->state == CS_RESPOND) {
        act = c->ex->deadline && now >= c->ex->deadline && c->ex->h->timeout;
        if (act)
            c->ex->h->timeout(c);
    } else if (c->state == CS_LINGER) {
        act = now - c->since > LINGER_MS;
        if (act)
            c->state = CS_DEAD;
    }
    if (act)
        conn_run(c);
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
