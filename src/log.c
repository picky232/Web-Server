#include "log.h"
#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
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

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 막히지 않고 쓸 수 있는 만큼만 쓴다. 파이프는 "쓸 수 있음"이라고 해도 PIPE_BUF까지만 보장되므로,
   더 큰 덩어리를 한 번에 쓰면 그 안에서 잠들 수 있다. 루프가 잠들면 그 루프의 모든 손님이 멈춘다 */
static size_t write_some(int fd, const char *p, size_t n, bool regular)
{
    size_t done = 0;
    while (done < n) {
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        if (!regular && poll(&pf, 1, 0) != 1)
            break;
        size_t k = n - done;
        if (!regular && k > PIPE_BUF)
            k = PIPE_BUF;
        ssize_t w = write(fd, p + done, k);
        if (w < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        if (w <= 0)
            return n; /* 받는 쪽이 사라졌다. 쌓아 둘 이유가 없다 */
        done += (size_t)w;
    }
    return done;
}

void log_flush(struct loop *l)
{
    /* 확인과 쓰기 사이에 다른 루프가 자리를 가져가지 않게 한 번에 하나만. 바쁘면 다음 바퀴에 */
    if (!buf_len(&l->log) || pthread_mutex_trylock(&g_lock) != 0)
        return;
    buf_consume(&l->log, write_some(l->srv->log_fd, buf_head(&l->log), buf_len(&l->log), l->srv->log_regular));
    pthread_mutex_unlock(&g_lock);
}

void log_note(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    /* 락을 기다리지 않는다. 쥔 루프가 멎은 디스크에 쓰는 중일 수 있고, 메인까지 같이 멎으면 그 사실을 알릴 수도 없다.
       못 얻으면 줄이 섞일 수 있지만 그대로 쓴다 */
    bool locked = pthread_mutex_trylock(&g_lock) == 0;
    write_some(STDERR_FILENO, line, (size_t)n, false);
    if (locked)
        pthread_mutex_unlock(&g_lock);
}
