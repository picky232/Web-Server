#ifndef CRASH_H
#define CRASH_H

#include <signal.h>

struct server;

/* 갑자기 죽을 때(잘못된 메모리 접근 등) 스레드별로 처리하던 요청을 표준 오류에 남긴다 */
void crash_install(struct server *s);
void *crash_thread_begin(void);    /* 스레드마다 한 번: 스택이 넘쳐 죽어도 기록할 수 있게 따로 쓸 스택 */
void crash_thread_end(void *stack);
void crash_unblock(sigset_t *set); /* 시그널 마스크에서 죽음 시그널을 뺀다 */

#endif
