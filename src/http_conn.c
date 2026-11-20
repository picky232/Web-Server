/*
 * http_conn.c — 바이트 스트림에서 HTTP 요청 경계를 찾아내는 곳.
 *
 * recv() 는 "요청 1개" 를 주지 않는다. 헤더가 3조각으로 올 수도 있고,
 * 요청 2개가 한 번에 올 수도 있다. 그래서:
 *   1) "\r\n\r\n" 이 보일 때까지 buf 에 계속 이어 붙인다 (상한 8 KiB).
 *   2) head 를 파싱해 Content-Length 를 알아낸다.
 *   3) 본문은 malloc 한 버퍼로 옮기고, 모자란 만큼 더 recv 한다.
 *   4) 남은 바이트는 다음 요청을 위해 http_conn_consume() 이 앞으로 당긴다.
 */
#include "http_conn.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "net_io.h"

void http_conn_init(http_conn_t *c, int fd)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
    snprintf(c->client_ip, sizeof c->client_ip, "%s", "?");

    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &a->sin_addr, c->client_ip, sizeof c->client_ip);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            inet_ntop(AF_INET6, &a->sin6_addr, c->client_ip, sizeof c->client_ip);
        }
    }
}

void http_request_free(http_request_t *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

void http_conn_consume(http_conn_t *c)
{
    if (c->consumed > c->len)
        c->consumed = c->len;
    memmove(c->buf, c->buf + c->consumed, c->len - c->consumed);
    c->len -= c->consumed;
    c->consumed = 0;
}

/* 요청 사이의 빈 줄(CRLF)은 무시해도 된다 (RFC 9112 §2.2) */
static void skip_leading_crlf(http_conn_t *c)
{
    size_t skip = 0;
    while (skip + 1 < c->len && c->buf[skip] == '\r' && c->buf[skip + 1] == '\n')
        skip += 2;
    if (skip > 0) {
        memmove(c->buf, c->buf + skip, c->len - skip);
        c->len -= skip;
    }
}

read_result_t http_read_request(http_conn_t *c, http_request_t *req,
                                const http_limits_t *lim, int *status)
{
    memset(req, 0, sizeof *req);
    *status = 0;
    int64_t deadline = now_ms() + lim->read_timeout_ms;

    /* ---- 1단계: head 끝("\r\n\r\n")이 버퍼에 나타날 때까지 이어 받기 ---- */
    skip_leading_crlf(c);
    size_t head_end = http_find_head_end(c->buf, c->len);
    while (head_end == 0) {
        if (c->len == sizeof c->buf) {
            /* 8 KiB 를 다 채웠는데도 head 가 끝나지 않음.
             * 첫 줄조차 끝나지 않았다면 URI 가 너무 긴 것(414), 아니면 헤더 과다(431). */
            bool has_line = false;
            for (size_t i = 0; i + 1 < c->len; i++)
                if (c->buf[i] == '\r' && c->buf[i + 1] == '\n') {
                    has_line = true;
                    break;
                }
            *status = has_line ? 431 : 414;
            return READ_BAD;
        }
        ssize_t n = recv_until_deadline(c->fd, c->buf + c->len,
                                        sizeof c->buf - c->len, deadline);
        if (n == IO_TIMEOUT) {
            if (c->len == 0)
                return READ_CLOSED; /* 아무것도 안 보낸 연결은 조용히 닫는다 */
            *status = 408;          /* 보내다 멈춘 요청 (slowloris 류) */
            return READ_BAD;
        }
        if (n == 0)
            return c->len == 0 ? READ_CLOSED : READ_ABORTED; /* 조기 종료 */
        if (n < 0)
            return READ_ABORTED;
        c->len += (size_t)n;
        skip_leading_crlf(c);
        head_end = http_find_head_end(c->buf, c->len);
    }

    /* ---- 2단계: head 파싱 (버퍼 안에서 제자리 파싱) ---- */
    int st = http_parse_request_head(c->buf, head_end, req);
    if (st != 0) {
        *status = st;
        return READ_BAD;
    }
    c->consumed = head_end;

    if (!req->has_content_length || req->content_length == 0)
        return READ_OK; /* 본문 없음 (GET 이나 Content-Length 없는 POST) */

    /* ---- 3단계: 본문 받기 ---- */
    if (req->content_length > lim->max_body_bytes) {
        *status = 413; /* 본문을 받기 전에 거부 → 메모리를 쓰지 않는다 */
        return READ_BAD;
    }
    size_t need = (size_t)req->content_length;
    req->body = malloc(need);
    if (req->body == NULL) {
        *status = 500;
        return READ_BAD;
    }

    /* head 와 같은 recv 로 이미 들어온 본문 앞부분을 옮긴다 */
    size_t in_buf = c->len - head_end;
    size_t take = in_buf < need ? in_buf : need;
    memcpy(req->body, c->buf + head_end, take);
    req->body_len = take;
    c->consumed = head_end + take;

    if (req->body_len < need && req->expect_continue) {
        /* curl 은 큰 POST 에 "Expect: 100-continue" 를 붙이고 이 답을 기다린다 */
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all(c->fd, cont, sizeof cont - 1, deadline) != 0)
            return READ_ABORTED;
    }

    while (req->body_len < need) {
        ssize_t n = recv_until_deadline(c->fd, req->body + req->body_len,
                                        need - req->body_len, deadline);
        if (n == IO_TIMEOUT) {
            *status = 408;
            return READ_BAD;
        }
        if (n <= 0)
            return READ_ABORTED; /* 본문 도중 연결 종료 */
        req->body_len += (size_t)n;
    }
    return READ_OK;
}
