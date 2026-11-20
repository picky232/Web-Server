/*
 * thread_pool.h — 고정 개수 worker 스레드 + 크기 제한 연결 큐.
 *
 *   accept 루프(main 스레드)  --cq_push(fd)-->  [ 원형 큐 ]  --cq_pop()-->  worker N개
 *
 * 공유 상태는 conn_queue_t 하나뿐이고, 그 안의 모든 필드는 mutex 로 보호된다.
 * - 큐가 가득 차면 push 는 기다리지 않고 CQ_FULL 을 돌려준다
 *   (accept 루프가 막히지 않게 하기 위해 → 호출자가 503 으로 거절).
 * - 큐가 비어 있으면 pop 은 condition variable 에서 잠든다.
 * - close 되면 잠든 worker 를 모두 깨우고, pop 은 -1 을 돌려 worker 가 끝난다.
 */
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { CQ_OK, CQ_FULL, CQ_CLOSED } cq_status_t;

typedef struct {
    int *items;          /* 원형 버퍼에 담긴 연결 소켓 fd 들 */
    size_t capacity;
    size_t head;         /* 다음에 꺼낼 위치 */
    size_t count;        /* 현재 들어 있는 개수 */
    bool closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
} conn_queue_t;

int cq_init(conn_queue_t *q, size_t capacity);
void cq_destroy(conn_queue_t *q);            /* 남은 fd 는 닫지 않는다 */
cq_status_t cq_push(conn_queue_t *q, int fd); /* 절대 블록하지 않음 */
int cq_pop(conn_queue_t *q, int *fd_out);     /* 0: fd 꺼냄, -1: 큐가 닫힘 */
int cq_try_pop(conn_queue_t *q, int *fd_out); /* 블록하지 않음. 0: 꺼냄, -1: 비었음 */
size_t cq_size(conn_queue_t *q);
void cq_close(conn_queue_t *q);

/* worker 가 fd 하나를 처리하는 함수. fd 를 닫는 책임은 handler 에게 있다. */
typedef void (*tp_handler_fn)(int fd, void *ctx);
/* 처리하지 못한 fd 를 정리하는 함수 (큐 가득 참 / 종료 시 남은 fd) */
typedef void (*tp_reject_fn)(int fd, void *ctx);

typedef struct {
    conn_queue_t queue;
    pthread_t *threads;
    size_t nthreads;
    tp_handler_fn handler;
    void *ctx;
} thread_pool_t;

int tp_init(thread_pool_t *tp, size_t nthreads, size_t queue_capacity,
            tp_handler_fn handler, void *ctx);
cq_status_t tp_submit(thread_pool_t *tp, int fd);
size_t tp_pending(thread_pool_t *tp);
/* 큐를 닫고, 모든 worker 가 현재 연결을 마칠 때까지 join 한 다음,
 * 아직 시작하지 못한 fd 들은 reject 로 넘기고 자원을 해제한다. */
void tp_shutdown(thread_pool_t *tp, tp_reject_fn reject, void *reject_ctx);

/* 현재 스레드의 worker 번호 (1부터). worker 가 아니면 0 (main 스레드). */
int tp_worker_id(void);

#endif /* THREAD_POOL_H */
