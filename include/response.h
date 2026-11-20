#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>

const char *status_text(int status);

/* 상태 줄 + 공통 헤더(Date, Server, Content-Type, Content-Length, Connection) + extra를 보낸다.
   ctype이 NULL이면 Content-Type 생략, length < 0이면 Content-Length 생략.
   extra는 "Name: value\r\n" 형태의 추가 헤더 줄들 (없으면 NULL). 0 = 성공, -1 = 실패 */
int send_head(int fd, int status, const char *ctype, long long length, int keep_alive,
              const char *extra);

/* 헤더 + 메모리에 있는 본문. head_only면 본문은 빼고 보낸다 (HEAD 요청) */
int send_body(int fd, int status, const char *ctype, const void *body, size_t len,
              int keep_alive, const char *extra, int head_only);

/* "<h1>404 Not Found</h1>" 같은 작은 HTML 오류 응답. HEAD 요청이면 head_only=1 */
int send_error(int fd, int status, int keep_alive, const char *extra, int head_only);

#endif
