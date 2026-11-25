#ifndef HEALTH_H
#define HEALTH_H

#include "config.h"
#include "ev.h"

/* 능동 헬스 체크: 손님이 없어도 메인 스레드가 주기적으로 백엔드를 살핀다.
   전부 안 기다리는 소켓이라, 응답 없는 백엔드가 있어도 accept가 멈추지 않는다 */
int health_start(struct config *cfg, int evfd);
void health_tick(long long now);      /* 검사를 시작하고, 시간 안에 답이 없는 것을 실패로 친다 */
void health_event(struct io *io, unsigned ready, long long now);
void health_stop(void);

#endif
