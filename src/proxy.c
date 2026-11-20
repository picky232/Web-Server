/*
 * proxy.c — reverse proxy: 클라이언트 요청을 고정 백엔드로 전달하고 응답을 중계.
 *
 * 흐름 (proxy_forward)
 *   1) target 이 ".." 등으로 proxy prefix 밖을 가리키지 않는지 확인
 *   2) round-robin 으로 백엔드 선택 → connect (타임아웃 있음)
 *   3) 요청 head 재작성: Host 교체, hop-by-hop 제거, X-Forwarded-* 추가,
 *      Content-Length 재계산, "Connection: close" (백엔드 연결은 요청마다 새로)
 *   4) 백엔드 응답 head 수신/파싱 → 클라이언트용 head 재작성
 *   5) 본문 중계: Content-Length 만큼, 또는 백엔드가 닫을 때까지
 *   6) 백엔드 fd 는 어떤 경로로 끝나도 close
 *
 * 오류 매핑
 *   connect 거부/실패, 잘못된 응답, 응답 전 연결 종료  → 502 Bad Gateway
 *   connect/응답 대기 시간 초과                        → 504 Gateway Timeout
 *   응답 본문을 보내기 시작한 뒤의 실패 → 상태 코드를 바꿀 수 없으므로 연결을 닫는다
 */
#include "proxy.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "log.h"
#include "net_io.h"
#include "router.h"
#include "static_files.h"

/* 크기 제한이 있는 문자열 빌더: 넘치면 overflow 만 표시하고 더 쓰지 않는다 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} strbuf_t;

__attribute__((format(printf, 2, 3))) static void sb_append(strbuf_t *sb, const char *fmt, ...)
{
    if (sb->overflow)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len)
        sb->overflow = true;
    else
        sb->len += (size_t)n;
}

int proxy_init(proxy_t *p, const server_config_t *cfg, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    atomic_init(&p->next, 0);
    p->timeout_ms = cfg->proxy_timeout_ms;
    p->prefix = cfg->proxy_prefix;

    for (int i = 0; i < cfg->backend_count; i++) {
        const backend_spec_t *spec = &cfg->backends[i];
        char port[16];
        snprintf(port, sizeof port, "%d", spec->port);

        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(spec->host, port, &hints, &res);
        if (rc != 0 || res == NULL) {
            snprintf(err, errlen, "cannot resolve backend %s:%s: %s", spec->host, port,
                     rc != 0 ? gai_strerror(rc) : "no address");
            return -1;
        }
        proxy_backend_t *b = &p->backends[i];
        memcpy(&b->addr, res->ai_addr, res->ai_addrlen);
        b->addrlen = res->ai_addrlen;
        snprintf(b->host_header, sizeof b->host_header, "%s:%d", spec->host, spec->port);
        freeaddrinfo(res);
        p->count++;
    }
    return 0;
}

bool proxy_is_hop_by_hop(const char *name, const char *connection_value)
{
    static const char *const hop[] = {
        "Connection",          "Keep-Alive", "Proxy-Connection", "TE",
        "Trailer",             "Transfer-Encoding", "Upgrade", "Proxy-Authenticate",
        "Proxy-Authorization", NULL};
    for (int i = 0; hop[i] != NULL; i++)
        if (strcasecmp(name, hop[i]) == 0)
            return true;
    /* "Connection: X-Foo" 처럼 Connection 에 나열된 헤더도 이 구간 전용이다 */
    return connection_value != NULL && http_header_has_token(connection_value, name);
}

/* 클라이언트 헤더 중 프록시가 직접 다시 쓰는 것들 (그대로 복사하지 않음) */
static bool is_rewritten_request_header(const char *name)
{
    static const char *const rewritten[] = {"Host", "Content-Length", "Expect",
                                            "X-Forwarded-For", "X-Forwarded-Host",
                                            "X-Forwarded-Proto", NULL};
    for (int i = 0; rewritten[i] != NULL; i++)
        if (strcasecmp(name, rewritten[i]) == 0)
            return true;
    return false;
}

/* 3) 백엔드로 보낼 요청 head 를 만들고 본문과 함께 전송. 반환: 0 또는 오류 상태 */
static int send_backend_request(const proxy_t *p, const proxy_backend_t *be, int bfd,
                                const http_conn_t *client, const http_request_t *req)
{
    char head[HTTP_MAX_HEAD_BYTES + 1024];
    strbuf_t sb = {.buf = head, .cap = sizeof head, .len = 0, .overflow = false};

    sb_append(&sb, "%s %s HTTP/1.1\r\n", req->method_str, req->target);
    sb_append(&sb, "Host: %s\r\n", be->host_header);

    const char *conn_hdr = http_header_get(req->headers, req->header_count, "Connection");
    for (size_t i = 0; i < req->header_count; i++) {
        const http_header_t *h = &req->headers[i];
        if (proxy_is_hop_by_hop(h->name, conn_hdr) || is_rewritten_request_header(h->name))
            continue;
        sb_append(&sb, "%s: %s\r\n", h->name, h->value);
    }

    sb_append(&sb, "X-Forwarded-For: %s\r\n", client->client_ip);
    const char *orig_host = http_header_get(req->headers, req->header_count, "Host");
    if (orig_host != NULL)
        sb_append(&sb, "X-Forwarded-Host: %s\r\n", orig_host);
    sb_append(&sb, "X-Forwarded-Proto: http\r\n");
    if (req->body_len > 0 || req->method == HTTP_METHOD_POST)
        sb_append(&sb, "Content-Length: %zu\r\n", req->body_len);
    sb_append(&sb, "Connection: close\r\n\r\n");
    if (sb.overflow)
        return 431;

    int64_t deadline = now_ms() + p->timeout_ms;
    int r = send_all(bfd, head, sb.len, deadline);
    if (r == 0 && req->body_len > 0)
        r = send_all(bfd, req->body, req->body_len, deadline);
    if (r == IO_TIMEOUT)
        return 504;
    return r == 0 ? 0 : 502;
}

/* 4) 백엔드 응답 head 를 "\r\n\r\n" 까지 받는다. 반환: 0 또는 502/504 */
static int read_backend_head(const proxy_t *p, int bfd, char *rbuf, size_t cap,
                             size_t *rlen, size_t *head_len)
{
    int64_t deadline = now_ms() + p->timeout_ms;
    *rlen = 0;
    while ((*head_len = http_find_head_end(rbuf, *rlen)) == 0) {
        if (*rlen == cap)
            return 502; /* head 가 너무 큼 */
        ssize_t n = recv_until_deadline(bfd, rbuf + *rlen, cap - *rlen, deadline);
        if (n == IO_TIMEOUT)
            return 504; /* 백엔드가 느림 */
        if (n <= 0)
            return 502; /* 응답 전에 끊김 */
        *rlen += (size_t)n;
    }
    return 0;
}

typedef enum { BODY_NONE, BODY_LENGTH, BODY_UNTIL_EOF } body_mode_t;

response_info_t proxy_forward(proxy_t *p, http_conn_t *client, const http_request_t *req,
                              bool keep_alive, int client_timeout_ms)
{
    response_info_t info = {.status = 0, .body_bytes = 0, .keep_open = keep_alive};
    const int cfd = client->fd;
    const bool head_req = (req->method == HTTP_METHOD_HEAD);
    int bfd = -1;
    int status = 0;

    /* ---- 1) "/api/../secret" 처럼 prefix 를 벗어나는 경로 차단 ---- */
    char norm[HTTP_MAX_TARGET_LEN + 16];
    status = static_normalize_target(req->target, norm, sizeof norm, true);
    if (status == 0 && !router_prefix_matches(norm, p->prefix))
        status = 403;
    if (status != 0)
        goto send_error;

    /* ---- 2) 백엔드 선택 (round-robin) 과 연결 ---- */
    unsigned idx = atomic_fetch_add(&p->next, 1u) % (unsigned)p->count;
    const proxy_backend_t *be = &p->backends[idx];
    int cerr = 0;
    bfd = connect_with_timeout((const struct sockaddr *)&be->addr, be->addrlen,
                               p->timeout_ms, &cerr);
    if (bfd < 0) {
        status = (cerr == ETIMEDOUT) ? 504 : 502;
        log_error("proxy: connect to %s failed: %s", be->host_header, strerror(cerr));
        goto send_error;
    }
    set_tcp_nodelay(bfd); /* 요청 head 와 본문을 따로 보내므로 (server.c 주석 참고) */

    /* ---- 3) 요청 전달 ---- */
    status = send_backend_request(p, be, bfd, client, req);
    if (status != 0) {
        log_error("proxy: sending request to %s failed (%d)", be->host_header, status);
        goto send_error;
    }

    /* ---- 4) 응답 head 수신과 파싱 ---- */
    char rbuf[PROXY_MAX_RESPONSE_HEAD];
    size_t rlen = 0, rhead = 0;
    status = read_backend_head(p, bfd, rbuf, sizeof rbuf, &rlen, &rhead);
    if (status != 0) {
        log_error("proxy: no valid response head from %s (%d)", be->host_header, status);
        goto send_error;
    }
    http_response_head_t rh;
    if (http_parse_response_head(rbuf, rhead, &rh) != 0 || rh.status < 200) {
        status = 502; /* 형식 오류 또는 1xx (Expect 를 보내지 않으므로 예상 밖) */
        log_error("proxy: malformed response head from %s", be->host_header);
        goto send_error;
    }

    /* 응답 본문의 끝을 어떻게 알 수 있는가 (framing) */
    body_mode_t mode;
    if (head_req || rh.status == 204 || rh.status == 304)
        mode = BODY_NONE;
    else if (rh.has_transfer_encoding)
        mode = BODY_UNTIL_EOF; /* chunked 바이트를 그대로 전달하고 EOF 까지 */
    else if (rh.has_content_length)
        mode = BODY_LENGTH;
    else
        mode = BODY_UNTIL_EOF; /* 길이 정보 없음: 백엔드가 닫는 것이 끝 */
    /* 끝을 EOF 로만 알 수 있는 응답은 클라이언트 연결도 닫아야 경계가 생긴다 */
    bool client_keep = keep_alive && mode != BODY_UNTIL_EOF;

    /* ---- 클라이언트에게 보낼 head 재작성 ---- */
    char out[PROXY_MAX_RESPONSE_HEAD + 256];
    strbuf_t ob = {.buf = out, .cap = sizeof out, .len = 0, .overflow = false};
    sb_append(&ob, "HTTP/1.1 %d %s\r\n", rh.status, rh.reason);
    const char *bconn = http_header_get(rh.headers, rh.header_count, "Connection");
    for (size_t i = 0; i < rh.header_count; i++) {
        const http_header_t *h = &rh.headers[i];
        if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            if (mode == BODY_UNTIL_EOF)
                sb_append(&ob, "%s: %s\r\n", h->name, h->value); /* 그대로 중계 */
            continue;
        }
        if (proxy_is_hop_by_hop(h->name, bconn))
            continue;
        if (strcasecmp(h->name, "Content-Length") == 0 && rh.has_transfer_encoding)
            continue; /* TE 가 있으면 CL 은 무시해야 한다 */
        sb_append(&ob, "%s: %s\r\n", h->name, h->value);
    }
    sb_append(&ob, "Connection: %s\r\n\r\n", client_keep ? "keep-alive" : "close");
    if (ob.overflow) {
        status = 502;
        goto send_error;
    }

    info.status = rh.status;
    info.keep_open = client_keep;
    if (send_all(cfd, out, ob.len, now_ms() + client_timeout_ms) != 0) {
        info.keep_open = false; /* 클라이언트가 떠남 */
        goto cleanup;
    }
    if (mode == BODY_NONE)
        goto cleanup;

    /* ---- 5) 본문 중계 ---- */
    unsigned long long remaining = (mode == BODY_LENGTH) ? rh.content_length : ULLONG_MAX;
    size_t pending = rlen - rhead; /* head 와 함께 이미 받은 본문 앞부분 */
    if (pending > remaining)
        pending = (size_t)remaining;
    if (pending > 0) {
        if (send_all(cfd, rbuf + rhead, pending, now_ms() + client_timeout_ms) != 0) {
            info.keep_open = false;
            goto cleanup;
        }
        info.body_bytes += pending;
        if (mode == BODY_LENGTH)
            remaining -= pending;
    }

    char chunk[64 * 1024];
    while (mode == BODY_UNTIL_EOF || remaining > 0) {
        size_t want = sizeof chunk;
        if (mode == BODY_LENGTH && remaining < want)
            want = (size_t)remaining;
        ssize_t n = recv_until_deadline(bfd, chunk, want, now_ms() + p->timeout_ms);
        if (n == 0 && mode == BODY_UNTIL_EOF)
            break; /* 정상 종료: 백엔드가 닫음 */
        if (n <= 0) {
            /* 본문 도중 백엔드 시간 초과/끊김: 이미 200 을 보냈으므로 연결을 닫아
             * 클라이언트가 "잘린 응답" 임을 알게 한다 */
            log_error("proxy: backend %s body %s", be->host_header,
                      n == IO_TIMEOUT ? "timed out" : "ended early");
            info.keep_open = false;
            break;
        }
        if (send_all(cfd, chunk, (size_t)n, now_ms() + client_timeout_ms) != 0) {
            info.keep_open = false;
            break;
        }
        info.body_bytes += (unsigned long long)n;
        if (mode == BODY_LENGTH)
            remaining -= (unsigned long long)n;
    }
    goto cleanup;

send_error:
    info.status = status;
    if (http_send_error(cfd, status, keep_alive, head_req, NULL,
                        now_ms() + client_timeout_ms) != 0)
        info.keep_open = false;

cleanup:
    if (bfd >= 0)
        close(bfd); /* 백엔드 fd 회수: 모든 경로가 여기를 지난다 */
    return info;
}
