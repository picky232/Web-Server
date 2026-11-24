#include "resolver.h"
#include "loop.h"

#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_MAX 256

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_more = PTHREAD_COND_INITIALIZER;
static struct resolve *g_head, *g_tail;
static int g_waiting;
static bool g_stop;

static void lookup(struct resolve *r)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(r->host, r->port, &hints, &res) != 0)
        return;
    for (struct addrinfo *a = res; a && r->n < RESOLVE_MAX; a = a->ai_next) {
        memset(&r->addr[r->n], 0, sizeof r->addr[r->n]);
        memcpy(&r->addr[r->n].ss, a->ai_addr, a->ai_addrlen);
        r->addr[r->n++].len = a->ai_addrlen;
    }
    freeaddrinfo(res);
    r->ok = r->n > 0;
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_head && !g_stop)
            pthread_cond_wait(&g_more, &g_lock);
        if (g_stop)
            break;
        struct resolve *r = g_head;
        g_head = r->next;
        if (!g_head)
            g_tail = NULL;
        g_waiting--;
        pthread_mutex_unlock(&g_lock);

        lookup(r); /* 여기서만 기다린다. 락은 놓은 상태 */

        struct msg m = {.type = MSG_RESOLVED, .ptr = r};
        pthread_mutex_lock(&g_lock);
        /* 종료가 시작됐으면 루프가 이미 사라졌을 수 있다 */
        while (!g_stop && !loop_send(r->loop, &m)) {
            pthread_mutex_unlock(&g_lock);
            usleep(1000); /* 쪽지함이 찼다. 이 스레드는 기다려도 된다 */
            pthread_mutex_lock(&g_lock);
        }
        if (g_stop)
            free(r);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int resolver_start(int threads)
{
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int err = 0;
    for (int i = 0; i < threads && !err; i++) {
        pthread_t t;
        err = pthread_create(&t, NULL, worker, NULL);
        if (!err)
            pthread_detach(t); /* 이름 해석은 중간에 멈출 수 없어, 종료할 때 기다리지 않는다 */
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return err ? -1 : 0;
}

struct resolve *resolver_ask(struct loop *l, struct conn *c, const char *host, int port)
{
    struct resolve *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[') { /* [::1] → ::1 */
        host++;
        n -= 2;
    }
    if (n >= sizeof r->host) {
        free(r);
        return NULL;
    }
    memcpy(r->host, host, n);
    snprintf(r->port, sizeof r->port, "%d", port);
    r->loop = l;
    r->c = c;
    pthread_mutex_lock(&g_lock);
    bool full = g_waiting >= QUEUE_MAX;
    if (!full) {
        if (g_tail)
            g_tail->next = r;
        else
            g_head = r;
        g_tail = r;
        g_waiting++;
        pthread_cond_signal(&g_more);
    }
    pthread_mutex_unlock(&g_lock);
    if (full) {
        free(r);
        return NULL;
    }
    return r;
}

void resolver_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stop = true;
    while (g_head) {
        struct resolve *r = g_head;
        g_head = r->next;
        free(r);
    }
    pthread_cond_broadcast(&g_more);
    pthread_mutex_unlock(&g_lock);
}
