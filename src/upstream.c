/* 리버스 프록시: 요청을 다시 써서 백엔드에 보내고, 응답을 해석해 클라이언트에게 다시 포장한다 */
#include "upstream.h"
#include "caching.h"
#include "config.h"
#include "conn.h"
#include "log.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SEND_CHUNK 16384
#define INTERIM_MAX 8

/* ---- 요청 머리 다시 쓰기 ---- */

static bool put(struct buf *out, struct sl name, struct sl value)
{
    return buf_append(out, name.p, name.n) && buf_append(out, ": ", 2) &&
           buf_append(out, value.p, value.n) && buf_append(out, "\r\n", 2);
}

bool upstream_request_head(const struct exchange *ex, const char *ip, const char *via,
                           const char *default_host, struct buf *out)
{
    const struct http_req *r = &ex->req;
    struct sl host = ex->target.absolute ? ex->target.authority
                     : r->host.p ? r->host : (struct sl){default_host, strlen(default_host)};
    bool ok = buf_append(out, r->method_text.p, r->method_text.n) && buf_append(out, " ", 1) &&
              buf_append(out, ex->path, strlen(ex->path)) &&
              buf_append(out, ex->target.query.p, ex->target.query.n) &&
              buf_printf(out, " HTTP/1.1\r\n") && put(out, (struct sl){"Host", 4}, host);
    /* 우리가 새로 쓰는 것. 클라이언트가 보낸 X-Forwarded-For는 거짓일 수 있으니 버린다 */
    static const char *const rewritten[] = {"Host", "Content-Length", "Expect", "X-Forwarded-For",
                                            "X-Forwarded-Proto", "X-Forwarded-Host"};
    for (int i = 0; ok && i < r->h.n; i++) {
        const struct field *f = &r->h.f[i];
        bool skip = fields_hop_by_hop(&r->h, f->name);
        for (size_t k = 0; !skip && k < sizeof rewritten / sizeof rewritten[0]; k++)
            skip = sl_eqi(f->name, rewritten[k]);
        /* X_Forwarded_For는 백엔드에서 X-Forwarded-For와 같은 환경 변수가 되어 위조에 쓰인다 */
        skip = skip || memchr(f->name.p, '_', f->name.n);
        long long hops;
        if (!skip && r->method == M_OPTIONS && sl_eqi(f->name, "Max-Forwards") && sl_to_ll(f->value, &hops))
            ok = buf_printf(out, "Max-Forwards: %lld\r\n", hops - 1);
        else if (!skip)
            ok = put(out, f->name, f->value);
    }
    ok = ok && buf_printf(out, "Via: 1.1 %s\r\nX-Forwarded-For: %s\r\nX-Forwarded-Proto: http\r\n", via, ip) &&
         put(out, (struct sl){"X-Forwarded-Host", 16}, host);
    /* 본문은 이미 다 받았으므로 chunked로 왔더라도 길이를 안다 */
    if (ok && (ex->body.size || r->method == M_POST || r->method == M_PUT || r->method == M_PATCH))
        ok = buf_printf(out, "Content-Length: %lld\r\n", ex->body.size);
    return ok && buf_append(out, "\r\n", 2);
}

/* ---- 백엔드 고르기와 연결 ---- */

static struct proxy_st *px_of(struct conn *c)
{
    return &c->ex->u.proxy;
}

/* 요청을 다시 보내도 두 번 처리될 위험이 없는가 */
static bool retry_safe(const struct exchange *ex)
{
    return (ex->req.method == M_GET || ex->req.method == M_HEAD) && ex->body.size == 0;
}

/* 지금 쓰던 백엔드 연결을 놓는다 */
static void release(struct conn *c, enum outcome o, bool reuse)
{
    struct proxy_st *px = px_of(c);
    if (px->up) {
        if (reuse)
            upconn_keep(px->up);
        else
            loop_bury(c->loop, &px->up->io);
        px->up = NULL;
    }
    if (px->be) {
        const char *why = pool_done(px->pool, px->be, o, c->loop->now);
        if (why)
            log_note("백엔드 제외: %s %s (%s)", px->pool->name, px->be->name, why);
        px->be = NULL;
    }
}

static void fail(struct conn *c, int status, enum outcome o)
{
    release(c, o, false);
    exchange_error(c, status, status == 503 ? "Retry-After: 1\r\n" : NULL);
}

/* 연결이 생겼다(또는 시작했다). 요청 머리를 준비한다 */
static void prepare(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    const struct config *cfg = c->loop->srv->cfg;
    px->up->c = c;
    px->body_off = 0;
    px->scanned = 0;
    px->send_broken = px->eof = px->broken = false;
    px->touched = c->loop->now;
    ex->deadline = c->loop->now + (px->connecting ? cfg->connect_ms : cfg->response_ms);
    if (!upstream_request_head(ex, c->ip, c->loop->srv->via, px->be->name, &px->up->out))
        fail(c, 431, OUT_NEUTRAL);
}

/* 다음 백엔드에 연결을 시작한다. 요청을 한 바이트도 안 보낸 상태에서만 부르므로 어떤 메서드든 안전하다 */
static void connect_next(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    for (;;) {
        struct backend *b = c->loop->now < px->give_up ? pool_pick(px->pool, px->tried, c->loop->now) : NULL;
        if (!b) {
            fail(c, px->fail_status, OUT_NEUTRAL);
            return;
        }
        px->be = b;
        ex->tries++;
        snprintf(ex->served_by, sizeof ex->served_by, "%s/%s", px->pool->name, b->name);
        px->tried |= 1u << (b - px->pool->be);
        px->fail_status = 502;
        atomic_fetch_add(&b->requests, 1);
        /* 보관된 연결은 하필 닫히는 중일 수 있다. 다시 보내도 되는 요청(GET·HEAD)만 쓴다 */
        px->up = px->fresh || !retry_safe(ex) ? NULL : upconn_reuse(c->loop, b);
        px->connecting = !px->up;
        if (px->up) {
            atomic_fetch_add(&b->reused, 1);
        } else if (!(px->up = upconn_open(c->loop, &b->addr, b))) {
            int e = errno;
            /* 포트·fd·메모리 부족은 우리 사정이다. 백엔드 장애로 세면 멀쩡한 백엔드가 모두 빠진다 */
            bool ours = e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM || e == EADDRNOTAVAIL;
            if (ours) {
                fail(c, 503, OUT_NEUTRAL);
                return;
            }
            release(c, OUT_REFUSED, false);
            continue;
        }
        prepare(c);
        return;
    }
}

/* Via에 내 이름이 있다 = 나에게 되돌아온 요청 */
static bool is_loop(const struct conn *c)
{
    const struct fields *h = &c->ex->req.h;
    for (int i = 0; i < h->n; i++) {
        if (!sl_eqi(h->f[i].name, "Via"))
            continue;
        char me[96]; /* "1.1 이름" 조각이 정확히 같은지. 앞부분만 같은 다른 서버를 나로 착각하지 않게 */
        snprintf(me, sizeof me, "1.1 %s", c->loop->srv->via);
        if (sl_list_has(h->f[i].value, me))
            return true;
    }
    return false;
}

static void proxy_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    memset(px, 0, sizeof *px);
    if (is_loop(c)) { /* 그대로 두면 연결이 다 찰 때까지 돈다 */
        exchange_error(c, 508, NULL);
        return;
    }
    if (caching_lookup(c))
        return;
    px->pool = ex->route->pool;
    px->give_up = c->loop->now + 3LL * c->loop->srv->cfg->connect_ms;
    px->fail_status = 503; /* 하나도 못 골랐다 = 모두 한도까지 찼다 */
    connect_next(c);
}

/* ---- 중계 ---- */

static bool send_request(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    struct upconn *up = px->up;
    bool moved = false;
    for (int i = 0; i < 8 && !px->send_broken; i++) {
        if (!buf_len(&up->out) && px->body_off < ex->body.size) {
            char *p = buf_reserve(&up->out, SEND_CHUNK);
            ssize_t n = p ? spool_read(&ex->body, px->body_off, p, SEND_CHUNK) : -1;
            if (n <= 0) {
                fail(c, 500, OUT_NEUTRAL);
                return true;
            }
            buf_commit(&up->out, (size_t)n);
            px->body_off += n;
        }
        if (!buf_len(&up->out))
            break;
        ssize_t w = write(up->io.fd, buf_head(&up->out), buf_len(&up->out));
        if (w > 0) {
            buf_consume(&up->out, (size_t)w);
            px->touched = c->loop->now;
            moved = true;
        } else if (errno == EAGAIN || errno == EINTR) {
            break;
        } else {
            px->send_broken = true; /* 더는 못 보낸다. 하지만 413 같은 응답이 이미 와 있을 수 있으니 읽기는 계속 */
            moved = true;
        }
    }
    return moved;
}

static bool recv_response(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    struct upconn *up = px->up;
    /* 클라이언트가 느려서 보낼 것이 쌓였으면 백엔드에서 더 읽지 않는다.
       그러면 커널 버퍼가 차고, TCP가 백엔드를 저절로 늦춘다 */
    if (px->eof || !(up->io.ready & IO_R) || !buf_room(&up->in) ||
        (px->head_done && !reply_room(&c->ex->reply, &c->out)))
        return false;
    size_t n = buf_room(&up->in);
    char *p = buf_reserve(&up->in, n);
    ssize_t r = p ? read(up->io.fd, p, n) : -1;
    if (r > 0) {
        buf_commit(&up->in, (size_t)r);
        if ((size_t)r < n)
            up->io.ready &= ~IO_R;
        if (px->head_done) /* 머리를 조금씩 흘려 보내는 것으로 제한 시간을 늘릴 수 없게 */
            px->touched = c->loop->now;
        return true;
    }
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
        up->io.ready &= ~IO_R;
        return false;
    }
    px->eof = true; /* 더 올 것이 없다 */
    px->broken = r < 0; /* 정상 닫힘이 아니라 끊김: 받은 것이 전부가 아닐 수 있다 */
    return true;
}

/* 응답 머리를 받기 전에 백엔드가 끊었다 */
static void lost_before_response(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    bool nothing = !buf_len(&px->up->in) && !px->interim;
    bool reused = px->up->reused;
    if (nothing && reused && retry_safe(c->ex) && !px->fresh) {
        /* 쉬던 연결이 하필 닫히던 중이었다. 백엔드 잘못이 아니므로 새 연결로 다시 */
        px->tried &= ~(1u << (px->be - px->pool->be));
        px->fresh = true;
        release(c, OUT_NEUTRAL, false);
        connect_next(c);
    } else if (nothing && retry_safe(c->ex) && !px->retried) {
        px->retried = true;
        release(c, OUT_BAD, false);
        connect_next(c);
    } else {
        fail(c, 502, reused ? OUT_NEUTRAL : OUT_BAD);
    }
}

/* 백엔드 응답 머리에서 클라이언트에게 그대로 넘길 헤더만 모은다 */
static bool copy_headers(const struct http_res *res, struct buf *extra, unsigned *flags)
{
    for (int i = 0; i < res->h.n; i++) {
        const struct field *f = &res->h.f[i];
        /* 본문 길이에 관한 헤더는 복사하지 않고 reply가 새로 하나만 만든다 */
        if (fields_hop_by_hop(&res->h, f->name) || sl_eqi(f->name, "Content-Length"))
            continue;
        if (sl_eqi(f->name, "Date"))
            *flags |= REPLY_HAS_DATE;
        if (sl_eqi(f->name, "Server"))
            *flags |= REPLY_HAS_SERVER;
        if (!put(extra, f->name, f->value))
            return false;
    }
    return true;
}

/* 최종 응답의 머리를 클라이언트로. false = 실패 처리까지 끝남 */
static bool start_reply(struct conn *c, const struct http_res *res)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    bool no_body = ex->reply.head_only || res->status == 204 || res->status == 304;
    enum body_kind kind = no_body ? BODY_NONE : res->chunked ? BODY_CHUNKED
                          : res->length >= 0 ? BODY_LENGTH : BODY_EOF;
    body_init(&px->dec, kind, res->length);
    /* HEAD에 본문을 붙여 보내는 백엔드가 있다. 그 본문이 늦게 오면 다음 손님의 응답으로 읽히므로 다시 쓰지 않는다 */
    px->reusable = res->keep && res->minor >= 1 && kind != BODY_EOF && !ex->reply.head_only;
    px->bad_status = res->status == 502 || res->status == 503;
    px->head_done = true;

    struct buf extra;
    buf_init(&extra, HTTP_HEAD_MAX * 2);
    unsigned flags = 0;
    bool ok = copy_headers(res, &extra, &flags);
    if (ok)
        caching_begin(c, res, &extra, flags);
    if (ok && caching_on(c))
        ok = buf_printf(&extra, "X-Cache: MISS\r\n");
    caching_invalidate(c, res->status);
    ok = ok && reply_start(&ex->reply, &c->out, res->status, res->length, buf_head(&extra), buf_len(&extra), flags);
    buf_free(&extra);
    if (!ok)
        fail(c, 502, OUT_NEUTRAL);
    return ok;
}

/* 103 같은 중간 응답은 HTTP/1.1 클라이언트에게 그대로 넘긴다. 최종 응답은 아직 안 왔다 */
static void pass_interim(struct conn *c, const struct http_res *res)
{
    struct buf extra;
    unsigned flags = 0;
    buf_init(&extra, HTTP_HEAD_MAX);
    if (c->ex->req.minor >= 1 && copy_headers(res, &extra, &flags) &&
        buf_room(&c->out) > buf_len(&extra) + 64) {
        buf_printf(&c->out, "HTTP/1.1 %d Early Hints\r\n", res->status);
        buf_append(&c->out, buf_head(&extra), buf_len(&extra));
        buf_append(&c->out, "\r\n", 2);
    }
    buf_free(&extra);
}

static bool read_head(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    struct buf *in = &px->up->in;
    bool moved = false;
    while (!px->head_done) {
        enum scan s = buf_len(in) ? http_scan_head(buf_head(in), buf_len(in), &px->scanned) : SCAN_MORE;
        if (s == SCAN_MORE && buf_len(in) < HTTP_HEAD_MAX) {
            if (!px->eof)
                return moved;
            lost_before_response(c);
            return true;
        }
        struct http_res res;
        if (s != SCAN_END || px->scanned > HTTP_HEAD_MAX ||
            !http_parse_response(buf_head(in), px->scanned, &res) || res.status == 101 ||
            (res.status < 200 && ++px->interim > INTERIM_MAX)) {
            fail(c, 502, OUT_BAD);
            return true;
        }
        if (res.status >= 200) {
            if (!start_reply(c, &res))
                return true;
        } else if (res.status == 103) {
            pass_interim(c, &res);
        }
        buf_consume(in, px->scanned);
        px->scanned = 0;
        moved = true;
    }
    return moved;
}

static bool relay_body(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    struct buf *in = &px->up->in;
    bool moved = false;
    while (!px->dec.done && buf_len(in)) {
        size_t n = reply_room(&ex->reply, &c->out);
        if (n == 0)
            return moved;
        if (n > buf_len(in))
            n = buf_len(in);
        struct sl data;
        long used = body_next(&px->dec, buf_head(in), n, &data);
        if (used < 0) {
            release(c, OUT_BAD, false);
            exchange_abort(c);
            return true;
        }
        reply_write(&ex->reply, &c->out, data.p, data.n);
        caching_add(px, data);
        buf_consume(in, (size_t)used);
        moved = true;
    }
    if (px->dec.done)
        caching_commit(c);
    if (px->dec.done || (px->eof && !px->broken && px->dec.kind == BODY_EOF)) {
        bool sent_all = !px->send_broken && !buf_len(&px->up->out) && px->body_off == ex->body.size;
        /* 응답 뒤에 남은 바이트가 있는 연결을 다시 쓰면 다음 손님에게 엉뚱한 응답이 간다 */
        bool reuse = px->reusable && sent_all && !px->eof && !buf_len(in) && !c->loop->draining;
        release(c, px->bad_status ? OUT_BAD : OUT_OK, reuse);
        exchange_finish(c);
        return true;
    }
    if (px->eof) { /* 약속한 길이를 다 못 받고 끊겼다. 클라이언트에게도 끊어서 알린다 */
        release(c, OUT_BAD, false);
        exchange_abort(c);
        return true;
    }
    return moved;
}

static bool proxy_pump(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct proxy_st *px = px_of(c);
    const struct config *cfg = c->loop->srv->cfg;
    bool moved = false;
    if (px->connecting) {
        if (!(px->up->io.ready & IO_W))
            return false;
        int err = net_error(px->up->io.fd);
        if (err) {
            release(c, err == ETIMEDOUT ? OUT_CONNECT_TIMEOUT : OUT_REFUSED, false);
            connect_next(c);
            return true;
        }
        px->connecting = false;
        px->touched = c->loop->now;
        moved = true;
    }
    struct upconn *up = px->up;
    moved |= send_request(c);
    if (c->state != CS_RESPOND || px->up != up)
        return true;
    moved |= recv_response(c);
    if (!px->head_done)
        moved |= read_head(c);
    if (c->state != CS_RESPOND || px->up != up)
        return true;
    if (px->head_done)
        moved |= relay_body(c);
    if (c->state != CS_RESPOND || px->up != up)
        return true;
    if (!px->head_done) {
        ex->deadline = px->touched + cfg->response_ms;
    } else if (reply_room(&ex->reply, &c->out) == 0) {
        px->touched = c->loop->now; /* 지금 늦는 쪽은 클라이언트다. 백엔드를 탓하지 않는다 */
        ex->deadline = 0;
    } else {
        ex->deadline = px->touched + cfg->stall_ms;
    }
    return moved;
}

static void proxy_interest(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    struct upconn *up = px->up;
    if (!up)
        return;
    unsigned want = 0;
    if (px->connecting) {
        want = IO_W;
    } else {
        if (!px->eof && buf_room(&up->in) && !(px->head_done && !reply_room(&c->ex->reply, &c->out)))
            want |= IO_R;
        if (!px->send_broken && (buf_len(&up->out) || px->body_off < c->ex->body.size))
            want |= IO_W;
    }
    loop_watch(c->loop, &up->io, want);
}

static void proxy_timeout(struct conn *c)
{
    struct proxy_st *px = px_of(c);
    if (px->connecting) {
        px->fail_status = 504;
        release(c, OUT_CONNECT_TIMEOUT, false);
        int keep = px->fail_status;
        connect_next(c);
        if (c->state == CS_RESPOND)
            px->fail_status = keep;
    } else if (!px->head_done) {
        /* 느린 주소 하나 때문에 멀쩡한 백엔드를 빼면 안 되므로 장애로 세지 않는다.
           정말 멈춘 백엔드는 헬스 체크와 동시 상한이 막는다 */
        fail(c, 504, OUT_NEUTRAL);
    } else {
        release(c, OUT_NEUTRAL, false);
        exchange_abort(c);
    }
}

static void proxy_stop(struct conn *c)
{
    caching_drop(px_of(c));
    release(c, OUT_NEUTRAL, false);
}

const struct handler proxy_handler = {
    .name = "proxy", .start = proxy_start, .pump = proxy_pump, .interest = proxy_interest,
    .timeout = proxy_timeout, .stop = proxy_stop,
};
