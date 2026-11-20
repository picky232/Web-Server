#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct item {
    int fd;
    char ip[46];
};

/* 원형 큐 하나를 여러 스레드가 같이 쓰니 mutex로 감싼다.
   not_empty: 큐가 비어 있으면 작업 스레드는 여기서 잠들고, 메인 스레드가 넣을 때 깨운다 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_not_empty = PTHREAD_COND_INITIALIZER;
static struct item *g_items;
static int g_cap, g_head, g_count;
static int g_stopping;
static pthread_t *g_threads;
static int g_nthreads;
static void (*g_handler)(int, const char *);

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 0 && !g_stopping)
            pthread_cond_wait(&g_not_empty, &g_lock);
        if (g_stopping) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        struct item it = g_items[g_head];
        g_head = (g_head + 1) % g_cap;
        g_count--;
        pthread_mutex_unlock(&g_lock);

        g_handler(it.fd, it.ip); /* 락을 놓고 처리한다. 잡은 채로 처리하면 스레드가 하나뿐인 것과 같다 */
    }
}

int pool_start(int nthreads, int qsize, void (*handler)(int fd, const char *ip))
{
    g_items = calloc((size_t)qsize, sizeof *g_items);
    g_threads = calloc((size_t)nthreads, sizeof *g_threads);
    if (!g_items || !g_threads)
        return -1;
    g_cap = qsize;
    g_handler = handler;
    for (g_nthreads = 0; g_nthreads < nthreads; g_nthreads++) {
        int err = pthread_create(&g_threads[g_nthreads], NULL, worker, NULL);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            return -1;
        }
    }
    return 0;
}

int pool_submit(int fd, const char *ip)
{
    pthread_mutex_lock(&g_lock);
    if (g_count == g_cap) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    struct item *it = &g_items[(g_head + g_count) % g_cap];
    it->fd = fd;
    snprintf(it->ip, sizeof it->ip, "%s", ip);
    g_count++;
    pthread_cond_signal(&g_not_empty);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int pool_waiting(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void pool_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stopping = 1;
    pthread_cond_broadcast(&g_not_empty); /* 잠든 스레드를 전부 깨워 끝내게 한다 */
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_nthreads; i++)
        pthread_join(g_threads[i], NULL);
    for (; g_count > 0; g_count--) {
        close(g_items[g_head].fd);
        g_head = (g_head + 1) % g_cap;
    }
    free(g_items);
    free(g_threads);
}
