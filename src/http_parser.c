/*
 * http_parser.c — 요청/응답 head 파서 (소켓 I/O 없음 → 단위 테스트가 쉽다).
 *
 * 지원 범위(의도적으로 작게):
 *   - 요청 라인: METHOD SP origin-form-target SP HTTP/1.0|HTTP/1.1
 *   - 줄 끝은 반드시 CRLF. obs-fold(줄 시작 공백 이어쓰기)는 400.
 *   - Transfer-Encoding 이 붙은 요청은 501 (chunked 요청 본문 미지원).
 *     Transfer-Encoding 과 Content-Length 가 함께 오면 요청 밀반입(smuggling)
 *     위험이 있으므로 400.
 */
#include "http.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

size_t http_find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

bool http_is_tchar(char c)
{
    if (isalnum((unsigned char)c))
        return true;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* [p, end) 범위에서 첫 "\r\n" 위치 */
static char *find_crlf(char *p, char *end)
{
    for (; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    return NULL;
}

int http_parse_header_lines(char *p, char *end, http_header_t *out, size_t max,
                            size_t *count)
{
    *count = 0;
    while (p < end) {
        char *crlf = find_crlf(p, end);
        if (crlf == NULL)
            return 400;
        *crlf = '\0';

        if (*p == ' ' || *p == '\t')
            return 400; /* obs-fold: 폐기된 문법, 거부 */

        char *colon = strchr(p, ':');
        if (colon == NULL || colon == p)
            return 400;
        for (char *c = p; c < colon; c++)
            if (!http_is_tchar(*c))
                return 400; /* "Host : x" 처럼 이름 뒤 공백도 여기서 거부 */
        *colon = '\0';

        /* 값 앞뒤의 공백/탭(OWS) 제거 */
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        char *ve = crlf;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
        *ve = '\0';
        for (char *c = v; *c; c++) {
            unsigned char uc = (unsigned char)*c;
            if ((uc < 0x20 && uc != '\t') || uc == 0x7f)
                return 400; /* 값 안의 제어 문자 (CR/LF 단독 포함) */
        }

        if (*count >= max)
            return 431;
        out[*count].name = p;
        out[*count].value = v;
        (*count)++;
        p = crlf + 2;
    }
    return 0;
}

const char *http_header_get(const http_header_t *headers, size_t count,
                            const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcasecmp(headers[i].name, name) == 0)
            return headers[i].value;
    return NULL;
}

bool http_header_has_token(const char *value, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *stop = p;
        while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'))
            stop--;
        if ((size_t)(stop - start) == tlen && strncasecmp(start, token, tlen) == 0)
            return true;
    }
    return false;
}

int http_parse_content_length(const char *value, unsigned long long *out)
{
    if (value == NULL || *value == '\0')
        return -1;
    unsigned long long v = 0;
    for (const char *s = value; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1; /* "-1", "1 2", "0x10", "1,1" 모두 거부 */
        unsigned d = (unsigned)(*s - '0');
        if (v > (ULLONG_MAX - d) / 10)
            return -1; /* 오버플로 */
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

static int classify_method(const char *m, http_method_t *out)
{
    if (strcmp(m, "GET") == 0) {
        *out = HTTP_METHOD_GET;
        return 0;
    }
    if (strcmp(m, "HEAD") == 0) {
        *out = HTTP_METHOD_HEAD;
        return 0;
    }
    if (strcmp(m, "POST") == 0) {
        *out = HTTP_METHOD_POST;
        return 0;
    }
    static const char *const others[] = {"PUT", "DELETE", "OPTIONS", "PATCH",
                                         "CONNECT", "TRACE", NULL};
    for (int i = 0; others[i]; i++) {
        if (strcmp(m, others[i]) == 0) {
            *out = HTTP_METHOD_OTHER;
            return 0;
        }
    }
    return 501; /* 문법상 올바른 token 이지만 모르는 메서드 (예: "get", "BREW") */
}

/* "HTTP/1.1" → minor=1. 형식이 틀리면 400, 다른 버전이면 505. */
static int parse_version(const char *v, int *minor)
{
    if (strncmp(v, "HTTP/", 5) != 0 || strlen(v) != 8 ||
        !isdigit((unsigned char)v[5]) || v[6] != '.' || !isdigit((unsigned char)v[7]))
        return 400;
    if (v[5] != '1' || (v[7] != '0' && v[7] != '1'))
        return 505;
    *minor = v[7] - '0';
    return 0;
}

int http_parse_request_head(char *buf, size_t head_len, http_request_t *req)
{
    memset(req, 0, sizeof *req);
    req->head_len = head_len;
    if (head_len < 4 || memchr(buf, '\0', head_len) != NULL)
        return 400; /* NUL 바이트가 섞이면 문자열 함수가 잘못 동작하므로 거부 */

    char *end = buf + head_len; /* head 는 "...\r\n\r\n" 으로 끝난다 */

    /* ---- 1) 요청 라인 ---- */
    char *line_end = find_crlf(buf, end);
    *line_end = '\0'; /* head_len>=4 이고 끝이 CRLFCRLF 이므로 반드시 찾음 */

    char *sp1 = strchr(buf, ' ');
    if (sp1 == NULL)
        return 400;
    *sp1 = '\0';
    char *method = buf;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen == 0 || mlen >= HTTP_MAX_METHOD_LEN)
        return mlen == 0 ? 400 : 501;
    for (char *c = method; *c; c++)
        if (!http_is_tchar(*c))
            return 400;

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (sp2 == NULL)
        return 400;
    *sp2 = '\0';
    size_t tlen = (size_t)(sp2 - target);
    if (tlen == 0)
        return 400;
    if (tlen > HTTP_MAX_TARGET_LEN)
        return 414;
    for (char *c = target; *c; c++) {
        unsigned char uc = (unsigned char)*c;
        if (uc <= 0x20 || uc >= 0x7f)
            return 400; /* 공백/제어 문자/8bit 문자는 인코딩되어 와야 한다 */
    }
    if (target[0] != '/')
        return 400; /* absolute-form("http://...")은 거부: 개방형 프록시 방지 */

    char *version = sp2 + 1;
    if (strchr(version, ' ') != NULL)
        return 400;
    int st = parse_version(version, &req->version_minor);
    if (st != 0)
        return st;
    st = classify_method(method, &req->method);
    if (st != 0)
        return st;

    req->method_str = method;
    req->target = target;
    req->target_len = tlen;

    /* ---- 2) 헤더 줄들: [요청 라인 다음, 마지막 빈 줄 앞) ---- */
    st = http_parse_header_lines(line_end + 2, end - 2, req->headers,
                                 HTTP_MAX_HEADERS, &req->header_count);
    if (st != 0)
        return st;

    /* ---- 3) 의미 검사 ---- */
    int host_count = 0;
    bool has_te = false, conn_close = false, conn_keep = false;
    for (size_t i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        const char *value = req->headers[i].value;
        if (strcasecmp(name, "Host") == 0) {
            host_count++;
        } else if (strcasecmp(name, "Content-Length") == 0) {
            unsigned long long cl;
            if (http_parse_content_length(value, &cl) != 0)
                return 400;
            if (req->has_content_length && cl != req->content_length)
                return 400; /* 서로 다른 Content-Length 중복 → 모호함 */
            req->has_content_length = true;
            req->content_length = cl;
        } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
            has_te = true;
        } else if (strcasecmp(name, "Connection") == 0) {
            if (http_header_has_token(value, "close"))
                conn_close = true;
            if (http_header_has_token(value, "keep-alive"))
                conn_keep = true;
        } else if (strcasecmp(name, "Expect") == 0) {
            if (strcasecmp(value, "100-continue") == 0)
                req->expect_continue = true;
        }
    }
    if (host_count > 1 || (req->version_minor == 1 && host_count == 0))
        return 400; /* HTTP/1.1 은 Host 헤더가 정확히 1개 필요 */
    if (has_te && req->has_content_length)
        return 400;
    if (has_te)
        return 501; /* chunked 등 요청 전송 코딩은 구현하지 않음 */

    /* HTTP/1.1 은 기본이 keep-alive, HTTP/1.0 은 기본이 close */
    req->keep_alive = (req->version_minor == 1) ? !conn_close : (conn_keep && !conn_close);
    return 0;
}

int http_parse_response_head(char *buf, size_t head_len, http_response_head_t *res)
{
    memset(res, 0, sizeof *res);
    if (head_len < 4 || memchr(buf, '\0', head_len) != NULL)
        return -1;
    char *end = buf + head_len;
    char *line_end = find_crlf(buf, end);
    *line_end = '\0';

    /* "HTTP/1.1 200 OK" — reason phrase 는 비어 있을 수 있다 */
    if (strncmp(buf, "HTTP/1.", 7) != 0 || (buf[7] != '0' && buf[7] != '1') ||
        buf[8] != ' ')
        return -1;
    char *code = buf + 9;
    if (!isdigit((unsigned char)code[0]) || !isdigit((unsigned char)code[1]) ||
        !isdigit((unsigned char)code[2]) || (code[3] != ' ' && code[3] != '\0'))
        return -1;
    res->status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    if (res->status < 100 || res->status > 599)
        return -1;
    res->reason = code[3] == ' ' ? code + 4 : "";
    for (const char *c = res->reason; *c; c++) {
        unsigned char uc = (unsigned char)*c;
        if ((uc < 0x20 && uc != '\t') || uc == 0x7f)
            return -1;
    }

    if (http_parse_header_lines(line_end + 2, end - 2, res->headers,
                                HTTP_MAX_HEADERS, &res->header_count) != 0)
        return -1;

    for (size_t i = 0; i < res->header_count; i++) {
        const char *name = res->headers[i].name;
        if (strcasecmp(name, "Content-Length") == 0) {
            unsigned long long cl;
            if (http_parse_content_length(res->headers[i].value, &cl) != 0)
                return -1;
            if (res->has_content_length && cl != res->content_length)
                return -1;
            res->has_content_length = true;
            res->content_length = cl;
        } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
            res->has_transfer_encoding = true;
        }
    }
    return 0;
}
