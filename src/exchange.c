#include "exchange.h"
#include "conn.h"
#include "loop.h"
#include "route.h"
#include "server.h"
#include "static.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stop_handler(struct conn *c)
{
    struct exchange *ex = c->ex;
    const struct handler *h = ex->h;
    ex->h = NULL;
    ex->deadline = 0;
    if (h && h->stop)
        h->stop(c);
}

void exchange_free(struct conn *c)
{
    struct exchange *ex = c->ex;
    if (!ex)
        return;
    stop_handler(c);
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

/* 아직 만들지 않은 칸: 어디로 갈 요청인지만 알려 준다 */
static void todo_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    char body[512];
    const struct route *r = ex->route;
    int n = snprintf(body, sizeof body, "%s %s로 갈 요청: path=%s body=%lld\n",
                     r->act == ACT_PROXY ? "proxy" : "cgi", r->pool ? r->pool->name : r->dir,
                     strlen(ex->path) < 300 ? ex->path : "(긴 경로)", ex->body.size);
    if (reply_text(&ex->reply, &c->out, 200, "text/plain; charset=utf-8", body, (size_t)n, NULL))
        exchange_finish(c);
    else
        exchange_abort(c);
}
static const struct handler todo_handler = {.name = "todo", .start = todo_start};

static const struct handler *handler_for(const struct route *r)
{
    switch (r->act) {
    case ACT_STATIC: return &static_handler;
    case ACT_PROXY: return &proxy_handler;
    case ACT_CGI: return &todo_handler;
    }
    return NULL;
}

/* 이 동작이 받을 수 있는 메서드. NULL = 전부 */
static const char *allowed(const struct route *r, enum http_method m)
{
    if (r->act == ACT_STATIC && m != M_GET && m != M_HEAD)
        return "Allow: GET, HEAD\r\n";
    if (r->act == ACT_CGI && m != M_GET && m != M_HEAD && m != M_POST)
        return "Allow: GET, HEAD, POST\r\n";
    return NULL;
}

static void start_handler(struct conn *c)
{
    conn_set_state(c, CS_RESPOND);
    c->ex->h->start(c);
}

/* 머리를 다 받은 요청을 어떻게 할지 정한다. 여기에는 I/O가 없다 */
static void decide(struct conn *c, int status)
{
    struct exchange *ex = c->ex;
    struct http_req *r = &ex->req;
    const struct config *cfg = c->loop->srv->cfg;
    ex->reply.minor = r->minor;
    ex->reply.head_only = r->method == M_HEAD && status == 0;
    ex->reply.keep = status == 0 && r->keep && !c->loop->draining;
    if (status == 0)
        body_init(&ex->dec, r->chunked ? BODY_CHUNKED : r->length > 0 ? BODY_LENGTH : BODY_NONE, r->length);
    if (status == 0)
        status = uri_split(r->target, r->method, &ex->target);
    if (status == 0 && r->method == M_CONNECT) {
        ex->reply.keep = false; /* 터널이라고 믿은 바이트가 다음 요청으로 읽히면 안 된다 */
        status = 501;
    }
    if (status == 0 && (ex->target.absolute || r->host.p)) /* 절대형이면 Host 헤더는 무시한다 */
        status = uri_host(ex->target.absolute ? ex->target.authority : r->host, 80, ex->host,
                          sizeof ex->host, &ex->port);
    if (status == 0 && ex->target.asterisk) {
        exchange_error(c, 204, "Allow: GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS\r\n");
        return;
    }
    if (status == 0) {
        size_t cap = ex->target.path.n + 2;
        ex->path = malloc(cap + 4);
        status = ex->path ? uri_normalize(ex->target.path, ex->path, cap + 4) : 500;
    }
    if (status == 0 && r->expect_other)
        status = 417;
    const char *allow = NULL;
    if (status == 0) {
        ex->route = route_find(cfg, ex->host, ex->path);
        if (!ex->route)
            status = 404;
        else if ((allow = allowed(ex->route, r->method)))
            status = 405;
    }
    if (status == 0 && r->length > cfg->max_body)
        status = 413; /* 길이를 미리 알면 본문을 받기 전에 거절한다 */
    if (status) {
        exchange_error(c, status, allow);
        return;
    }
    ex->h = handler_for(ex->route);
    if (ex->dec.done)
        start_handler(c);
    else
        conn_set_state(c, CS_BODY);
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
    struct exchange *ex = calloc(1, sizeof *ex);
    if (!ex) {
        conn_set_state(c, CS_DEAD);
        return true;
    }
    c->ex = ex;
    ex->start_ms = c->loop->now;
    spool_init(&ex->body, &c->loop->srv->spool_bytes);
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
