/*
 * http_response.h — 응답 상태 라인/헤더 작성과 오류 응답.
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERVER_NAME "jungle-webserver/1.0"

/* 요청 1개를 처리한 결과 (로그와 keep-alive 판단에 사용) */
typedef struct {
    int status;                    /* 클라이언트에게 보낸 상태 코드 */
    unsigned long long body_bytes; /* 실제로 보낸 본문 바이트 */
    bool keep_open;                /* 이 연결로 다음 요청을 받아도 되는가 */
} response_info_t;

const char *http_reason_phrase(int status);

/* 상태 라인 + 공통 헤더 + 빈 줄을 보낸다. extra_headers 는 "Name: v\r\n" 형식 또는 NULL.
 * 반환: 0 성공, -1/IO_TIMEOUT 실패 */
int http_send_head(int fd, int status, const char *content_type,
                   unsigned long long content_length, bool keep_alive,
                   const char *extra_headers, int64_t deadline_ms);

/* 작은 메모리 본문을 가진 응답 전체를 보낸다. head_only 면 본문 생략(HEAD). */
int http_send_simple(int fd, int status, const char *content_type,
                     const char *body, size_t body_len, bool keep_alive,
                     bool head_only, const char *extra_headers, int64_t deadline_ms);

/* "<h1>404 Not Found</h1>" 같은 짧은 HTML 오류 응답.
 * HEAD 요청에 대한 오류라면 head_only=true 로 본문을 생략한다. */
int http_send_error(int fd, int status, bool keep_alive, bool head_only,
                    const char *extra_headers, int64_t deadline_ms);

#endif /* HTTP_RESPONSE_H */
