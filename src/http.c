#include "http.h"

#include <string.h>

enum scan http_scan_head(const char *p, size_t n, size_t *pos)
{
    for (size_t i = *pos; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch == '\r') {
            if (i + 1 == n) { /* 다음 바이트를 봐야 판단할 수 있다 */
                *pos = i;
                return SCAN_MORE;
            }
            if (p[i + 1] != '\n')
                return SCAN_BAD;
        } else if (ch == '\n') {
            /* LF만 온 줄을 받아 주면, 우리와 백엔드가 줄의 끝을 다르게 볼 수 있다 */
            if (i == 0 || p[i - 1] != '\r')
                return SCAN_BAD;
            if (i >= 3 && p[i - 2] == '\n') {
                *pos = i + 1;
                return SCAN_END;
            }
        } else if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            return SCAN_BAD; /* TLS로 잘못 접속한 바이트도 여기서 바로 걸린다 */
        }
    }
    *pos = n;
    return SCAN_MORE;
}

static bool is_tchar(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

bool http_is_token(struct sl s)
{
    for (size_t i = 0; i < s.n; i++)
        if (!is_tchar((unsigned char)s.p[i]))
            return false;
    return s.n > 0;
}

/* "HTTP/1.x" → 0/1. 형식이 틀리면 -400, 모르는 주 버전이면 -505 */
static int parse_version(struct sl v)
{
    if (v.n != 8 || memcmp(v.p, "HTTP/", 5) || v.p[6] != '.' || v.p[5] < '0' || v.p[5] > '9' ||
        v.p[7] < '0' || v.p[7] > '9')
        return -400;
    if (v.p[5] != '1')
        return -505;
    return v.p[7] == '0' ? 0 : 1; /* 1.2 같은 것은 우리가 아는 1.1로 다룬다 (RFC 9110 2.5) */
}

static enum http_method method_of(struct sl s)
{
    static const char *const names[] = {"GET", "HEAD", "POST", "PUT", "DELETE",
                                        "PATCH", "OPTIONS", "TRACE", "CONNECT"};
    for (int i = 0; i < M_OTHER; i++)
        if (sl_eq(s, names[i])) /* 메서드는 대소문자를 구분한다 */
            return (enum http_method)i;
    return M_OTHER;
}

/* 시작 줄 다음부터 빈 줄 앞까지. 0, 400, 431 */
static int parse_fields(const char *p, const char *end, struct fields *h, bool response)
{
    h->n = 0;
    while (p < end) {
        const char *eol = memchr(p, '\r', (size_t)(end - p));
        if (!eol || eol == p)
            break;
        if (*p == ' ' || *p == '\t')
            return 400; /* 옛날 줄 이어 쓰기. 해석이 갈려 공격에 쓰이므로 거절 */
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon)
            return 400;
        struct sl name = {p, (size_t)(colon - p)};
        if (response) /* 응답은 콜론 앞 공백을 지우고 받는다. 요청은 거절해야 한다 (RFC 9112 5.1) */
            name = sl_trim(name);
        if (!http_is_token(name))
            return 400;
        if (h->n == HTTP_MAX_FIELDS)
            return 431;
        h->f[h->n].name = name;
        h->f[h->n].value = sl_trim((struct sl){colon + 1, (size_t)(eol - colon - 1)});
        h->n++;
        p = eol + 2;
    }
    return 0;
}

const struct field *fields_find(const struct fields *h, const char *name)
{
    for (int i = 0; i < h->n; i++)
        if (sl_eqi(h->f[i].name, name))
            return &h->f[i];
    return NULL;
}

static int count(const struct fields *h, const char *name)
{
    int n = 0;
    for (int i = 0; i < h->n; i++)
        n += sl_eqi(h->f[i].name, name);
    return n;
}

bool fields_token(const struct fields *h, const char *name, const char *token)
{
    for (int i = 0; i < h->n; i++)
        if (sl_eqi(h->f[i].name, name) && sl_list_has(h->f[i].value, token))
            return true;
    return false;
}

bool fields_hop_by_hop(const struct fields *h, struct sl name)
{
    static const char *const list[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE",
                                       "Trailer", "Transfer-Encoding", "Upgrade",
                                       "Proxy-Authorization", "Proxy-Authenticate"};
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        if (sl_eqi(name, list[i]))
            return true;
    char buf[128]; /* "Connection: X-Secret"처럼 상대가 직접 지정한 이름도 이 구간 전용이다 */
    return sl_copy(name, buf, sizeof buf) && fields_token(h, "Connection", buf);
}

static bool keep_alive(const struct fields *h, int minor)
{
    if (fields_token(h, "Connection", "close"))
        return false;
    return minor >= 1 || fields_token(h, "Connection", "keep-alive");
}

/* 본문의 끝을 정하는 두 헤더를 검사한다. 여기서 틀리면 다음 요청의 시작 위치가 어긋난다 */
static int request_framing(struct http_req *r)
{
    const struct field *cl = fields_find(&r->h, "Content-Length");
    const struct field *te = fields_find(&r->h, "Transfer-Encoding");
    r->length = -1;
    if (cl && (te || count(&r->h, "Content-Length") > 1 || !sl_to_ll(cl->value, &r->length)))
        return 400;
    if (!te)
        return 0;
    if (r->minor == 0 || count(&r->h, "Transfer-Encoding") > 1)
        return 400;
    r->chunked = sl_eqi(te->value, "chunked");
    if (r->chunked)
        return 0;
    /* 마지막이 chunked면 앞 코딩을 모르는 것(501), 아니면 본문의 끝을 알 수 없는 것(400) */
    const char *last = te->value.p + te->value.n;
    bool ends_chunked = te->value.n > 7 && sl_eqi((struct sl){last - 7, 7}, "chunked") &&
                        (last[-8] == ' ' || last[-8] == ',' || last[-8] == '\t');
    return ends_chunked ? 501 : 400;
}

int http_parse_request(const char *p, size_t n, struct http_req *r)
{
    memset(r, 0, sizeof *r);
    const char *end = p + n;
    const char *eol = memchr(p, '\r', n);
    const char *sp1 = memchr(p, ' ', (size_t)(eol - p));
    const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
    if (!sp2)
        return 400;
    r->method_text = (struct sl){p, (size_t)(sp1 - p)};
    r->target = (struct sl){sp1 + 1, (size_t)(sp2 - sp1 - 1)};
    if (!http_is_token(r->method_text) || r->target.n == 0)
        return 400;
    for (size_t i = 0; i < r->target.n; i++)
        if (r->target.p[i] <= 0x20 || r->target.p[i] > 0x7e) /* char가 음수인 바이트도 걸린다 */
            return 400;
    r->minor = parse_version((struct sl){sp2 + 1, (size_t)(eol - sp2 - 1)});
    if (r->minor < 0)
        return -r->minor;
    if (r->target.n > HTTP_URI_MAX)
        return 414;
    int st = parse_fields(eol + 2, end, &r->h, false);
    if (st)
        return st;

    int hosts = count(&r->h, "Host");
    if (hosts > 1 || (r->minor >= 1 && hosts == 0))
        return 400;
    if (hosts)
        r->host = fields_find(&r->h, "Host")->value;
    st = request_framing(r);
    if (st)
        return st;
    r->keep = keep_alive(&r->h, r->minor);
    const struct field *ex = fields_find(&r->h, "Expect");
    if (ex && r->minor >= 1) {
        r->expect_100 = sl_eqi(ex->value, "100-continue");
        r->expect_other = !r->expect_100;
    }
    r->method = method_of(r->method_text);
    return r->method == M_OTHER || r->method == M_TRACE ? 501 : 0;
}

bool http_parse_response(const char *p, size_t n, struct http_res *r)
{
    memset(r, 0, sizeof *r);
    const char *eol = memchr(p, '\r', n);
    if (eol - p < 12 || p[8] != ' ')
        return false;
    r->minor = parse_version((struct sl){p, 8});
    if (r->minor < 0)
        return false;
    for (int i = 9; i < 12; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        r->status = r->status * 10 + (p[i] - '0');
    }
    if (r->status < 100 || (eol - p > 12 && p[12] != ' '))
        return false;
    if (eol - p > 13)
        r->reason = (struct sl){p + 13, (size_t)(eol - p - 13)};
    if (parse_fields(eol + 2, p + n, &r->h, true))
        return false;

    const struct field *te = fields_find(&r->h, "Transfer-Encoding");
    if (te && r->minor == 0)
        return false;
    r->length = -1;
    if (te) { /* Transfer-Encoding이 있으면 Content-Length는 무시한다 (RFC 9112 6.3) */
        r->chunked = count(&r->h, "Transfer-Encoding") == 1 && sl_eqi(te->value, "chunked");
        r->until_close = !r->chunked;
    } else {
        for (int i = 0; i < r->h.n; i++) {
            long long v;
            if (!sl_eqi(r->h.f[i].name, "Content-Length"))
                continue;
            if (!sl_to_ll(r->h.f[i].value, &v) || (r->length >= 0 && r->length != v))
                return false;
            r->length = v;
        }
    }
    r->keep = keep_alive(&r->h, r->minor);
    return true;
}
