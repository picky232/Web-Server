/*
 * http.h — HTTP/1.x 메시지 "머리(head)" 파싱. 소켓을 전혀 모르는 순수 함수들.
 *
 * head = 요청/상태 라인 + 헤더 줄들 + 빈 줄("\r\n\r\n" 로 끝남).
 * 파서는 버퍼를 제자리(in place)에서 고친다: 각 줄 끝의 "\r\n" 과 ':' 자리에
 * '\0' 을 넣고, http_request_t 의 문자열 포인터들은 그 버퍼 안을 가리킨다.
 * 따라서 버퍼가 살아 있는 동안만 포인터가 유효하다.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_HEAD_BYTES 8192   /* 요청 라인+헤더 전체 상한 → 넘으면 431/414 */
#define HTTP_MAX_TARGET_LEN 2048   /* 요청 URI 길이 상한 → 넘으면 414 */
#define HTTP_MAX_HEADERS 64        /* 헤더 줄 개수 상한 → 넘으면 431 */
#define HTTP_MAX_METHOD_LEN 16

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_OTHER /* PUT/DELETE 등 표준 메서드지만 이 서버가 처리하지 않음 → 405 */
} http_method_t;

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

typedef struct {
    /* ---- http_parse_request_head 가 채우는 부분 (버퍼 안을 가리킴) ---- */
    const char *method_str;
    http_method_t method;
    const char *target;        /* 예: "/index.html?x=1" (origin-form 만 허용) */
    size_t target_len;
    int version_minor;         /* 0 → HTTP/1.0, 1 → HTTP/1.1 */
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    bool has_content_length;
    unsigned long long content_length;
    bool keep_alive;           /* 클라이언트가 연결 유지를 원하는가 */
    bool expect_continue;      /* "Expect: 100-continue" */
    size_t head_len;           /* head 바이트 수 ("\r\n\r\n" 포함) */

    /* ---- http_read_request 가 채우는 부분 ---- */
    char *body;                /* malloc 된 본문 (없으면 NULL) */
    size_t body_len;
} http_request_t;

/* 백엔드 응답의 상태 라인 + 헤더 (proxy 가 사용) */
typedef struct {
    int status;
    const char *reason;
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t header_count;
    bool has_content_length;
    unsigned long long content_length;
    bool has_transfer_encoding;
} http_response_head_t;

/* buf[0..len) 에서 "\r\n\r\n" 을 찾아 head 의 길이(빈 줄 포함)를 돌려준다.
 * 아직 head 가 다 오지 않았으면 0. */
size_t http_find_head_end(const char *buf, size_t len);

/* 요청 head 를 파싱한다. 반환: 0 성공, 또는 보내야 할 HTTP 오류 상태 코드
 * (400, 414, 431, 501, 505). */
int http_parse_request_head(char *buf, size_t head_len, http_request_t *req);

/* 응답 head 를 파싱한다. 반환: 0 성공, -1 형식 오류 */
int http_parse_response_head(char *buf, size_t head_len, http_response_head_t *res);

/* "Name: value\r\n" 줄들을 파싱한다 (요청/응답 공용). 반환: 0, 400, 431 */
int http_parse_header_lines(char *p, char *end, http_header_t *out, size_t max,
                            size_t *count);

/* 대소문자 무시로 헤더를 찾는다 (첫 번째 값). 없으면 NULL. */
const char *http_header_get(const http_header_t *headers, size_t count,
                            const char *name);

/* "close, Upgrade" 같은 쉼표 목록에 token 이 있는지 (대소문자 무시) */
bool http_header_has_token(const char *value, const char *token);

/* Content-Length 값 검증: 숫자만, 오버플로 없음. 반환 0 성공, -1 실패 */
int http_parse_content_length(const char *value, unsigned long long *out);

/* RFC 9110 token 문자인가 (메서드, 헤더 이름에 쓰임) */
bool http_is_tchar(char c);

#endif /* HTTP_H */
