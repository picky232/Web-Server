#include "proxy_msg.h"
#include "router.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- 공통 유틸 ----------------------------- */

static unsigned char lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

static int ci_eq(const char *buf, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)buf[i]) != (unsigned char)lit[i]) return 0;
    return 1;
}

static int is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* 증가하는 문자열 버퍼 */
typedef struct { char *p; size_t len, cap; int oom; } sbuf;

static void sb_append(sbuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return; }
    if ((size_t)n < sizeof tmp) { sb_append(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) { b->oom = 1; return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_append(b, big, (size_t)n);
    free(big);
}

/* ------------------------------ 응답 파서 ------------------------------ */

static int find_crlf(const char *buf, size_t from, size_t end, size_t *at) {
    for (size_t i = from; i + 1 < end; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n') { *at = i; return 1; }
    return 0;
}

int pm_resp_parse(const char *buf, size_t len, pm_resp *r) {
    size_t end = 0;
    int found = 0;
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = i + 4;
            found = 1;
            break;
        }
    if (!found) return 0;

    memset(r, 0, sizeof *r);
    r->head_len = end;

    /* 상태 라인: "HTTP/1.x SSS[ reason]" */
    size_t le;
    if (!find_crlf(buf, 0, end, &le) || le < 12) return -1;
    if (memcmp(buf, "HTTP/1.", 7) != 0 || buf[7] < '0' || buf[7] > '9' || buf[8] != ' ') return -1;
    for (int i = 9; i < 12; i++)
        if (buf[i] < '0' || buf[i] > '9') return -1;
    if (le > 12 && buf[12] != ' ') return -1;
    r->minor  = buf[7] - '0';
    r->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    if (r->status < 100) return -1;
    if (le > 13) { r->reason_off = 13; r->reason_len = le - 13; }

    size_t pos = le + 2;
    while (pos < end) {
        size_t e;
        if (!find_crlf(buf, pos, end, &e)) return -1;
        if (e == pos) break;  /* 빈 줄 */
        if (buf[pos] == ' ' || buf[pos] == '\t') return -1;  /* obs-fold */

        size_t colon = pos;
        while (colon < e && buf[colon] != ':') colon++;
        if (colon == e || colon == pos) return -1;
        for (size_t i = pos; i < colon; i++)
            if (!is_tchar((unsigned char)buf[i])) return -1;

        size_t vs = colon + 1, ve = e;
        while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
        while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;

        if (r->nheaders >= HP_MAX_HEADERS) return -1;
        hp_header *h = &r->headers[r->nheaders++];
        h->name_off = pos;  h->name_len = colon - pos;
        h->value_off = vs;  h->value_len = ve - vs;

        const char *nm = buf + pos, *val = buf + vs;
        size_t nl = colon - pos, vl = ve - vs;

        if (ci_eq(nm, nl, "content-length")) {
            if (vl == 0) return -1;
            size_t v = 0;
            for (size_t i = 0; i < vl; i++) {
                if (val[i] < '0' || val[i] > '9') return -1;
                if (v > (SIZE_MAX - 9) / 10) return -1;
                v = v * 10 + (size_t)(val[i] - '0');
            }
            if (r->has_cl && r->content_length != v) return -1;  /* 서로 다른 중복 값 */
            r->has_cl = 1;
            r->content_length = v;
        } else if (ci_eq(nm, nl, "transfer-encoding")) {
            if (!ci_eq(val, vl, "chunked")) return -1;  /* gzip 등 다른 코딩은 지원하지 않음 */
            r->chunked = 1;
        } else if (ci_eq(nm, nl, "connection")) {
            size_t i = 0;
            while (i < vl) {
                while (i < vl && (val[i] == ' ' || val[i] == ',' || val[i] == '\t')) i++;
                size_t s = i;
                while (i < vl && val[i] != ',' && val[i] != ' ' && val[i] != '\t') i++;
                if (ci_eq(val + s, i - s, "close")) r->conn_close = 1;
            }
        }
        pos = e + 2;
    }
    if (r->chunked) r->has_cl = 0;  /* TE 가 CL 보다 우선 (RFC 7230 3.3.3) */
    return 1;
}

pm_body_mode pm_resp_body_mode(const pm_resp *r, int req_is_head) {
    if (req_is_head || r->status / 100 == 1 || r->status == 204 || r->status == 304) return BM_NONE;
    if (r->chunked) return BM_CHUNKED;
    if (r->has_cl) return r->content_length ? BM_LENGTH : BM_NONE;
    return BM_CLOSE;
}

/* ------------------------------ chunk 감지기 ------------------------------ */

enum { CS_SIZE, CS_EXT, CS_SIZE_LF, CS_DATA, CS_DATA_CR, CS_DATA_LF,
       CS_TRAILER, CS_TRAILER_LINE, CS_TRAILER_LF, CS_END_LF, CS_DONE };

void chunk_init(chunk_dec *d) { memset(d, 0, sizeof *d); d->st = CS_SIZE; }

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int chunk_feed(chunk_dec *d, const char *buf, size_t n, size_t *used) {
    size_t i = 0;
    while (i < n && d->st != CS_DONE) {
        char c = buf[i];
        switch (d->st) {
        case CS_SIZE: {
            int h = hexv(c);
            if (h >= 0) {
                if (d->digits >= 16 || d->remain > (SIZE_MAX >> 4)) return -1;
                d->remain = (d->remain << 4) | (size_t)h;
                d->digits++;
            } else if (c == ';' && d->digits > 0) d->st = CS_EXT;
            else if (c == '\r' && d->digits > 0) d->st = CS_SIZE_LF;
            else return -1;
            i++;
            break;
        }
        case CS_EXT:
            if (c == '\r') d->st = CS_SIZE_LF;
            i++;
            break;
        case CS_SIZE_LF:
            if (c != '\n') return -1;
            d->st = d->remain == 0 ? CS_TRAILER : CS_DATA;
            i++;
            break;
        case CS_DATA: {
            size_t take = n - i < d->remain ? n - i : d->remain;
            d->remain -= take;
            i += take;
            if (d->remain == 0) d->st = CS_DATA_CR;
            break;
        }
        case CS_DATA_CR:
            if (c != '\r') return -1;
            d->st = CS_DATA_LF;
            i++;
            break;
        case CS_DATA_LF:
            if (c != '\n') return -1;
            d->st = CS_SIZE;
            d->remain = 0;
            d->digits = 0;
            i++;
            break;
        case CS_TRAILER:  /* 줄의 시작: 빈 줄이면 끝 */
            d->st = (c == '\r') ? CS_END_LF : CS_TRAILER_LINE;
            i++;
            break;
        case CS_TRAILER_LINE:
            if (c == '\r') d->st = CS_TRAILER_LF;
            i++;
            break;
        case CS_TRAILER_LF:
            if (c != '\n') return -1;
            d->st = CS_TRAILER;
            i++;
            break;
        case CS_END_LF:
            if (c != '\n') return -1;
            d->st = CS_DONE;
            i++;
            break;
        default:
            return -1;
        }
    }
    *used = i;
    return d->st == CS_DONE ? 1 : 0;
}

/* ------------------------------ 헤더 재작성 ------------------------------ */

static int in_list(const char *name, size_t nlen, const char *const *list) {
    for (; *list; list++)
        if (ci_eq(name, nlen, *list)) return 1;
    return 0;
}

static const char *const k_req_skip[] = {
    "connection", "keep-alive", "proxy-connection", "proxy-authorization",
    "te", "trailer", "upgrade", "transfer-encoding", "expect", "x-forwarded-for", NULL
};

static const char *const k_resp_skip[] = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-connection",
    "te", "trailer", "upgrade", NULL
};

char *pm_build_request(const hp_parser *p, const char *rbuf, const char *peer_ip,
                       size_t strip_len, int upstream_ka, size_t *out_len) {
    sbuf b = {0};
    const char *method = p->method == HP_GET ? "GET" : p->method == HP_HEAD ? "HEAD" : "POST";

    const char *uri = rbuf + p->uri_off;
    size_t ulen = p->uri_len;
    size_t off = router_origin_off(uri, ulen);
    const char *o = uri + off;
    size_t ol = ulen - off;
    if (strip_len > 0) {
        size_t s = strip_len < ol ? strip_len : ol;
        o += s;
        ol -= s;
    }
    int need_slash = (ol == 0 || o[0] != '/');
    sb_printf(&b, "%s %s%.*s HTTP/1.1\r\n", method, need_slash ? "/" : "", (int)ol, o);

    int has_host = 0;
    const hp_header *xff = NULL;
    for (int i = 0; i < p->nheaders; i++) {
        const hp_header *h = &p->headers[i];
        const char *nm = rbuf + h->name_off;
        if (ci_eq(nm, h->name_len, "x-forwarded-for") && !xff) xff = h;
        if (in_list(nm, h->name_len, k_req_skip)) continue;
        if (ci_eq(nm, h->name_len, "host")) has_host = 1;
        sb_printf(&b, "%.*s: %.*s\r\n", (int)h->name_len, nm, (int)h->value_len, rbuf + h->value_off);
    }
    if (!has_host) sb_printf(&b, "Host: localhost\r\n");
    if (xff) sb_printf(&b, "X-Forwarded-For: %.*s, %s\r\n", (int)xff->value_len, rbuf + xff->value_off, peer_ip);
    else     sb_printf(&b, "X-Forwarded-For: %s\r\n", peer_ip);
    sb_printf(&b, "Connection: %s\r\n\r\n", upstream_ka ? "keep-alive" : "close");

    size_t blen = p->total_len - p->header_end;
    if (blen) sb_append(&b, rbuf + p->header_end, blen);

    if (b.oom) { free(b.p); return NULL; }
    *out_len = b.len;
    return b.p;
}

/* 상태 라인과 전달 가능한 헤더들을 쓴다. drop_age: 백엔드의 Age 는 우리가 새로 계산하므로 캐시용에서는 뺀다. */
static void write_forwardable(sbuf *b, const pm_resp *r, const char *buf, int drop_age) {
    sb_printf(b, "HTTP/1.1 %d %.*s\r\n", r->status, (int)r->reason_len, r->reason_len ? buf + r->reason_off : "");
    for (int i = 0; i < r->nheaders; i++) {
        const hp_header *h = &r->headers[i];
        const char *nm = buf + h->name_off;
        if (in_list(nm, h->name_len, k_resp_skip)) continue;
        if (r->chunked && ci_eq(nm, h->name_len, "content-length")) continue;
        if (drop_age && ci_eq(nm, h->name_len, "age")) continue;
        sb_printf(b, "%.*s: %.*s\r\n", (int)h->name_len, nm, (int)h->value_len, buf + h->value_off);
    }
}

char *pm_build_client_head(const pm_resp *r, const char *buf, int client_ka, const char *extra, size_t *out_len) {
    sbuf b = {0};
    write_forwardable(&b, r, buf, 0);
    if (extra) sb_append(&b, extra, strlen(extra));
    sb_printf(&b, "Via: 1.1 webserver\r\nConnection: %s\r\n\r\n", client_ka ? "keep-alive" : "close");
    if (b.oom) { free(b.p); return NULL; }
    *out_len = b.len;
    return b.p;
}

char *pm_build_stored_head(const pm_resp *r, const char *buf, size_t *out_len) {
    sbuf b = {0};
    write_forwardable(&b, r, buf, 1);
    if (b.oom) { free(b.p); return NULL; }
    *out_len = b.len;
    return b.p;
}
