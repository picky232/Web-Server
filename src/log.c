#include "log.h"
#include "conn.h"
#include "loop.h"
#include "server.h"

#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void log_access(struct conn *c)
{
    struct loop *l = c->loop;
    const struct exchange *ex = c->ex;
    int status = ex->reply.started ? ex->reply.status : 0;
    atomic_fetch_add(&l->requests, 1);
    atomic_fetch_add(&l->bytes, ex->reply.sent);
    atomic_fetch_add(&l->by_class[status / 100 % 6], 1);
    if (l->srv->log_fd < 0)
        return;
    char when[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
    /* 쿼리 문자열과 헤더 값은 남기지 않는다. 토큰 같은 비밀 값이 들어 있을 수 있다 */
    const char *path = ex->path ? ex->path : "-";
    bool ok = buf_printf(&l->log, "%s %s \"%.*s %.200s\" %d %lld %lldms %s%s%s\n", when, c->ip,
                         (int)ex->req.method_text.n, ex->req.method_text.n ? ex->req.method_text.p : "", path,
                         status, ex->reply.sent, l->now - ex->start_ms, ex->served_by[0] ? ex->served_by : "-",
                         ex->tries > 1 ? " retried" : "", ex->reply.finished ? "" : " cut");
    if (!ok)
        atomic_fetch_add(&l->log_dropped, 1);
}

void log_flush(struct loop *l)
{
    if (!buf_len(&l->log))
        return;
    /* 로그를 받는 쪽(파이프)이 막혀 있으면 기다리지 않는다. 루프가 멈추면 모든 손님이 멈춘다 */
    struct pollfd p = {.fd = l->srv->log_fd, .events = POLLOUT};
    if (poll(&p, 1, 0) != 1)
        return;
    ssize_t w = write(l->srv->log_fd, buf_head(&l->log), buf_len(&l->log));
    buf_consume(&l->log, w > 0 ? (size_t)w : buf_len(&l->log));
}
