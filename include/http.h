#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* 소켓을 모르는 순수 함수만 모은 곳. 버퍼만 받아서 해석한다 */

#define HEAD_MAX 8192   /* 요청 줄 + 헤더 전체 상한 → 넘으면 414/431 */
#define URI_MAX 2048    /* 요청 대상(경로) 상한 → 넘으면 414 */
#define HEADERS_MAX 64  /* 헤더 개수 상한 → 넘으면 431 */

struct header {
    const char *name;
    const char *value;
};

struct request {
    char raw[HEAD_MAX + 1]; /* 헤더 블록 복사본. 아래 포인터들은 전부 이 안을 가리킨다 */
    size_t head_len;        /* 빈 줄까지 포함한 길이 */
    const char *method;
    const char *target;     /* "/a/b?x=1" 원본 그대로 */
    int minor;              /* HTTP/1.0 → 0, HTTP/1.1 → 1 */
    struct header headers[HEADERS_MAX];
    int nheaders;
    long long content_length;
    int keep_alive;
    int expect_continue;
};

/* 버퍼에서 빈 줄(\r\n\r\n)을 찾아 헤더 블록 길이를 돌려준다. 아직 없으면 0 */
size_t http_find_head_end(const char *buf, size_t len);

/* 헤더 블록(buf[0..len))을 해석한다. 0 = 성공, 그 외 = 보내야 할 오류 상태 코드 */
int http_parse_request(const char *buf, size_t len, struct request *req);

/* 이름이 같은 헤더의 값 (대소문자 무시). 없으면 NULL */
const char *http_header(const struct request *req, const char *name);

/* "a, b ,c" 같은 쉼표 목록에 token이 있는지 (대소문자 무시) */
int http_list_has(const char *list, const char *token);

/* target에서 ?query를 떼고 %XX를 한 번 풀어 out에 경로를 쓴다.
   0 = 성공, 400 = 잘못된 %XX·제어 문자, 403 = '.'으로 시작하는 조각(.., 숨김 파일) */
int http_decode_path(const char *target, char *out, size_t outsz);

#endif
