#include "http_parser.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------ 문자 분류 ------------------------------ */

static int is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_ctl(unsigned char c) { return c < 0x20 || c == 0x7f; }

static unsigned char lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

/* buf[0..len) 와 소문자 리터럴 lit 의 대소문자 무시 비교 */
static int ci_eq(const char *buf, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)buf[i]) != (unsigned char)lit[i]) return 0;
    return 1;
}

/* ------------------------------- 초기화 -------------------------------- */

void hp_init(hp_parser *p, size_t max_head, size_t max_uri, size_t max_body) {
    memset(p, 0, sizeof *p);
    p->max_head = max_head;
    p->max_uri  = max_uri;
    p->max_body = max_body;
    p->state    = HP_S_REQLINE;
}

static hp_status fail(hp_parser *p, int status) {
    p->state = HP_S_ERROR;
    p->err_status = status;
    return HP_ERROR;
}

/* ----------------------------- 요청 라인 ------------------------------- */

static hp_status parse_request_line(hp_parser *p, const char *buf, size_t start, size_t end) {
    /* METHOD SP URI SP VERSION  (공백은 정확히 1개씩) */
    size_t sp1 = start;
    while (sp1 < end && buf[sp1] != ' ') sp1++;
    if (sp1 == end) return fail(p, 400);
    size_t sp2 = sp1 + 1;
    while (sp2 < end && buf[sp2] != ' ') sp2++;
    if (sp2 == end) return fail(p, 400);

    size_t mlen = sp1 - start;
    size_t uoff = sp1 + 1, ulen = sp2 - uoff;
    size_t voff = sp2 + 1, vlen = end - voff;

    if (mlen == 0) return fail(p, 400);
    for (size_t i = start; i < sp1; i++)
        if (!is_tchar((unsigned char)buf[i])) return fail(p, 400);

    if (ulen == 0) return fail(p, 400);
    for (size_t i = uoff; i < sp2; i++)
        if (is_ctl((unsigned char)buf[i])) return fail(p, 400);

    /* 버전: 정확히 "HTTP/d.d" (뒤에 공백이 더 있으면 400) */
    if (vlen != 8 || memcmp(buf + voff, "HTTP/", 5) != 0 || buf[voff + 6] != '.' ||
        buf[voff + 5] < '0' || buf[voff + 5] > '9' || buf[voff + 7] < '0' || buf[voff + 7] > '9')
        return fail(p, 400);

    /* URI 길이는 형식 검사 뒤에 판단 (414) */
    if (ulen > p->max_uri) return fail(p, 414);

    /* origin-form("/...") 또는 absolute-form("http://...") 만 허용 */
    if (buf[uoff] != '/') {
        int abs = 0;
        for (size_t i = uoff; i + 2 < sp2; i++)
            if (buf[i] == ':' && buf[i + 1] == '/' && buf[i + 2] == '/') { abs = 1; break; }
        if (!abs) return fail(p, 400);
    }

    int major = buf[voff + 5] - '0', minor = buf[voff + 7] - '0';
    if (major != 1 || minor > 1) return fail(p, 505);
    p->version_minor = minor;

    if (mlen == 3 && memcmp(buf + start, "GET", 3) == 0)       p->method = HP_GET;
    else if (mlen == 4 && memcmp(buf + start, "HEAD", 4) == 0) p->method = HP_HEAD;
    else if (mlen == 4 && memcmp(buf + start, "POST", 4) == 0) p->method = HP_POST;
    else return fail(p, 501);  /* 올바른 토큰이지만 미지원 */

    p->uri_off = uoff;
    p->uri_len = ulen;
    return HP_NEED_MORE;
}

/* ------------------------------- 헤더 ---------------------------------- */

/* Connection 헤더 값의 콤마 구분 토큰을 검사 */
static void scan_connection(const char *v, size_t len, int *close_flag, int *ka_flag) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < len && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (ci_eq(v + s, i - s, "close")) *close_flag = 1;
        else if (ci_eq(v + s, i - s, "keep-alive")) *ka_flag = 1;
    }
}

static hp_status parse_header_line(hp_parser *p, const char *buf, size_t start, size_t end) {
    if (buf[start] == ' ' || buf[start] == '\t') return fail(p, 400);  /* obs-fold 거부 */

    size_t colon = start;
    while (colon < end && buf[colon] != ':') colon++;
    if (colon == end || colon == start) return fail(p, 400);

    /* 이름은 토큰이어야 함 (콜론 앞 공백도 여기서 걸러짐: RFC 7230 3.2.4) */
    for (size_t i = start; i < colon; i++)
        if (!is_tchar((unsigned char)buf[i])) return fail(p, 400);

    size_t vs = colon + 1, ve = end;
    while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
    while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;
    for (size_t i = vs; i < ve; i++)
        if (is_ctl((unsigned char)buf[i]) && buf[i] != '\t') return fail(p, 400);

    if (p->nheaders >= HP_MAX_HEADERS) return fail(p, 431);
    hp_header *h = &p->headers[p->nheaders++];
    h->name_off = start;  h->name_len = colon - start;
    h->value_off = vs;    h->value_len = ve - vs;

    const char *name = buf + start; size_t nlen = colon - start;
    const char *val  = buf + vs;    size_t vlen = ve - vs;

    if (ci_eq(name, nlen, "host")) {
        if (p->has_host) return fail(p, 400);  /* 중복 Host */
        p->has_host = 1;
    } else if (ci_eq(name, nlen, "content-length")) {
        if (p->has_content_length) return fail(p, 400);  /* 중복은 smuggling 위험 -> 거부 */
        if (vlen == 0) return fail(p, 400);
        size_t v = 0;
        for (size_t i = 0; i < vlen; i++) {
            if (val[i] < '0' || val[i] > '9') return fail(p, 400);
            size_t d = (size_t)(val[i] - '0');
            if (v > (SIZE_MAX - 9) / 10) return fail(p, 413);  /* 오버플로 */
            v = v * 10 + d;
        }
        p->has_content_length = 1;
        p->content_length = v;
    }
    /* Connection / Transfer-Encoding 은 헤더 끝에서 목록을 훑어 한 번에 판단한다. */
    return HP_NEED_MORE;
}

/* 빈 줄을 만난 뒤: 헤더 간 의존 관계 검증 */
static hp_status finish_headers(hp_parser *p, size_t body_start, int conn_close, int conn_ka, int has_te) {
    if (p->version_minor == 1 && !p->has_host) return fail(p, 400);
    if (has_te) return fail(p, p->has_content_length ? 400 : 501); /* chunked 요청 본문 미지원 */
    if (p->method == HP_POST && !p->has_content_length) return fail(p, 411);
    if (p->has_content_length && p->content_length > p->max_body) return fail(p, 413);

    p->keep_alive = (p->version_minor >= 1) ? !conn_close : (conn_ka && !conn_close);
    p->header_end = body_start;
    p->total_len  = body_start + (p->has_content_length ? p->content_length : 0);
    return HP_NEED_MORE;
}

/* ------------------------------- 메인 ---------------------------------- */

hp_status hp_feed(hp_parser *p, const char *buf, size_t len) {
    while (p->state == HP_S_REQLINE || p->state == HP_S_HEADERS) {
        size_t i = p->scan;
        while (i < len && buf[i] != '\n') i++;

        if (i == len) {  /* 아직 줄이 끝나지 않음 */
            p->scan = len;
            size_t pending = len - p->pos;
            if (p->state == HP_S_REQLINE && pending > p->max_uri + 32) return fail(p, 414);
            if (len > p->max_head) return fail(p, 431);
            return HP_NEED_MORE;
        }
        p->scan = i + 1;
        if (p->scan > p->max_head) return fail(p, 431);

        /* 개행은 반드시 CRLF */
        if (i == p->pos || buf[i - 1] != '\r') return fail(p, 400);
        size_t line_start = p->pos, line_end = i - 1;  /* CR 제외 */
        p->pos = i + 1;

        if (p->state == HP_S_REQLINE) {
            if (line_end == line_start) continue;  /* 요청 라인 앞의 빈 줄은 무시 (RFC 7230 3.5) */
            if (line_end - line_start > p->max_uri + 32) return fail(p, 414);
            hp_status s = parse_request_line(p, buf, line_start, line_end);
            if (s == HP_ERROR) return s;
            p->state = HP_S_HEADERS;
            continue;
        }

        /* HP_S_HEADERS */
        if (line_end == line_start) {  /* 빈 줄: 헤더 끝 */
            int conn_close = 0, conn_ka = 0, has_te = 0;
            for (int k = 0; k < p->nheaders; k++) {
                const hp_header *h = &p->headers[k];
                const char *n = buf + h->name_off, *v = buf + h->value_off;
                if (ci_eq(n, h->name_len, "connection")) scan_connection(v, h->value_len, &conn_close, &conn_ka);
                else if (ci_eq(n, h->name_len, "transfer-encoding")) has_te = 1;
            }
            hp_status s = finish_headers(p, p->pos, conn_close, conn_ka, has_te);
            if (s == HP_ERROR) return s;
            p->state = HP_S_BODY;
            break;
        }

        if (parse_header_line(p, buf, line_start, line_end) == HP_ERROR) return HP_ERROR;
    }

    if (p->state == HP_S_ERROR) return HP_ERROR;
    if (p->state == HP_S_BODY) {
        if (len < p->total_len) return HP_NEED_MORE;
        p->state = HP_S_DONE;
    }
    return p->state == HP_S_DONE ? HP_COMPLETE : HP_NEED_MORE;
}

const char *hp_header_get(const hp_parser *p, const char *buf, const char *name, size_t *vlen) {
    char lname[64];
    size_t n = strlen(name);
    if (n >= sizeof lname) return NULL;
    for (size_t i = 0; i < n; i++) lname[i] = (char)lower((unsigned char)name[i]);
    lname[n] = '\0';
    for (int i = 0; i < p->nheaders; i++) {
        const hp_header *h = &p->headers[i];
        if (ci_eq(buf + h->name_off, h->name_len, lname)) {
            if (vlen) *vlen = h->value_len;
            return buf + h->value_off;
        }
    }
    return NULL;
}
