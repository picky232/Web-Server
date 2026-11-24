#ifndef HTTP_H
#define HTTP_H

#include "str.h"

/* 소켓을 모르는 순수 함수만 모은 곳. 바이트 묶음을 받아 해석만 한다.
   근거: RFC 9110, 9112 (과제의 RFC 7230~7235를 대체한 문서) */

#define HTTP_HEAD_MAX 16384 /* 시작 줄 + 헤더 전체 상한 */
#define HTTP_URI_MAX 8000
#define HTTP_MAX_FIELDS 100

enum http_method {
    M_GET, M_HEAD, M_POST, M_PUT, M_DELETE, M_PATCH, M_OPTIONS, M_TRACE, M_CONNECT, M_OTHER
};

struct field {
    struct sl name, value;
};
struct fields {
    struct field f[HTTP_MAX_FIELDS];
    int n;
};

struct http_req {
    struct sl method_text, target;
    enum http_method method;
    int minor; /* HTTP/1.0 → 0, HTTP/1.1 → 1 */
    struct fields h;
    struct sl host;   /* Host 헤더. 없으면 p == NULL */
    long long length; /* Content-Length. 없으면 -1 */
    bool chunked;
    bool keep; /* 클라이언트가 응답 뒤에도 연결을 유지하고 싶어 하는가 */
    bool expect_100;
    bool expect_other;
};

struct http_res {
    int status, minor;
    struct sl reason;
    struct fields h;
    long long length;
    bool chunked;
    bool keep;
};

enum scan { SCAN_MORE, SCAN_END, SCAN_BAD };

/* 빈 줄(\r\n\r\n)을 찾는다. 이어서 부를 수 있게 *pos에 어디까지 봤는지 적어 둔다.
   SCAN_END면 *pos가 머리의 전체 길이. 홑 LF·홑 CR·제어 문자는 바로 SCAN_BAD */
enum scan http_scan_head(const char *p, size_t n, size_t *pos);

/* 0 = 성공, 그 외 = 보내야 할 상태 코드 */
int http_parse_request(const char *p, size_t n, struct http_req *r);
bool http_parse_response(const char *p, size_t n, struct http_res *r);

const struct field *fields_find(const struct fields *h, const char *name);
bool fields_token(const struct fields *h, const char *name, const char *token); /* 같은 이름이 여러 줄이어도 */
/* 이 구간(클라이언트↔우리, 우리↔백엔드)에서만 뜻이 있어 다음으로 넘기면 안 되는 헤더인가 */
bool fields_hop_by_hop(const struct fields *h, struct sl name);
bool http_is_token(struct sl s);

#endif
