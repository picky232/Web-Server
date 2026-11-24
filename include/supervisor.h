#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "config.h"

#include <stdbool.h>

/* 감독 프로세스(-s): listen 소켓을 쥐고 일하는 프로세스(워커)를 띄운다.
   SIGHUP → 새 설정으로 새 워커를 띄우고, 준비되면 옛 워커를 내보낸다. listen 소켓은 감독이 계속 쥐고 있어
   그 사이에 온 연결도 잃지 않는다. 워커가 죽으면 다시 띄운다 */
int supervisor_run(struct config *cfg, char **argv);

/* 워커 쪽: 감독이 넘겨준 fd들. 감독 없이 실행됐으면 n = 0 */
struct inherited {
    int n;      /* listen 소켓 수 */
    int fd[3];  /* web, admin, forward 순서 */
    int alive;  /* 감독이 죽으면 EOF가 되는 파이프. 없으면 -1 */
    int ready;  /* 준비가 끝나면 한 바이트를 쓰는 파이프. 없으면 -1 */
};
struct inherited supervisor_inherit(void);

#endif
