/*
 * http_conn.h — 클라이언트 연결 1개에서 "요청 1개" 를 완전히 읽어 오는 모듈.
 *
 * 연결마다 고정 크기 버퍼(buf)를 하나 둔다. recv() 로 받은 바이트를 계속
 * 뒤에 붙이다가 "\r\n\r\n" 이 보이면 head 가 완성된 것이다.
 * head 뒤에 이미 따라온 바이트는 본문의 앞부분(또는 다음 요청)이다.
 *
 *   buf: [ head ......\r\n\r\n | body 앞부분 | (다음 요청 바이트) | 빈 공간 ]
 *          <---- head_len ----->
 *          <---------------- len (유효 바이트) ----------------->
 */
#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <netinet/in.h>
#include <stddef.h>

#include "http.h"

typedef struct {
    int fd;                          /* accept() 가 돌려준 "연결 소켓" */
    char client_ip[INET6_ADDRSTRLEN];
    char buf[HTTP_MAX_HEAD_BYTES];   /* 아직 처리하지 않은 수신 바이트 */
    size_t len;                      /* buf 안의 유효 바이트 수 */
    size_t consumed;                 /* 현재 요청이 buf 에서 차지한 바이트 수 */
} http_conn_t;

typedef struct {
    int read_timeout_ms;             /* 요청 1개를 다 받기까지의 마감 */
    unsigned long long max_body_bytes;
} http_limits_t;

typedef enum {
    READ_OK,      /* 요청(head+body)을 완전히 받음 */
    READ_CLOSED,  /* 새 요청의 첫 바이트도 오기 전에 상대가 닫음/조용히 시간 초과 */
    READ_ABORTED, /* 요청 도중 연결 끊김/오류 → 응답 없이 닫는다 */
    READ_BAD      /* 프로토콜 오류 → *status 로 오류 응답 후 닫는다 */
} read_result_t;

void http_conn_init(http_conn_t *c, int fd);

/* 요청 1개를 읽는다. READ_BAD 이면 *status 에 400/408/413/414/431/500/501/505.
 * 어떤 결과든 끝나면 http_request_free() 를 호출해야 한다. */
read_result_t http_read_request(http_conn_t *c, http_request_t *req,
                                const http_limits_t *lim, int *status);

/* req->body 해제 (여러 번 호출해도 안전) */
void http_request_free(http_request_t *req);

/* 처리한 요청의 바이트를 buf 에서 지우고 뒤의 바이트(다음 요청)를 앞으로 당긴다 */
void http_conn_consume(http_conn_t *c);

#endif /* HTTP_CONN_H */
