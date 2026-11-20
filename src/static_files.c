/*
 * static_files.c — 정적 파일 서빙과 document root 탈출 방지.
 *
 * 경로 보안은 두 겹이다.
 *   1겹 (문자열): static_normalize_target() 이 %XX 디코딩 후 "." / ".." 을
 *          정리한다. 루트 위로 올라가려는 ".." 는 여기서 403.
 *   2겹 (파일시스템): realpath() 로 심볼릭 링크까지 모두 풀어낸 실제 경로가
 *          여전히 루트 아래인지 path_is_within() 으로 확인한다.
 *          (www/evil -> /etc 같은 링크를 1겹만으로는 막을 수 없다.)
 */
#include "static_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net_io.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int static_normalize_target(const char *target, char *out, size_t outsz,
                            bool allow_hidden)
{
    if (outsz < 2)
        return 500;

    /* 1) query("?...")와 fragment("#...")는 파일 경로가 아니다 */
    size_t plen = strcspn(target, "?#");
    if (plen > HTTP_MAX_TARGET_LEN)
        return 414;

    /* 2) percent-decoding (딱 한 번: "%252e" 는 "%2e" 라는 글자 그대로 남는다) */
    char dec[HTTP_MAX_TARGET_LEN + 1];
    size_t dl = 0;
    for (size_t i = 0; i < plen; i++) {
        char ch = target[i];
        if (ch == '%') {
            if (i + 2 >= plen)
                return 400; /* "%" 또는 "%4" 로 끝남 */
            int hi = hexval(target[i + 1]);
            int lo = hexval(target[i + 2]);
            if (hi < 0 || lo < 0)
                return 400; /* "%zz" */
            ch = (char)(hi * 16 + lo);
            i += 2;
        }
        if ((unsigned char)ch < 0x20 || ch == 0x7f)
            return 400; /* %00(NUL), %0A(개행) 등 제어 문자 */
        dec[dl++] = ch;
    }
    dec[dl] = '\0';
    if (dl == 0 || dec[0] != '/')
        return 400;

    /* 3) 세그먼트 단위 정규화. out 은 항상 "/seg1/seg2" 형태를 유지한다. */
    size_t olen = 0;
    bool last_was_dot = false;
    size_t i = 0;
    while (i < dl) {
        while (i < dl && dec[i] == '/')
            i++; /* "//" 는 "/" 와 같게 취급 */
        size_t s = i;
        while (i < dl && dec[i] != '/')
            i++;
        size_t segl = i - s;
        if (segl == 0)
            break; /* 끝의 '/' */
        const char *seg = dec + s;
        last_was_dot = false;

        if (segl == 1 && seg[0] == '.') {
            last_was_dot = true;
            continue; /* "." 은 현재 디렉터리 → 무시 */
        }
        if (segl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (olen == 0)
                return 403; /* 루트 위로 올라가려는 시도 */
            while (olen > 0 && out[olen - 1] != '/')
                olen--;
            olen--; /* 그 '/' 까지 지운다: "/a/b" → "/a" */
            last_was_dot = true;
            continue;
        }
        if (!allow_hidden && seg[0] == '.')
            return 403; /* .git, .env 같은 숨김 파일은 서빙하지 않는다 */
        if (olen + 1 + segl + 2 > outsz)
            return 414;
        out[olen++] = '/';
        memcpy(out + olen, seg, segl);
        olen += segl;
    }

    if (olen == 0) {
        out[olen++] = '/';
    } else if (dec[dl - 1] == '/' || last_was_dot) {
        out[olen++] = '/'; /* 디렉터리를 요청했다는 표시 유지 */
    }
    out[olen] = '\0';
    return 0;
}

const char *mime_type_for_path(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"txt", "text/plain; charset=utf-8"},
        {"md", "text/plain; charset=utf-8"},
        {"xml", "application/xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"webp", "image/webp"},
        {"pdf", "application/pdf"},
        {"wasm", "application/wasm"},
        {"woff2", "font/woff2"},
        {NULL, NULL},
    };
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0')
        return "application/octet-stream";
    for (int i = 0; table[i].ext != NULL; i++)
        if (strcasecmp(dot + 1, table[i].ext) == 0)
            return table[i].type;
    return "application/octet-stream";
}

bool path_is_within(const char *root, const char *resolved)
{
    size_t rl = strlen(root);
    if (rl == 1 && root[0] == '/')
        return resolved[0] == '/';
    return strncmp(resolved, root, rl) == 0 &&
           (resolved[rl] == '/' || resolved[rl] == '\0');
}

static int errno_to_status(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENAMETOOLONG:
        return 414;
    default:
        return 500;
    }
}

/* 오류 응답을 보내고 결과를 기록하는 공통 경로 */
static response_info_t reply_error(int fd, int status, bool keep_alive, bool head_only,
                                   const char *extra, int send_timeout_ms)
{
    response_info_t info = {.status = status, .body_bytes = 0, .keep_open = keep_alive};
    if (http_send_error(fd, status, keep_alive, head_only, extra,
                        now_ms() + send_timeout_ms) != 0)
        info.keep_open = false;
    return info;
}

response_info_t static_serve(int fd, const char *root_real, const http_request_t *req,
                             bool keep_alive, int send_timeout_ms)
{
    bool head_only = (req->method == HTTP_METHOD_HEAD);

    /* ---- 1) URL → 루트 기준 상대 경로 ---- */
    char rel[HTTP_MAX_TARGET_LEN + 16];
    int st = static_normalize_target(req->target, rel, sizeof rel, false);
    if (st != 0)
        return reply_error(fd, st, keep_alive, head_only, NULL, send_timeout_ms);

    /* ---- 2) 루트 + 상대 경로 (끝이 '/' 이면 index.html) ---- */
    char full[PATH_MAX];
    size_t rl = strlen(rel);
    int n = snprintf(full, sizeof full, "%s%s%s", root_real, rel,
                     rel[rl - 1] == '/' ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof full)
        return reply_error(fd, 414, keep_alive, head_only, NULL, send_timeout_ms);

    /* ---- 3) realpath: 심볼릭 링크를 풀고 루트 밖이면 403 ---- */
    char resolved[PATH_MAX];
    if (realpath(full, resolved) == NULL)
        return reply_error(fd, errno_to_status(errno), keep_alive, head_only, NULL,
                           send_timeout_ms);
    if (!path_is_within(root_real, resolved))
        return reply_error(fd, 403, keep_alive, head_only, NULL, send_timeout_ms);

    /* ---- 4) open + fstat (열린 fd 기준으로 크기/종류 확인) ---- */
    int ffd = open(resolved, O_RDONLY);
    if (ffd < 0)
        return reply_error(fd, errno_to_status(errno), keep_alive, head_only, NULL,
                           send_timeout_ms);
    struct stat sb;
    if (fstat(ffd, &sb) < 0) {
        close(ffd);
        return reply_error(fd, 500, keep_alive, head_only, NULL, send_timeout_ms);
    }
    if (S_ISDIR(sb.st_mode)) {
        /* "/docs" 처럼 '/' 없이 디렉터리를 요청 → "/docs/" 로 안내(301).
         * 그래야 그 안의 상대 링크(style.css 등)가 올바르게 풀린다. */
        close(ffd);
        size_t plen = strcspn(req->target, "?#");
        char loc[HTTP_MAX_TARGET_LEN + 32];
        snprintf(loc, sizeof loc, "Location: %.*s/%s\r\n", (int)plen, req->target,
                 req->target + plen);
        return reply_error(fd, 301, keep_alive, head_only, loc, send_timeout_ms);
    }
    if (!S_ISREG(sb.st_mode)) {
        close(ffd);
        return reply_error(fd, 403, keep_alive, head_only, NULL, send_timeout_ms);
    }

    /* ---- 5) 헤더 → 파일 내용(바이너리 그대로) 전송 ---- */
    response_info_t info = {.status = 200, .body_bytes = 0, .keep_open = keep_alive};
    if (http_send_head(fd, 200, mime_type_for_path(full), (unsigned long long)sb.st_size,
                       keep_alive, NULL, now_ms() + send_timeout_ms) != 0) {
        info.keep_open = false;
        close(ffd);
        return info;
    }
    if (head_only) {
        close(ffd);
        return info;
    }

    char chunk[64 * 1024];
    off_t left = sb.st_size;
    while (left > 0) {
        size_t want = (size_t)(left < (off_t)sizeof chunk ? left : (off_t)sizeof chunk);
        ssize_t r = read(ffd, chunk, want);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            /* 파일이 전송 중 줄어듦: 약속한 Content-Length 를 못 채우므로 연결을 닫는다 */
            info.keep_open = false;
            break;
        }
        /* 청크마다 새 마감 시각: 느리지만 진행 중인 다운로드는 끊지 않는다 */
        if (send_all(fd, chunk, (size_t)r, now_ms() + send_timeout_ms) != 0) {
            info.keep_open = false; /* 클라이언트가 떠남(EPIPE 등) */
            break;
        }
        left -= r;
        info.body_bytes += (unsigned long long)r;
    }
    close(ffd);
    return info;
}
