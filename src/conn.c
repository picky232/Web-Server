#include "conn.h"
#include "config.h"
#include "net.h"
#include "proxy.h"
#include "response.h"
#include "router.h"
#include "static.h"
#include "threadpool.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SLICE_MS 100 /* 오래 기다릴 때도 이 간격마다 깨어나 종료 신호를 확인한다 */

/* 헤더 블록이 HEAD_MAX를 넘었을 때: 요청 줄부터 너무 길면 414, 헤더가 많으면 431 */
static int too_big_status(const struct conn *c)
{
    size_t n = c->len < HEAD_MAX ? c->len : HEAD_MAX;
    return memchr(c->buf, '\n', n) ? 431 : 414;
}

/* 요청 하나의 헤더 블록을 다 받을 때까지 읽는다.
   1 = c->req에 요청 준비됨, 0 = 응답 없이 닫기(EOF·유휴 시간 초과·종료), 그 외 = 보낼 오류 코드 */
static int read_request(struct conn *c, int first)
{
    long long idle_start = net_now_ms();
    if (c->len > 0)
        c->req_start = idle_start; /* 앞 요청과 같이 도착한 바이트가 이미 있다 (파이프라인) */

    for (;;) {
        size_t hl = http_find_head_end(c->buf, c->len);
        if (hl > HEAD_MAX || (!hl && c->len >= HEAD_MAX))
            return too_big_status(c);
        if (hl) {
            int st = http_parse_request(c->buf, hl, &c->req);
            memmove(c->buf, c->buf + hl, c->len - hl); /* 헤더는 req.raw로 복사됐으니 버퍼에서 뺀다 */
            c->len -= hl;
            return st ? st : 1;
        }

        /* 아직 빈 줄이 안 왔다. 하나도 안 왔으면 유휴 시간, 일부 왔으면 read-timeout */
        long long deadline = c->len == 0
            ? idle_start + (first ? g_cfg.read_timeout_ms : g_cfg.keepalive_ms)
            : c->req_start + g_cfg.read_timeout_ms;
        long long left = deadline - net_now_ms();
        if (left <= 0)
            return c->len ? 408 : 0;
        if (g_stop)
            return 0;
        int w = net_wait(c->fd, POLLIN, left < SLICE_MS ? (int)left : SLICE_MS);
        if (w < 0)
            return 0;
        if (w == 0) {
            /* 한 간격(0.1초) 동안 아무것도 안 온 유휴 연결인데 큐에 새 연결이 기다리면,
               이 연결을 닫고 스레드를 비켜 준다. 데이터가 없을 때만 판단해야 도착한 요청을 안 버린다 */
            if (c->len == 0 && !first && pool_waiting() > 0)
                return 0;
            continue;
        }
        ssize_t r = recv(c->fd, c->buf + c->len, CONN_BUF - c->len, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0; /* 클라이언트가 닫았거나 끊었다. 보낼 곳이 없으니 조용히 닫는다 */
        if (c->len == 0)
            c->req_start = net_now_ms();
        c->len += (size_t)r;
    }
}

int conn_read_body(struct conn *c, int out_fd)
{
    int out_failed = 0;
    while (c->body_left > 0) {
        if (c->len == 0) {
            /* 클라이언트가 "Expect: 100-continue"로 허락을 기다리는 중이면 먼저 보내 준다 */
            if (c->req.expect_continue && !c->sent_continue) {
                const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (net_send_all(c->fd, cont, sizeof cont - 1) < 0)
                    return -1;
                c->sent_continue = 1;
            }
            long long left = c->req_start + g_cfg.read_timeout_ms - net_now_ms();
            if (left <= 0 || net_wait(c->fd, POLLIN, (int)left) <= 0)
                return -1;
            ssize_t r = recv(c->fd, c->buf, CONN_BUF, 0);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                return -1;
            c->len = (size_t)r;
        }
        size_t n = c->len;
        if ((long long)n > c->body_left)
            n = (size_t)c->body_left; /* 뒤쪽 바이트는 다음 요청 몫이라 남긴다 */
        if (out_fd >= 0 && !out_failed && net_send_all(out_fd, c->buf, n) < 0)
            out_failed = 1; /* 보낼 곳이 실패해도 본문은 끝까지 읽어 연결을 깨끗이 둔다 */
        memmove(c->buf, c->buf + n, c->len - n);
        c->len -= n;
        c->body_left -= (long long)n;
    }
    return out_failed ? -2 : 0;
}

static int is_head(const struct conn *c)
{
    return c->req.method && !strcmp(c->req.method, "HEAD");
}

/* 요청 하나에 응답한다. 보낸 상태 코드를 돌려주고, 연결이 끊겼으면 -1 */
static int dispatch(struct conn *c)
{
    char path[URI_MAX + 1];
    int st = http_decode_path(c->req.target, path, sizeof path);
    if (st) {
        if (st != 403)
            c->keep = 0;
        return send_error(c->fd, st, c->keep, NULL, is_head(c)) < 0 ? -1 : st;
    }
    if (router_is_proxy(path))
        return proxy_forward(c); /* /api/... → 백엔드. 본문은 프록시가 백엔드로 흘려보낸다 */
    if (conn_read_body(c, -1) < 0) /* 정적 파일은 본문을 쓰지 않으니 먼저 버린다 */
        return -1;
    return static_serve(c, path);
}

static void access_log(const struct conn *c, int status)
{
    if (!g_cfg.quiet)
        fprintf(stderr, "%s \"%s %s\" %d\n", c->ip, c->req.method ? c->req.method : "-",
                c->req.target ? c->req.target : "-", status);
}

void conn_handle(int fd, const char *ip)
{
    struct conn *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return;
    }
    c->fd = fd;
    snprintf(c->ip, sizeof c->ip, "%s", ip);

    for (int n = 0;; n++) {
        int r = read_request(c, n == 0);
        if (r == 0) {
            close(fd);
            break;
        }
        if (r != 1) {
            /* 형식이 깨진 요청: 어디까지가 이 요청인지 믿을 수 없으니 응답 후 닫는다 */
            send_error(fd, r, 0, NULL, 0);
            access_log(c, r);
            net_close_lingering(fd);
            break;
        }
        c->body_left = c->req.content_length;
        c->sent_continue = 0;
        if (c->req.content_length > g_cfg.max_body) {
            send_error(fd, 413, 0, NULL, 0); /* 본문을 받기 전에 거절한다 */
            access_log(c, 413);
            net_close_lingering(fd);
            break;
        }
        /* 큐에 기다리는 연결이 있으면 이번 응답에 "Connection: close"를 실어 보낸다.
           말없이 닫으면 클라이언트가 이미 보낸 다음 요청이 허공에 뜬다 */
        c->keep = c->req.keep_alive && n + 1 < g_cfg.max_requests && !g_stop &&
                  pool_waiting() == 0;

        int st = dispatch(c);
        if (st < 0) {
            close(fd);
            break;
        }
        access_log(c, st);
        /* 핸들러가 본문을 안 읽었으면 여기서 버린다. 안 버리면 다음 요청 자리에 본문이 남는다 */
        if (c->keep && c->body_left > 0 && conn_read_body(c, -1) < 0) {
            close(fd);
            break;
        }
        if (!c->keep || g_stop) {
            net_close_lingering(fd);
            break;
        }
    }
    free(c);
}
