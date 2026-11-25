#include "exchange.h"
#include "app.h"
#include "conn.h"
#include "loop.h"
#include "route.h"
#include "log.h"
#include "server.h"
#include "static.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stop_handler(struct conn *c)
{
    struct exchange *ex = c->ex;
    const struct handler *h = ex->h;
    ex->h = NULL;
    ex->deadline = 0;
    if (h && ex->started && h->stop)
        h->stop(c);
}

void exchange_free(struct conn *c)
{
    struct exchange *ex = c->ex;
    if (!ex)
        return;
    stop_handler(c);
    log_access(c);
    spool_free(&ex->body);
    free(ex->head);
    free(ex->path);
    free(ex);
    c->ex = NULL;
}

void exchange_finish(struct conn *c)
{
    if (!reply_finish(&c->ex->reply, &c->out)) {
        exchange_abort(c);
        return;
    }
    stop_handler(c); /* 남은 응답을 보내는 동안 백엔드·파일을 붙잡고 있을 필요가 없다 */
    conn_set_state(c, CS_FLUSH);
}

void exchange_abort(struct conn *c)
{
    /* 닫힘을 본문의 끝으로 쓰는 응답은 정상 종료와 구별되지 않는다. 그때만 RST로 끊는다 */
    c->reset = c->ex->reply.framing == FRAME_CLOSE;
    conn_set_state(c, CS_DEAD);
}

void exchange_error(struct conn *c, int status, const char *extra)
{
    struct exchange *ex = c->ex;
    if (ex->reply.started) { /* 이미 머리를 보냈으니 상태 코드를 바꿀 수 없다 */
        exchange_abort(c);
        return;
    }
    /* 본문을 다 읽지 않았거나 형식을 믿을 수 없으면 다음 요청의 시작을 알 수 없다 → 닫는다 */
    if (!ex->dec.done || status == 400)
        ex->reply.keep = false;
    char body[256];
    int n = snprintf(body, sizeof body, "<!doctype html><meta charset=utf-8><title>%d %s</title><h1>%d %s</h1>\n",
                     status, status_text(status), status, status_text(status));
    if (reply_text(&ex->reply, &c->out, status, "text/html; charset=utf-8", body, (size_t)n, extra)) {
        stop_handler(c);
        conn_set_state(c, CS_FLUSH);
    } else {
        exchange_abort(c);
    }
}

static const struct handler *handler_for(const struct route *r)
{
    switch (r->act) {
    case ACT_STATIC: return &static_handler;
    case ACT_PROXY: return &proxy_handler;
    case ACT_APP: return app_handler;
    }
    return NULL;
}

/* 이 동작이 받을 수 있는 메서드. NULL = 전부 */
static const char *allowed(const struct route *r, enum http_method m)
{
    if (r->act == ACT_STATIC && m != M_GET && m != M_HEAD)
        return "Allow: GET, HEAD\r\n";
    return NULL;
}

static void start_handler(struct conn *c)
{
    conn_set_state(c, CS_RESPOND);
    c->ex->started = true;
    c->ex->h->start(c);
}

static int copy_path(struct exchange *ex)
{
    struct sl p = ex->target.path;
    ex->path = calloc(1, p.n + 8); /* 정리에 실패해도 항상 끝이 있는 문자열이어야 로그에 쓸 수 있다 */
    return ex->path ? uri_normalize(p, ex->path, p.n + 8) : 500;
}

/* 규칙에서 처리할 쪽을 고른다 */
static int choose(struct conn *c, const char **allow)
{
    struct exchange *ex = c->ex;
    enum http_method m = ex->req.method;
    if (m == M_CONNECT) {
        ex->reply.keep = false; /* 터널이라고 믿은 바이트가 다음 요청으로 읽히면 안 된다 */
        return 501;
    }
    int status = copy_path(ex);
    if (status)
        return status;
    if (c->origin == ORIGIN_ADMIN) {
        ex->h = &status_handler;
        *allow = m == M_GET || m == M_HEAD ? NULL : "Allow: GET, HEAD\r\n";
        return *allow ? 405 : 0;
    }
    ex->route = route_find(c->loop->srv->cfg, ex->host, ex->path);
    if (!ex->route)
        return 404;
    ex->h = handler_for(ex->route);
    *allow = allowed(ex->route, m);
    return *allow ? 405 : 0;
}

/* 머리를 다 받은 요청을 어떻게 할지 정한다. 여기에는 I/O가 없다 */
static void decide(struct conn *c, int status)
{
    struct exchange *ex = c->ex;
    struct http_req *r = &ex->req;
    const struct config *cfg = c->loop->srv->cfg;
    const char *allow = NULL;
    ex->reply.minor = r->minor;
    ex->reply.head_only = r->method == M_HEAD && status == 0;
    ex->reply.keep = status == 0 && r->keep && !c->loop->draining;
    if (status == 0) {
        body_init(&ex->dec, r->chunked ? BODY_CHUNKED : r->length > 0 ? BODY_LENGTH : BODY_NONE, r->length);
        status = uri_split(r->target, r->method, &ex->target);
    }
    /* 호스트는 요청 줄에 적힌 것(절대형, CONNECT)이 먼저, 없으면 Host 헤더 */
    struct sl authority = ex->target.authority.n ? ex->target.authority : r->host;
    if (status == 0 && authority.p)
        status = uri_host(authority, 80, ex->host, sizeof ex->host, &ex->port);
    /* 서버 전체에 대한 OPTIONS, 또는 "여기까지만 전달"(Max-Forwards: 0)이면 직접 답한다 */
    const struct field *hops = r->method == M_OPTIONS ? fields_find(&r->h, "Max-Forwards") : NULL;
    if (status == 0 && (ex->target.asterisk || (hops && sl_eq(hops->value, "0")))) {
        exchange_error(c, 204, "Allow: GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS\r\n");
        return;
    }
    if (status == 0)
        status = choose(c, &allow);
    if (status == 0 && r->expect_other)
        status = 417;
    if (status == 0 && r->length > cfg->max_body)
        status = 413; /* 길이를 미리 알면 본문을 받기 전에 거절한다 */
    if (status) {
        exchange_error(c, status, allow);
    } else if (ex->dec.done) {
        start_handler(c);
    } else {
        conn_set_state(c, CS_BODY);
    }
}

static struct exchange *new_exchange(struct conn *c)
{
    struct exchange *ex = calloc(1, sizeof *ex);
    if (!ex) {
        conn_set_state(c, CS_DEAD);
        return NULL;
    }
    ex->start_ms = c->loop->now;
    spool_init(&ex->body, &c->loop->srv->spool_bytes);
    c->ex = ex;
    return ex;
}

void exchange_reject(struct conn *c, int status)
{
    if (new_exchange(c))
        decide(c, status);
}

bool exchange_head(struct conn *c)
{
    /* 앞 요청 뒤에 붙은 빈 줄은 무시한다 (RFC 9112 2.2) */
    while (buf_len(&c->in) >= 2 && !memcmp(buf_head(&c->in), "\r\n", 2))
        buf_consume(&c->in, 2);
    size_t n = buf_len(&c->in);
    if (n == 0) {
        if (c->rd_eof)
            conn_set_state(c, CS_DEAD);
        return c->rd_eof;
    }
    if (!c->first_byte)
        c->first_byte = c->loop->now;
    enum scan s = http_scan_head(buf_head(&c->in), n, &c->scanned);
    bool full = s == SCAN_MORE && buf_room(&c->in) == 0;
    if (s == SCAN_MORE && !full) {
        if (c->rd_eof)
            conn_set_state(c, CS_DEAD); /* 머리를 보내다 떠났다 */
        return c->rd_eof;
    }
    struct exchange *ex = new_exchange(c);
    if (!ex)
        return true;
    int status = 400;
    if (full) {
        /* 첫 줄조차 안 끝났으면 주소가 너무 긴 것, 아니면 헤더가 너무 많은 것 */
        status = memchr(buf_head(&c->in), '\n', n) ? 431 : 414;
    } else if (s == SCAN_END) {
        ex->head = malloc(c->scanned);
        if (ex->head) {
            memcpy(ex->head, buf_head(&c->in), c->scanned);
            status = http_parse_request(ex->head, c->scanned, &ex->req);
        } else {
            status = 500;
        }
        buf_consume(&c->in, c->scanned);
    }
    c->scanned = 0;
    c->first_byte = 0;
    c->served++;
    decide(c, status);
    return true;
}

bool exchange_body(struct conn *c)
{
    struct exchange *ex = c->ex;
    const struct config *cfg = c->loop->srv->cfg;
    if (ex->req.expect_100 && !ex->sent_100 && buf_len(&c->in) == 0) {
        /* 클라이언트가 "보내도 되나요?"를 묻고 기다리는 중. 최종 응답을 정하지 않은 지금만 보낼 수 있다 */
        buf_append(&c->out, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    ex->sent_100 = true;
    bool moved = false;
    while (!ex->dec.done && buf_len(&c->in)) {
        struct sl data;
        long used = body_next(&ex->dec, buf_head(&c->in), buf_len(&c->in), &data);
        int status = used < 0 ? 400 : 0;
        if (!status && data.n) {
            if (ex->body.size + (long long)data.n > cfg->max_body)
                status = 413;
            else
                status = spool_append(&ex->body, data.p, data.n, cfg->max_disk);
        }
        if (status) {
            exchange_error(c, status, NULL);
            return true;
        }
        buf_consume(&c->in, (size_t)used);
        moved = true;
    }
    if (ex->dec.done) {
        start_handler(c);
        return true;
    }
    if (c->rd_eof) /* 본문을 다 보내지 않고 떠났다 */
        conn_set_state(c, CS_DEAD);
    return moved;
}
