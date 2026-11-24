#include "uri.h"

#include <string.h>
#include <strings.h>

int uri_split(struct sl s, enum http_method m, struct target *t)
{
    memset(t, 0, sizeof *t);
    if (memchr(s.p, '#', s.n))
        return 400;
    if (m == M_CONNECT) { /* 포트가 반드시 있어야 한다 */
        t->authority = s;
        return s.n && s.p[s.n - 1] >= '0' && s.p[s.n - 1] <= '9' && memchr(s.p, ':', s.n) ? 0 : 400;
    }
    if (sl_eq(s, "*")) {
        t->asterisk = true;
        return m == M_OPTIONS ? 0 : 400;
    }
    if (s.p[0] != '/') {
        /* 프록시에게 보내는 모양이지만, 일반 서버도 받아야 한다 (RFC 9112 3.2.2) */
        if (s.n <= 7 || strncasecmp(s.p, "http://", 7))
            return 400;
        s.p += 7;
        s.n -= 7;
        size_t i = 0;
        while (i < s.n && s.p[i] != '/' && s.p[i] != '?')
            i++;
        t->absolute = true;
        t->authority = (struct sl){s.p, i};
        s.p += i;
        s.n -= i;
        if (i == 0)
            return 400;
    }
    const char *q = memchr(s.p, '?', s.n);
    size_t plen = q ? (size_t)(q - s.p) : s.n;
    if (t->absolute && s.n == 0 && m == M_OPTIONS)
        t->asterisk = true; /* "OPTIONS http://host" = 서버 전체에 대한 질문 */
    t->path = plen ? (struct sl){s.p, plen} : (struct sl){"/", 1};
    if (q)
        t->query = (struct sl){q, s.n - plen};
    return 0;
}

int uri_host(struct sl a, int default_port, char *host, size_t cap, int *port)
{
    *port = default_port;
    size_t hn;
    if (a.n && a.p[0] == '[') {
        const char *close = memchr(a.p, ']', a.n);
        if (!close)
            return 400;
        hn = (size_t)(close - a.p) + 1;
    } else {
        const char *colon = memchr(a.p, ':', a.n);
        hn = colon ? (size_t)(colon - a.p) : a.n;
    }
    if (hn == 0 || hn >= cap)
        return 400;
    bool v6 = a.p[0] == '[';
    for (size_t i = 0; i < hn; i++) {
        char ch = a.p[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + 32);
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
                  ch == '_' || (v6 && (ch == ':' || (ch == '[' && i == 0) || (ch == ']' && i == hn - 1)));
        if (!ok)
            return 400;
        host[i] = ch;
    }
    host[hn] = '\0';
    if (hn < a.n) {
        long long v;
        if (a.p[hn] != ':')
            return 400;
        if (hn + 1 < a.n) {
            if (!sl_to_ll((struct sl){a.p + hn + 1, a.n - hn - 1}, &v) || v < 1 || v > 65535)
                return 400;
            *port = (int)v;
        }
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

static bool unreserved(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

int uri_normalize(struct sl in, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < in.n) {
        while (i < in.n && in.p[i] == '/')
            i++;
        size_t seg = o; /* 이 조각의 '/' 위치 */
        if (o + 1 >= cap)
            return 414;
        out[o++] = '/';
        while (i < in.n && in.p[i] != '/') {
            char ch = in.p[i];
            if (o + 4 >= cap)
                return 414;
            if (ch == '%') {
                int hi = i + 2 < in.n ? hexval(in.p[i + 1]) : -1;
                int lo = i + 2 < in.n ? hexval(in.p[i + 2]) : -1;
                int v = hi * 16 + lo;
                if (hi < 0 || lo < 0 || v < 0x20 || v == 0x7f || v == '/' || v == '\\' || v == '.')
                    return 400;
                if (unreserved(v)) {
                    out[o++] = (char)v;
                } else {
                    out[o++] = '%';
                    out[o++] = "0123456789ABCDEF"[hi];
                    out[o++] = "0123456789ABCDEF"[lo];
                }
                i += 3;
            } else {
                if (!unreserved(ch) && !strchr("!$&'()*+,;=:@", ch))
                    return 400;
                out[o++] = ch;
                i++;
            }
        }
        size_t n = o - seg - 1;
        bool dot = n == 1 && out[seg + 1] == '.';
        bool dotdot = n == 2 && out[seg + 1] == '.' && out[seg + 2] == '.';
        if (dot || dotdot) {
            o = seg;
            if (dotdot) {
                if (seg == 0)
                    return 400; /* 루트 위로 올라가려 함 */
                while (out[--o] != '/')
                    ;
            }
            if (i >= in.n)
                out[o++] = '/'; /* "/a/.." 은 폴더 "/" 이다 */
        }
    }
    out[o] = '\0';
    return 0;
}

bool uri_decode(const char *path, char *out, size_t cap)
{
    size_t o = 0;
    for (; *path; path++) {
        if (o + 1 >= cap)
            return false;
        if (*path == '%') {
            out[o++] = (char)(hexval(path[1]) * 16 + hexval(path[2]));
            path += 2;
        } else {
            out[o++] = *path;
        }
    }
    out[o] = '\0';
    return true;
}
