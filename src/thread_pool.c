/*
 * thread_pool.c — 생산자 1개(accept 루프) / 소비자 N개(worker) 구조.
 *
 * 스레드 안전성 규칙
 *   - q->items/head/count/closed 는 반드시 q->mu 를 잡은 상태에서만 읽고 쓴다.
 *   - pthread_cond_wait 는 while 루프 안에서 호출한다(spurious wakeup 대비).
 *   - tp->threads/nthreads 는 init/shutdown(main 스레드)만 건드린다.
 */
#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local int g_worker_id = 0;

int tp_worker_id(void)
{
    return g_worker_id;
}

/* ------------------------------------------------------------------ */
/* 연결 큐                                                              */
/* ------------------------------------------------------------------ */

int cq_init(conn_queue_t *q, size_t capacity)
{
    memset(q, 0, sizeof *q);
    if (capacity == 0)
        return -1;
    q->items = calloc(capacity, sizeof *q->items);
    if (q->items == NULL)
        return -1;
    q->capacity = capacity;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->items);
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q->items);
        return -1;
    }
    return 0;
}

void cq_destroy(conn_queue_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
    free(q->items);
    q->items = NULL;
}

cq_status_t cq_push(conn_queue_t *q, int fd)
{
    pthread_mutex_lock(&q->mu);
    if (q->closed) {
        pthread_mutex_unlock(&q->mu);
        return CQ_CLOSED;
    }
    if (q->count == q->capacity) {
        pthread_mutex_unlock(&q->mu);
        return CQ_FULL; /* 기다리지 않는다: accept 루프는 계속 돌아야 한다 */
    }
    size_t tail = (q->head + q->count) % q->capacity;
    q->items[tail] = fd;
    q->count++;
    pthread_cond_signal(&q->not_empty); /* 잠든 worker 하나를 깨운다 */
    pthread_mutex_unlock(&q->mu);
    return CQ_OK;
}

int cq_pop(conn_queue_t *q, int *fd_out)
{
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mu); /* 잠들면서 mutex 를 놓는다 */
    if (q->closed) {
        /* 종료 중에는 남은 항목이 있어도 새 일을 시작하지 않는다.
         * 남은 fd 는 tp_shutdown 이 정리한다. */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *fd_out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

int cq_try_pop(conn_queue_t *q, int *fd_out)
{
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *fd_out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

size_t cq_size(conn_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    size_t n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

void cq_close(conn_queue_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty); /* 잠든 worker 를 "모두" 깨운다 */
    pthread_mutex_unlock(&q->mu);
}

/* ------------------------------------------------------------------ */
/* 스레드 풀                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    thread_pool_t *tp;
    int id;
} worker_arg_t;

static void *worker_main(void *arg)
{
    worker_arg_t *wa = arg;
    thread_pool_t *tp = wa->tp;
    g_worker_id = wa->id;
    free(wa);

    int fd;
    while (cq_pop(&tp->queue, &fd) == 0)
        tp->handler(fd, tp->ctx); /* 연결 1개를 끝까지 처리 (keep-alive 포함) */
    return NULL;
}

int tp_init(thread_pool_t *tp, size_t nthreads, size_t queue_capacity,
            tp_handler_fn handler, void *ctx)
{
    memset(tp, 0, sizeof *tp);
    if (nthreads == 0 || cq_init(&tp->queue, queue_capacity) != 0)
        return -1;
    tp->threads = calloc(nthreads, sizeof *tp->threads);
    if (tp->threads == NULL) {
        cq_destroy(&tp->queue);
        return -1;
    }
    tp->handler = handler;
    tp->ctx = ctx;

    for (size_t i = 0; i < nthreads; i++) {
        worker_arg_t *wa = malloc(sizeof *wa);
        int rc = -1;
        if (wa != NULL) {
            wa->tp = tp;
            wa->id = (int)i + 1;
            rc = pthread_create(&tp->threads[i], NULL, worker_main, wa);
            if (rc != 0)
                free(wa);
        }
        if (rc != 0) {
            /* 일부만 만들어졌다면 그것들만 정리하고 실패를 알린다 */
            fprintf(stderr, "pthread_create failed for worker %zu\n", i + 1);
            tp->nthreads = i;
            cq_close(&tp->queue);
            for (size_t k = 0; k < i; k++)
                pthread_join(tp->threads[k], NULL);
            free(tp->threads);
            cq_destroy(&tp->queue);
            memset(tp, 0, sizeof *tp);
            return -1;
        }
        tp->nthreads = i + 1;
    }
    return 0;
}

cq_status_t tp_submit(thread_pool_t *tp, int fd)
{
    return cq_push(&tp->queue, fd);
}

size_t tp_pending(thread_pool_t *tp)
{
    return cq_size(&tp->queue);
}

void tp_shutdown(thread_pool_t *tp, tp_reject_fn reject, void *reject_ctx)
{
    if (tp->threads == NULL)
        return;
    cq_close(&tp->queue);
    for (size_t i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL); /* 처리 중이던 연결이 끝나길 기다림 */

    /* 이제 큐를 만지는 스레드는 우리뿐: 남은 fd 를 정리 (fd leak 방지) */
    int fd;
    while (cq_try_pop(&tp->queue, &fd) == 0)
        reject(fd, reject_ctx);

    free(tp->threads);
    tp->threads = NULL;
    tp->nthreads = 0;
    cq_destroy(&tp->queue);
}
