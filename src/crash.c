#include "crash.h"
#include "conn.h"
#include "server.h"

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STACK_SIZE 65536

static const int g_fatal[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP};

void crash_unblock(sigset_t *set)
{
    for (size_t i = 0; i < sizeof g_fatal / sizeof g_fatal[0]; i++)
        sigdelset(set, g_fatal[i]);
}

/* sanitizer 빌드는 자기 처리기로 더 자세히 알려 준다. 덮어쓰지 않는다 */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

#ifdef SANITIZED
void crash_install(struct server *s) { (void)s; }
void *crash_thread_begin(void) { return NULL; }
void crash_thread_end(void *stack) { (void)stack; }
#else

static struct server *g_srv;

/* 이 안에서는 printf도 malloc도 쓸 수 없다. 죽는 순간에 그 함수들이 쓰던 락이 잡혀 있을 수 있다 */
struct line {
    char p[400];
    size_t n;
};

static void add(struct line *l, const char *p, size_t n)
{
    for (size_t i = 0; i < n && l->n < sizeof l->p; i++)
        l->p[l->n++] = p[i];
}

static void add_num(struct line *l, int v)
{
    char d[12];
    size_t n = sizeof d;
    do
        d[--n] = (char)('0' + v % 10);
    while ((v /= 10) && n);
    add(l, d + n, sizeof d - n);
}

/* 로그를 받는 쪽이 막혀 있으면 쓰지 않는다. 여기서 잠들면 죽어야 할 프로세스가 죽지 못한다 */
static void emit(const struct line *l)
{
    struct pollfd p = {.fd = STDERR_FILENO, .events = POLLOUT};
    if (poll(&p, 1, 0) == 1)
        (void)!write(STDERR_FILENO, l->p, l->n);
}

static void on_crash(int sig)
{
    signal(sig, SIG_DFL); /* SA_RESETHAND는 SIGILL·SIGTRAP에는 듣지 않는다 (POSIX). 안 하면 끝없이 되돌아온다 */
    struct line l = {.n = 0};
    add(&l, "죽음: 시그널 ", strlen("죽음: 시그널 "));
    add_num(&l, sig);
    add(&l, "\n", 1);
    emit(&l);
    for (int i = 0; i < g_srv->nloops; i++) {
        struct conn *c = atomic_load_explicit(&g_srv->loops[i].current, memory_order_relaxed);
        if (!c || !c->ex)
            continue;
        l.n = 0;
        add(&l, "  처리 중이던 요청: 루프 ", strlen("  처리 중이던 요청: 루프 "));
        add_num(&l, i);
        add(&l, " ", 1);
        add(&l, c->ex->req.method_text.p, c->ex->req.method_text.n);
        add(&l, " ", 1);
        if (c->ex->path) /* 쿼리는 남기지 않는다 */
            add(&l, c->ex->path, strnlen(c->ex->path, 200));
        add(&l, "\n", 1);
        emit(&l);
    }
    raise(sig); /* 기본 처리로 돌려놓았으니, 이 시그널로 죽는다 */
}

void *crash_thread_begin(void)
{
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_size = STACK_SIZE;
    ss.ss_sp = malloc(STACK_SIZE);
    if (ss.ss_sp)
        sigaltstack(&ss, NULL);
    return ss.ss_sp;
}

void crash_thread_end(void *stack)
{
    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    off.ss_size = STACK_SIZE; /* macOS는 끌 때도 크기를 본다 */
    sigaltstack(&off, NULL);
    free(stack);
}

void crash_install(struct server *s)
{
    g_srv = s;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_crash;
    sa.sa_flags = (int)(SA_RESETHAND | SA_ONSTACK);
    for (size_t i = 0; i < sizeof g_fatal / sizeof g_fatal[0]; i++)
        sigaction(g_fatal[i], &sa, NULL);
}
#endif
