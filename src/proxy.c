#include "proxy.h"
#include "config.h"
#include "net.h"
#include "response.h"
#include "router.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RESP_HEAD_MAX 16384

/* 이 연결(클라이언트↔프록시, 프록시↔백엔드) 한 구간에서만 의미가 있는 헤더 (hop-by-hop).
   다음 구간으로 넘기면 안 된다 */
static const char *const HOP_BY_HOP[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE",
                                         "Trailer", "Transfer-Encoding", "Upgrade", NULL};
/* 백엔드로 보낼 때 우리가 새로 쓰는 헤더. 클라이언트가 보낸 값은 버린다 (X-Forwarded-For 위조 방지) */
static const char *const REWRITTEN[] = {"Host", "Expect", "X-Forwarded-For", "X-Forwarded-Host",
                                        "X-Forwarded-Proto", NULL};

static int in_list(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (!strcasecmp(name, *list))
            return 1;
    return 0;
}

/* 고정 크기 버퍼에 문자열 이어 붙이기. 넘치면 overflow만 표시하고 멈춘다 */
struct out {
    char *buf;
    size_t len, cap;
    int overflow;
};

static void add(struct out *o, const char *s)
{
    size_t n = strlen(s);
    if (o->overflow || o->len + n > o->cap) {
        o->overflow = 1;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void add_header(struct out *o, const char *name, const char *value)
{
    add(o, name);
    add(o, ": ");
    add(o, value);
    add(o, "\r\n");
}

/* 응답을 시작하기 전에 실패했을 때: 오류 응답을 보내고 클라이언트 연결도 닫게 한다 */
static int fail(struct conn *c, int status, int bfd, int head)
{
    if (bfd >= 0)
        close(bfd);
    c->keep = 0;
    return send_error(c->fd, status, 0, NULL, head) < 0 ? -1 : status;
}

/* 클라이언트 요청을 백엔드용으로 다시 쓴다: Host 교체, hop-by-hop 제거, X-Forwarded-* 추가 */
static void build_request(const struct conn *c, const struct backend *b, struct out *q)
{
    const struct request *req = &c->req;
    add(q, req->method);
    add(q, " ");
    add(q, req->target);
    add(q, " HTTP/1.1\r\n");
    add_header(q, "Host", b->hostport);
    const char *conn_hdr = http_header(req, "Connection");
    for (int i = 0; i < req->nheaders; i++) {
        const char *name = req->headers[i].name;
        /* "Connection: X-Secret" 처럼 Connection에 적힌 이름도 이 구간 전용이라 뺀다 */
        if (in_list(name, HOP_BY_HOP) || in_list(name, REWRITTEN) ||
            (conn_hdr && http_list_has(conn_hdr, name)))
            continue;
        add_header(q, name, req->headers[i].value);
    }
    add_header(q, "X-Forwarded-For", c->ip);
    const char *host = http_header(req, "Host");
    if (host)
        add_header(q, "X-Forwarded-Host", host);
    add(q, "X-Forwarded-Proto: http\r\n");
    add(q, "Connection: close\r\n\r\n"); /* 백엔드 연결은 요청마다 새로 연다 → 응답 끝 = 백엔드가 닫을 때 */
}

int proxy_forward(struct conn *c)
{
    const struct request *req = &c->req;
    int head = !strcmp(req->method, "HEAD");
    if (!head && strcmp(req->method, "GET") && strcmp(req->method, "POST"))
        return send_error(c->fd, 405, c->keep, "Allow: GET, HEAD, POST\r\n", 0) < 0 ? -1 : 405;

    int idx = router_next_backend();
    if (idx < 0)
        return fail(c, 502, -1, head);
    const struct backend *b = &g_cfg.backends[idx];
    int bfd = net_connect(b->host, b->port, g_cfg.proxy_timeout_ms);
    if (bfd < 0)
        return fail(c, errno == ETIMEDOUT ? 504 : 502, -1, head); /* 꺼져 있으면 바로 거절됨 → 502 */
    struct timeval tv = {.tv_sec = g_cfg.proxy_timeout_ms / 1000,
                         .tv_usec = (g_cfg.proxy_timeout_ms % 1000) * 1000};
    setsockopt(bfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    /* 1) 요청 헤더 + 본문을 백엔드로 */
    char qbuf[2 * HEAD_MAX + 512];
    struct out q = {qbuf, 0, sizeof qbuf, 0};
    build_request(c, b, &q);
    if (q.overflow)
        return fail(c, 431, bfd, head);
    if (net_send_all(bfd, q.buf, q.len) < 0)
        return fail(c, 502, bfd, head);
    int br = conn_read_body(c, bfd);
    if (br == -1) {
        close(bfd);
        return -1;
    }
    if (br == -2)
        return fail(c, 502, bfd, head);

    /* 2) 백엔드 응답 헤더를 빈 줄까지 받는다. 제한 시간 안에 안 오면 504 */
    char rb[RESP_HEAD_MAX];
    size_t rlen = 0, hl;
    while (!(hl = http_find_head_end(rb, rlen))) {
        if (rlen == sizeof rb)
            return fail(c, 502, bfd, head);
        int w = net_wait(bfd, POLLIN, g_cfg.proxy_timeout_ms);
        if (w <= 0)
            return fail(c, w == 0 ? 504 : 502, bfd, head);
        ssize_t r = recv(bfd, rb + rlen, sizeof rb - rlen, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return fail(c, 502, bfd, head); /* 응답 없이 끊음 */
        rlen += (size_t)r;
    }

    /* 3) 응답 헤더 해석 + 클라이언트용으로 다시 쓰기 */
    char h[RESP_HEAD_MAX + 1];
    memcpy(h, rb, hl);
    h[hl] = '\0';
    char *line = h, *eol = strstr(line, "\r\n");
    *eol = '\0';
    if (strncmp(line, "HTTP/1.", 7) || strlen(line) < 12 || line[8] != ' ' ||
        strspn(line + 9, "0123456789") != 3 || (line[12] != ' ' && line[12] != '\0'))
        return fail(c, 502, bfd, head);
    int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    line[12] = '\0';

    char obuf[RESP_HEAD_MAX + 256];
    struct out o = {obuf, 0, sizeof obuf, 0};
    add(&o, "HTTP/1.1 ");
    add(&o, line + 9);
    add(&o, " ");
    add(&o, eol > line + 12 ? line + 13 : status_text(status));
    add(&o, "\r\n");
    long long clen = -1;
    int chunked = 0;
    for (line = eol + 2; (eol = strstr(line, "\r\n")) && eol != line; line = eol + 2) {
        *eol = '\0';
        char *colon = strchr(line, ':');
        if (!colon)
            return fail(c, 502, bfd, head);
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if (!strcasecmp(line, "Content-Length")) {
            if (!*v || strspn(v, "0123456789") != strlen(v) || strlen(v) > 15)
                return fail(c, 502, bfd, head);
            sscanf(v, "%lld", &clen);
        }
        if (!strcasecmp(line, "Transfer-Encoding"))
            chunked = 1; /* chunked는 풀지 않고 그대로 흘려보낸다. 끝은 백엔드가 닫을 때 */
        else if (in_list(line, HOP_BY_HOP))
            continue;
        add_header(&o, line, v);
    }
    int no_body = head || status / 100 == 1 || status == 204 || status == 304;
    int until_eof = !no_body && (chunked || clen < 0);
    if (until_eof)
        c->keep = 0; /* 길이를 모르는 응답이라 클라이언트에게도 "닫힐 때가 끝"이라고 알려야 한다 */
    add(&o, c->keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    if (o.overflow)
        return fail(c, 502, bfd, head);
    if (net_send_all(c->fd, o.buf, o.len) < 0) {
        close(bfd);
        return -1;
    }

    /* 4) 본문 중계: 이미 받아 둔 부분부터, 나머지는 받는 대로 바로 보낸다 */
    if (!no_body) {
        long long left = until_eof ? -1 : clen;
        size_t avail = rlen - hl;
        if (left >= 0 && (long long)avail > left)
            avail = (size_t)left;
        if (avail && net_send_all(c->fd, rb + hl, avail) < 0) {
            close(bfd);
            return -1;
        }
        if (left > 0)
            left -= (long long)avail;
        char buf[65536];
        while (left != 0) {
            if (net_wait(bfd, POLLIN, g_cfg.proxy_timeout_ms) <= 0) {
                c->keep = 0; /* 이미 응답을 시작했으니 504로 바꿀 수 없다. 닫아서 끝을 알린다 */
                break;
            }
            size_t want = (left < 0 || left > (long long)sizeof buf) ? sizeof buf : (size_t)left;
            ssize_t r = recv(bfd, buf, want, 0);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                if (left > 0)
                    c->keep = 0; /* 약속한 길이보다 일찍 끊김 → 클라이언트 연결도 닫는다 */
                break;
            }
            if (net_send_all(c->fd, buf, (size_t)r) < 0) {
                close(bfd);
                return -1;
            }
            if (left > 0)
                left -= r;
        }
    }
    close(bfd);
    return status;
}
