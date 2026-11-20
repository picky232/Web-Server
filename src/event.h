#ifndef EVENT_H
#define EVENT_H

/* kqueue(macOS/BSD) / epoll(Linux) 를 감싸는 얇은 이벤트 루프 추상화.
 * 레벨 트리거 방식. 호출자가 fd 별 현재 관심 mask 를 추적한다. */

#define EV_READ  0x1
#define EV_WRITE 0x2

typedef struct ev_loop ev_loop;

typedef struct {
    void *udata;   /* 등록 시 넘긴 포인터 */
    int   fd;
    int   mask;    /* 발생한 이벤트: EV_READ / EV_WRITE */
    int   eof;     /* 상대가 연결을 닫았거나 오류 */
} ev_event;

ev_loop *ev_create(void);
void     ev_destroy(ev_loop *l);

/* old_mask -> new_mask 로 관심 이벤트를 갱신한다 (0 이면 미등록). */
int ev_update(ev_loop *l, int fd, int old_mask, int new_mask, void *udata);

/* 이벤트를 최대 max 개 가져온다. timeout_ms < 0 이면 무기한 대기.
 * 반환: 이벤트 수, 시그널로 중단되면 0, 오류 시 -1. */
int ev_wait(ev_loop *l, ev_event *out, int max, int timeout_ms);

#endif
