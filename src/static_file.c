#include "static_file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* absolute-form("http://host/path") 이면 스킴/호스트를 건너뛰고,
 * 쿼리('?')와 프래그먼트('#') 앞까지의 경로 구간을 구한다. */
static int extract_path(const char *uri, size_t len, size_t *off, size_t *plen) {
    size_t s = 0;
    if (len == 0) return -1;
    if (uri[0] != '/') {
        size_t i = 0;
        while (i + 2 < len && !(uri[i] == ':' && uri[i + 1] == '/' && uri[i + 2] == '/')) i++;
        if (i + 2 >= len) return -1;
        s = i + 3;
        while (s < len && uri[s] != '/') s++;
        if (s == len) { *off = 0; *plen = 0; return 1; }  /* "http://host" -> "/" */
    }
    size_t e = s;
    while (e < len && uri[e] != '?' && uri[e] != '#') e++;
    *off  = s;
    *plen = e - s;
    return 0;
}

/* 퍼센트 디코딩. 잘못된 시퀀스나 %00 은 실패. */
static int percent_decode(const char *src, size_t n, char *dst, size_t cap, size_t *outlen) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;  /* "%" 뒤에 16진수 두 자리가 있어야 함 */
            int h = hexval(src[i + 1]), l = hexval(src[i + 2]);
            if (h < 0 || l < 0) return -1;
            c = (unsigned char)(h * 16 + l);
            i += 2;
        }
        if (c == '\0') return -1;
        if (o + 1 >= cap) return -1;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    *outlen = o;
    return 0;
}

/* '.' 으로 시작하는 세그먼트가 있으면 1 */
static int has_dot_segment(const char *path) {
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] == '.') return 1;
    return 0;
}

static int under_docroot(const char *docroot, size_t dlen, const char *resolved) {
    if (strncmp(resolved, docroot, dlen) != 0) return 0;
    return resolved[dlen] == '\0' || resolved[dlen] == '/' || dlen == 1; /* docroot 가 "/" 인 경우 */
}

static sr_result errno_result(void) {
    switch (errno) {
    case ENOENT: case ENOTDIR:   return SR_NOT_FOUND;
    case EACCES:                 return SR_FORBIDDEN;
    case ENAMETOOLONG: case ELOOP: return SR_BAD;
    default:                     return SR_ERROR;
    }
}

sr_result static_resolve(const char *docroot, const char *uri, size_t uri_len, static_target *t) {
    size_t off, plen;
    int rc = extract_path(uri, uri_len, &off, &plen);
    if (rc < 0) return SR_BAD;
    t->path_off = off;
    t->path_len = plen;

    char dec[PATH_MAX];
    size_t dlen = 0;
    if (rc == 1 || plen == 0) { dec[0] = '/'; dec[1] = '\0'; dlen = 1; }
    else if (percent_decode(uri + off, plen, dec, sizeof dec, &dlen) < 0) return SR_BAD;
    if (dec[0] != '/') return SR_BAD;
    if (has_dot_segment(dec)) return SR_FORBIDDEN;

    size_t droot_len = strlen(docroot);
    char joined[PATH_MAX];
    if (droot_len + dlen + sizeof "/index.html" >= sizeof joined) return SR_BAD;
    memcpy(joined, docroot, droot_len);
    memcpy(joined + droot_len, dec, dlen + 1);

    char resolved[PATH_MAX];
    if (!realpath(joined, resolved)) return errno_result();
    if (!under_docroot(docroot, droot_len, resolved)) return SR_FORBIDDEN;  /* 심볼릭 링크 탈출 */

    struct stat st;
    if (stat(resolved, &st) < 0) return errno_result();

    if (S_ISDIR(st.st_mode)) {
        if (dec[dlen - 1] != '/') return SR_REDIRECT;
        size_t rl = strlen(resolved);
        if (rl + sizeof "/index.html" >= sizeof joined) return SR_BAD;
        memcpy(joined, resolved, rl);
        memcpy(joined + rl, "/index.html", sizeof "/index.html");
        if (!realpath(joined, resolved)) return errno == ENOENT ? SR_FORBIDDEN : errno_result(); /* 목록 노출 금지 */
        if (!under_docroot(docroot, droot_len, resolved)) return SR_FORBIDDEN;
        if (stat(resolved, &st) < 0) return errno_result();
    }

    if (!S_ISREG(st.st_mode)) return SR_FORBIDDEN;
    if (strlen(resolved) >= sizeof t->file) return SR_BAD;
    strcpy(t->file, resolved);
    return SR_OK;
}
