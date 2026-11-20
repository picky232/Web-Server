#include "static.h"
#include "mime.h"
#include "net.h"
#include "response.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[PATH_MAX];
static size_t g_root_len;

int static_init(const char *root)
{
    if (!realpath(root, g_root)) {
        perror(root);
        return -1;
    }
    g_root_len = strlen(g_root);
    return 0;
}

static int reply_error(struct conn *c, int status, const char *extra, int head)
{
    return send_error(c->fd, status, c->keep, extra, head) < 0 ? -1 : status;
}

int static_serve(struct conn *c, const char *path)
{
    int head = !strcmp(c->req.method, "HEAD");
    if (!head && strcmp(c->req.method, "GET"))
        return reply_error(c, 405, "Allow: GET, HEAD\r\n", 0);

    /* "/sub/" 처럼 /로 끝나면 그 폴더의 index.html */
    char fs[PATH_MAX];
    size_t plen = strlen(path);
    int n = snprintf(fs, sizeof fs, "%s%s%s", g_root, path, path[plen - 1] == '/' ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof fs)
        return reply_error(c, 414, NULL, head);

    /* realpath가 ..과 심볼릭 링크를 전부 풀어 준다. 그 결과가 루트 밖이면 거절 */
    char real[PATH_MAX];
    if (!realpath(fs, real))
        return reply_error(c, errno == EACCES ? 403 : 404, NULL, head);
    if (strncmp(real, g_root, g_root_len) || (real[g_root_len] != '/' && real[g_root_len] != '\0'))
        return reply_error(c, 403, NULL, head);

    struct stat st;
    if (stat(real, &st) < 0)
        return reply_error(c, 404, NULL, head);
    if (S_ISDIR(st.st_mode)) {
        /* "/sub" → "/sub/" 로 다시 오라고 알려 준다. 그래야 페이지 안 상대 경로가 맞는다 */
        const char *t = c->req.target;
        size_t q = strcspn(t, "?");
        char loc[URI_MAX + 32];
        snprintf(loc, sizeof loc, "Location: %.*s/%s\r\n", (int)q, t, t + q);
        return reply_error(c, 301, loc, head);
    }
    if (!S_ISREG(st.st_mode))
        return reply_error(c, 403, NULL, head);

    int fd = open(real, O_RDONLY);
    if (fd < 0)
        return reply_error(c, errno == EACCES ? 403 : 404, NULL, head);
    if (fstat(fd, &st) < 0 ||
        send_head(c->fd, 200, mime_type(real), (long long)st.st_size, c->keep, NULL) < 0) {
        close(fd);
        return -1;
    }
    if (!head) {
        /* 큰 파일도 64KB씩 읽어서 보낸다. 파일 전체를 메모리에 올리지 않는다 */
        char buf[65536];
        ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0)
            if (net_send_all(c->fd, buf, (size_t)r) < 0)
                break;
        if (r != 0) {
            close(fd);
            return -1; /* 중간에 끊겼다. 이미 헤더를 보냈으니 연결을 닫는 수밖에 없다 */
        }
    }
    close(fd);
    return 200;
}
