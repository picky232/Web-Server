#include "http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

size_t http_find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return 0;
}

/* 메서드·헤더 이름에 쓸 수 있는 글자 (RFC 9110 token) */
static int is_tchar(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

static int is_token(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!is_tchar((unsigned char)*s))
            return 0;
    return 1;
}

static int known_method(const char *m)
{
    static const char *const known[] = {"GET", "HEAD", "POST", "PUT", "DELETE",
                                        "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(m, known[i]))
            return 1;
    return 0;
}

/* "METHOD SP target SP HTTP/x.y" 한 줄 해석 */
static int parse_request_line(char *line, struct request *req)
{
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || strchr(sp2 + 1, ' ') || sp1 == line || sp2 == sp1 + 1)
        return 400;
    *sp1 = *sp2 = '\0';
    const char *method = line, *target = sp1 + 1, *version = sp2 + 1;

    if (!is_token(method))
        return 400;
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) || version[6] != '.' ||
        version[5] < '0' || version[5] > '9' || version[7] < '0' || version[7] > '9')
        return 400;
    if (version[5] != '1' || version[7] > '1')
        return 505; /* 형식은 맞지만 우리가 모르는 버전 (예: HTTP/2.0) */
    if (strlen(target) > URI_MAX)
        return 414;
    /* origin-form("/경로")만 받는다. "http://host/..." 나 "host:443"은 프록시 요청이라 거절 */
    if (target[0] != '/')
        return 400;
    for (const unsigned char *p = (const unsigned char *)target; *p; p++)
        if (*p <= 0x20 || *p == 0x7f)
            return 400;
    if (!known_method(method))
        return 501;

    req->method = method;
    req->target = target;
    req->minor = version[7] - '0';
    return 0;
}

static int parse_header_line(char *line, struct request *req)
{
    if (line[0] == ' ' || line[0] == '\t')
        return 400; /* 앞이 공백인 줄 = 옛날 줄 이어 쓰기(obs-fold). RFC 9112에서 거절 */
    char *colon = strchr(line, ':');
    if (!colon)
        return 400;
    *colon = '\0';
    if (!is_token(line))
        return 400; /* "Bad Header: x"처럼 이름에 공백이 있으면 여기서 걸린다 */
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    char *end = v + strlen(v);
    while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f)
            return 400;
    if (req->nheaders == HEADERS_MAX)
        return 431;
    req->headers[req->nheaders].name = line;
    req->headers[req->nheaders].value = v;
    req->nheaders++;
    return 0;
}

static int count_header(const struct request *req, const char *name)
{
    int n = 0;
    for (int i = 0; i < req->nheaders; i++)
        n += !strcasecmp(req->headers[i].name, name);
    return n;
}

/* 본문 길이와 연결 유지 여부처럼 헤더끼리 맞춰 봐야 하는 규칙 */
static int check_semantics(struct request *req)
{
    int hosts = count_header(req, "Host");
    if (hosts > 1 || (req->minor == 1 && hosts == 0))
        return 400; /* HTTP/1.1은 Host가 정확히 하나여야 한다 */

    const char *cl = http_header(req, "Content-Length");
    if (count_header(req, "Content-Length") > 1)
        return 400;
    if (http_header(req, "Transfer-Encoding"))
        return cl ? 400 : 501; /* 둘 다 있으면 본문 경계가 두 개라 위험(request smuggling) */
    req->content_length = 0;
    if (cl) {
        size_t n = strlen(cl);
        if (n == 0 || n > 15 || strspn(cl, "0123456789") != n)
            return 400;
        req->content_length = strtoll(cl, NULL, 10);
    }

    const char *conn = http_header(req, "Connection");
    if (req->minor == 1)
        req->keep_alive = !(conn && http_list_has(conn, "close"));
    else
        req->keep_alive = conn && http_list_has(conn, "keep-alive") && !http_list_has(conn, "close");

    const char *expect = http_header(req, "Expect");
    req->expect_continue = expect && !strcasecmp(expect, "100-continue");
    return 0;
}

int http_parse_request(const char *buf, size_t len, struct request *req)
{
    req->method = req->target = NULL;
    req->nheaders = 0;
    req->content_length = 0;
    req->keep_alive = 0;
    req->expect_continue = 0;
    if (len > HEAD_MAX)
        return 431;
    if (memchr(buf, '\0', len))
        return 400;
    memcpy(req->raw, buf, len);
    req->raw[len] = '\0';
    req->head_len = len;

    /* 줄 단위로 자르면서 해석. 마지막 빈 줄에서 끝난다 */
    char *line = req->raw;
    char *eol = strstr(line, "\r\n");
    *eol = '\0';
    int st = parse_request_line(line, req);
    if (st)
        return st;
    for (line = eol + 2; (eol = strstr(line, "\r\n")) && eol != line; line = eol + 2) {
        *eol = '\0';
        if ((st = parse_header_line(line, req)))
            return st;
    }
    return check_semantics(req);
}

const char *http_header(const struct request *req, const char *name)
{
    for (int i = 0; i < req->nheaders; i++)
        if (!strcasecmp(req->headers[i].name, name))
            return req->headers[i].value;
    return NULL;
}

int http_list_has(const char *list, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        size_t n = strcspn(p, ",");
        size_t m = n;
        while (m > 0 && (p[m - 1] == ' ' || p[m - 1] == '\t'))
            m--;
        if (m == tlen && !strncasecmp(p, token, tlen))
            return 1;
        p += n;
    }
    return 0;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

int http_decode_path(const char *target, char *out, size_t outsz)
{
    size_t end = strcspn(target, "?#"); /* 쿼리·조각은 파일 경로가 아니다 */
    size_t o = 0;
    for (size_t i = 0; i < end; i++) {
        int ch = (unsigned char)target[i];
        if (ch == '%') {
            int hi = i + 1 < end ? hexval(target[i + 1]) : -1;
            int lo = i + 2 < end ? hexval(target[i + 2]) : -1;
            if (hi < 0 || lo < 0)
                return 400;
            ch = hi * 16 + lo;
            i += 2;
            if (ch < 0x20 || ch == 0x7f)
                return 400; /* %00 같은 제어 문자 */
        }
        if (o + 1 >= outsz)
            return 414;
        out[o++] = (char)ch;
    }
    out[o] = '\0';

    /* 디코딩한 뒤에 조각을 검사해야 %2e%2e 같은 우회도 같은 규칙에 걸린다.
       '.'으로 시작하는 조각(., .., .git, .env)은 전부 막는다 */
    for (const char *p = out; (p = strchr(p, '/')); p++)
        if (p[1] == '.')
            return 403;
    return 0;
}
