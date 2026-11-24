#include "static.h"
#include "config.h"
#include "conn.h"
#include "mime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define READ_CHUNK 32768

int status_from_errno(int e)
{
    switch (e) {
    case ENOENT: case ENOTDIR: case ENAMETOOLONG: return 404;
    case EACCES: case EPERM: case ELOOP: case EISDIR: case ENXIO: case EOPNOTSUPP: return 403;
    case EMFILE: case ENFILE: case ENOMEM: return 503; /* 우리 쪽 자원이 모자란 것. 파일이 없는 게 아니다 */
    default: return 500;
    }
}

int open_under(int root_fd, const char *rel, struct stat *st)
{
    char name[256];
    int dir = root_fd;
#ifdef FAULT_INJECT /* 시험 빌드 전용: 좀처럼 나지 않는 실패를 일부러 낸다 */
    if (!strncmp(rel, "/__fault/", 9)) {
        errno = !strcmp(rel + 9, "emfile") ? EMFILE : EIO;
        return -1;
    }
#endif
    for (;;) {
        while (*rel == '/')
            rel++;
        const char *slash = strchr(rel, '/');
        size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
        if (n >= sizeof name) {
            errno = ENAMETOOLONG;
            break;
        }
        memcpy(name, rel, n);
        name[n] = '\0';
        if (name[0] == '.') { /* .git, .env 같은 숨김 파일. ".."은 경로 정리에서 이미 사라졌다 */
            errno = EACCES;
            break;
        }
        /* 검사한 뒤 여는 사이에 링크로 바꿔치기할 틈이 없도록, 여는 동작 자체가 링크를 거절한다.
           O_NONBLOCK: 이름 있는 파이프를 열다가 루프가 멈추지 않게 */
        int fd = openat(dir, n ? name : ".", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        int e = errno;
        if (dir != root_fd)
            close(dir);
        if (fd < 0) {
            errno = e;
            return -1;
        }
        dir = fd;
        if (!slash) {
            if (fstat(fd, st) == 0)
                return fd;
            e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        rel = slash;
    }
    int e = errno;
    if (dir != root_fd)
        close(dir);
    errno = e;
    return -1;
}

bool etag_matches(struct sl h, const char *etag)
{
    if (sl_eq(sl_trim(h), "*"))
        return true;
    size_t n = strlen(etag);
    for (const char *p = h.p; p + n <= h.p + h.n; p++) /* W/"..." 도 같은 것으로 본다 (약한 비교) */
        if (!memcmp(p, etag, n))
            return true;
    return false;
}

static void redirect_dir(struct conn *c)
{
    struct exchange *ex = c->ex;
    char loc[HTTP_HEAD_MAX];
    /* 정리된 경로는 '/'가 하나로 시작하므로 "//다른호스트/"로 튀지 않는다 */
    int n = snprintf(loc, sizeof loc, "Location: %s/%.*s\r\n", ex->path, (int)ex->target.query.n,
                     ex->target.query.n ? ex->target.query.p : "");
    exchange_error(c, n > 0 && n < 900 ? 301 : 404, n > 0 && n < 900 ? loc : NULL);
}

static void static_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct static_st *s = &ex->u.file;
    s->fd = -1;
    char rel[HTTP_URI_MAX + 16];
    if (!uri_decode(ex->path + ex->route->prefix_len, rel, sizeof rel - 12)) {
        exchange_error(c, 404, NULL);
        return;
    }
    size_t n = strlen(rel);
    if (n == 0) { /* "/assets" 규칙에 "/assets"로 왔다 → "/assets/" */
        redirect_dir(c);
        return;
    }
    bool dir_url = rel[n - 1] == '/';
    if (dir_url)
        strcpy(rel + n, "index.html");
    struct stat st;
    s->fd = open_under(ex->route->root_fd, rel, &st);
    if (s->fd < 0) {
        int status = status_from_errno(errno);
        if (status == 404 && dir_url) { /* 폴더는 있는데 index.html이 없으면 목록을 보여 주지 않는다 */
            rel[n] = '\0';
            int d = open_under(ex->route->root_fd, rel, &st);
            if (d >= 0) {
                close(d);
                status = 403;
            }
        }
        exchange_error(c, status, NULL);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        redirect_dir(c);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        exchange_error(c, 403, NULL);
        return;
    }
#ifdef __APPLE__
    long nsec = st.st_mtimespec.tv_nsec;
#else
    long nsec = st.st_mtim.tv_nsec;
#endif
    char etag[64], modified[64], extra[512];
    snprintf(etag, sizeof etag, "\"%llx-%lx-%llx\"", (long long)st.st_mtime, nsec, (long long)st.st_size);
    struct tm tm;
    gmtime_r(&st.st_mtime, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    int en = snprintf(extra, sizeof extra, "Content-Type: %s\r\nETag: %s\r\nLast-Modified: %s\r\n",
                      mime_type(rel), etag, modified);

    /* 브라우저가 이미 같은 것을 갖고 있으면 본문 없이 304 */
    const struct field *inm = fields_find(&ex->req.h, "If-None-Match");
    const struct field *ims = fields_find(&ex->req.h, "If-Modified-Since");
    /* shortcut: 날짜는 우리가 보낸 값과 글자가 같은지만 본다. 다른 형식의 날짜를 보내는 클라이언트가 생기면 해석 */
    bool same = inm ? etag_matches(inm->value, etag) : ims && sl_eq(ims->value, modified);
    if (!reply_start(&ex->reply, &c->out, same ? 304 : 200, st.st_size, extra, (size_t)en, 0)) {
        exchange_abort(c);
        return;
    }
    s->off = 0;
    s->left = same || ex->reply.head_only ? 0 : st.st_size; /* 이후 파일이 커져도 약속한 만큼만 */
}

static bool static_pump(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct static_st *s = &ex->u.file;
    if (s->left == 0) {
        exchange_finish(c);
        return true;
    }
    size_t n = reply_room(&ex->reply, &c->out);
    if (n > READ_CHUNK)
        n = READ_CHUNK;
    if ((long long)n > s->left)
        n = (size_t)s->left;
    if (n == 0)
        return false; /* 클라이언트가 느리다. 보낸 뒤에 이어서 */
    /* shortcut: 커널 → 우리 메모리 → 커널로 복사한다. 측정에서 병목이면 sendfile로 바꾼다 */
    char buf[READ_CHUNK];
    ssize_t r = pread(s->fd, buf, n, (off_t)s->off);
    if (r <= 0) { /* 보내는 도중 파일이 줄었거나 읽기 오류. 길이를 이미 약속했으니 끊어서 알린다 */
        exchange_abort(c);
        return true;
    }
    reply_write(&ex->reply, &c->out, buf, (size_t)r);
    s->off += r;
    s->left -= r;
    return true;
}

static void static_stop(struct conn *c)
{
    if (c->ex->u.file.fd >= 0)
        close(c->ex->u.file.fd);
}

const struct handler static_handler = {
    .name = "static", .start = static_start, .pump = static_pump, .stop = static_stop,
};
