#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* 종료 시그널 수신 시 0 으로 반환. 실패 시 -1.
 * cfg 는 실행 중 갱신된다 (upstream 의 round-robin 위치, 장애 표시). */
int server_run(app_config *cfg);

/* 시그널 핸들러에서 호출: 이벤트 루프를 종료시킨다. */
void server_request_stop(void);

#endif
