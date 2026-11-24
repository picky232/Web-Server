#ifndef EXCHANGE_H
#define EXCHANGE_H

#include "body.h"
#include "cgi.h"
#include "http.h"
#include "reply.h"
#include "spool.h"
#include "upstream.h"
#include "uri.h"

struct cached;
struct cache_st {
    struct cached *o;
    size_t off;
};

struct static_st {
    int fd;
    long long off, left;
};

struct conn;
struct route;

/* 응답을 만들어 내는 쪽의 공통 모양. 정적 파일·백엔드 중계·CGI·상태 페이지가 이 함수들만 채우면 된다.
   새 기능(캐시, 포워드 프록시)도 여기에 하나 더 끼우는 것으로 끝난다 */
struct handler {
    const char *name;
    void (*start)(struct conn *c);    /* 본문까지 다 받은 뒤 한 번 */
    bool (*pump)(struct conn *c);     /* 이어서 만들기. 조금이라도 나아갔으면 true */
    void (*interest)(struct conn *c); /* 상대 소켓(백엔드, CGI)을 어느 방향으로 지켜볼지 다시 계산 */
    void (*timeout)(struct conn *c);  /* ex->deadline이 지남 */
    void (*stop)(struct conn *c);     /* 끝났거나 클라이언트가 떠남: 자원 회수 */
};

/* 요청 하나와 그 응답. 요청이 시작될 때 할당하고 응답을 다 보내면 해제한다 */
struct exchange {
    char *head; /* 요청 머리 복사본. req의 조각들이 이 안을 가리킨다 */
    struct http_req req;
    struct target target;
    char host[256];
    int port;
    char *path; /* 정리된 경로. 라우팅과 전달에 이것만 쓴다 */
    struct body_dec dec;
    struct spool body;
    bool sent_100;
    const struct route *route;
    const struct handler *h;
    bool started; /* h->start를 불렀다 → 끝날 때 h->stop도 불러야 한다 */
    struct reply reply;
    long long start_ms;
    char served_by[96]; /* 로그용: 누가 처리했나 */
    int tries;
    long long deadline; /* 핸들러가 정한 제한 시각. 0 = 없음 */
    union {
        struct static_st file;
        struct proxy_st proxy;
        struct cgi_st cgi;
        struct cache_st cache;
    } u; /* 핸들러별 상태 */
};

bool exchange_head(struct conn *c);    /* 요청 머리를 읽고 어떻게 처리할지 결정 */
bool exchange_body(struct conn *c);    /* 요청 본문 모으기 */
void exchange_error(struct conn *c, int status, const char *extra);
void exchange_reject(struct conn *c, int status); /* 요청을 해석하기 전에 거절 */
void exchange_finish(struct conn *c);  /* 핸들러가 응답을 다 만들었다 */
void exchange_abort(struct conn *c);   /* 응답 도중 실패: 연결을 끊어서 알린다 */
void exchange_free(struct conn *c);

#endif
