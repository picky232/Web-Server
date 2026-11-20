/*
 * log.c — 한 줄을 버퍼에 완성한 뒤 fputs 한 번으로 출력한다.
 * stdio 스트림은 내부적으로 lock 을 잡으므로, 여러 스레드가 동시에 써도
 * 줄이 서로 섞이지 않는다.
 */
#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "thread_pool.h"

static atomic_bool g_quiet = false;

void log_set_quiet(bool quiet)
{
    atomic_store(&g_quiet, quiet);
}

static void vlog(const char *level, const char *fmt, va_list ap)
{
    char line[2048];
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    char who[16];
    int wid = tp_worker_id();
    if (wid > 0)
        snprintf(who, sizeof who, "w%d", wid);
    else
        snprintf(who, sizeof who, "main");

    int n = snprintf(line, sizeof line, "[%s] [%s] %s", ts, who, level);
    if (n < 0 || (size_t)n >= sizeof line)
        return;
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    fprintf(stderr, "%s\n", line);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("ERROR: ", fmt, ap);
    va_end(ap);
}

void log_access(const char *fmt, ...)
{
    if (atomic_load(&g_quiet))
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}
