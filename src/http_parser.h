#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/* 증분 HTTP/1.x 요청 파서 (RFC 7230 최소 구현).
 *
 * - 호출자는 요청 시작부터 "누적된" 버퍼를 매번 넘긴다. 파서는 이전에 본 위치부터 이어서 스캔한다.
 * - 파서는 버퍼를 소유하지 않는다. 결과는 버퍼 내 오프셋으로만 기록한다
 *   (버퍼가 realloc 되어도 오프셋은 유효).
 * - 한 요청이 끝나면 total_len 바이트를 버퍼 앞에서 제거하고 hp_init 으로 재사용한다
 *   (keep-alive / pipelining).
 */

#define HP_MAX_HEADERS 64

typedef enum { HP_GET, HP_HEAD, HP_POST } hp_method;
typedef enum { HP_NEED_MORE, HP_COMPLETE, HP_ERROR } hp_status;
typedef enum { HP_S_REQLINE, HP_S_HEADERS, HP_S_BODY, HP_S_DONE, HP_S_ERROR } hp_state;

typedef struct {
    size_t name_off, name_len;
    size_t value_off, value_len;
} hp_header;

typedef struct {
    /* 한도 (hp_init 에서 설정) */
    size_t max_head;   /* 요청 라인 + 헤더 전체 최대 바이트 -> 431 */
    size_t max_uri;    /* URI 최대 길이 -> 414 */
    size_t max_body;   /* 본문 최대 길이 -> 413 */

    hp_state state;
    size_t   pos;      /* 현재 줄의 시작 */
    size_t   scan;     /* 개행 탐색 재개 위치 */

    /* 요청 라인 */
    hp_method method;
    size_t    uri_off, uri_len;
    int       version_minor;  /* HTTP/1.x 의 x */

    /* 헤더 */
    hp_header headers[HP_MAX_HEADERS];
    int       nheaders;
    int       has_host;
    int       has_content_length;
    size_t    content_length;
    int       keep_alive;

    /* 위치 */
    size_t header_end;  /* 본문 시작 오프셋 */
    size_t total_len;   /* 이 요청이 차지하는 전체 바이트 (헤더+본문) */

    int err_status;     /* HP_ERROR 일 때 응답할 상태 코드 */
} hp_parser;

void      hp_init(hp_parser *p, size_t max_head, size_t max_uri, size_t max_body);
hp_status hp_feed(hp_parser *p, const char *buf, size_t len);

/* 헤더 값 조회 (이름은 대소문자 무시). 없으면 NULL. */
const char *hp_header_get(const hp_parser *p, const char *buf, const char *name, size_t *vlen);

#endif
