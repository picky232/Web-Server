#ifndef SERVER_H
#define SERVER_H

#include "loop.h"

/* 서버 프로그램 전체. 시작할 때 채우고 그 뒤로는 atomic만 바뀐다 */
struct server {
    struct loop *loops;
    int nloops;
    atomic_bool draining;
};

/* 연결이 들어온 문 */
enum origin { ORIGIN_WEB, ORIGIN_ADMIN, ORIGIN_FORWARD };

#endif
