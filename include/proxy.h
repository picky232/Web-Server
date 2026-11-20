/*
 * proxy.h — 고정된 백엔드로만 요청을 중계하는 reverse proxy.
 *
 * 백엔드 주소는 서버 시작 시 CLI(--backend)로 정해지고 한 번만 DNS 해석된다.
 * 클라이언트가 보낸 URL/Host 로 접속 대상을 바꿀 수 없으므로 "개방형 프록시"가 아니다.
 */
#ifndef PROXY_H
#define PROXY_H

#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>

#include "config.h"
#include "http.h"
#include "http_conn.h"
#include "http_response.h"

#define PROXY_MAX_RESPONSE_HEAD 16384 /* 백엔드 응답 head 상한 → 넘으면 502 */

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char host_header[CONFIG_HOST_MAX + 8]; /* 백엔드에 보낼 Host 값 "host:port" */
} proxy_backend_t;

typedef struct {
    proxy_backend_t backends[CONFIG_MAX_BACKENDS];
    int count;
    atomic_uint next;          /* round-robin 커서: 여러 worker 가 동시에 증가시킴 */
    int timeout_ms;            /* connect / 응답 대기 / 본문 수신 간격 */
    const char *prefix;        /* 예: "/api" */
} proxy_t;

/* 설정의 백엔드 주소들을 getaddrinfo 로 해석해 둔다. 실패 시 -1 + err */
int proxy_init(proxy_t *p, const server_config_t *cfg, char *err, size_t errlen);

/* 요청 1개를 백엔드로 전달하고 응답을 클라이언트에게 중계한다.
 * 백엔드 문제는 502(연결 실패/잘못된 응답) 또는 504(시간 초과)로 응답한다. */
response_info_t proxy_forward(proxy_t *p, http_conn_t *client, const http_request_t *req,
                              bool keep_alive, int client_timeout_ms);

/* hop-by-hop 헤더인가: 한 구간(클라이언트↔프록시, 프록시↔백엔드)에만 의미가 있어
 * 그대로 전달하면 안 되는 헤더. connection_value 에 나열된 이름도 포함. */
bool proxy_is_hop_by_hop(const char *name, const char *connection_value);

#endif /* PROXY_H */
