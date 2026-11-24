#ifndef SERVER_H
#define SERVER_H

#include "cache.h"
#include "config.h"
#include "loop.h"

enum phase { PHASE_RUN, PHASE_DRAIN, PHASE_KILL }; /* 평소 → 하던 것만 마치기 → 바로 끝내기 */

/* 서버 프로그램 전체. 시작할 때 채우고 그 뒤로는 atomic만 바뀐다 */
struct server {
    const struct config *cfg;
    struct loop *loops;
    int nloops;
    char via[64]; /* Via 헤더에 쓰는 내 이름 */
    atomic_int loops_alive;
    atomic_int phase; /* enum phase. 메인이 바꾸고 루프가 매 바퀴 읽는다 */
    int log_fd; /* -1 = 끔 */
    bool log_regular; /* 로그가 보통 파일인가 (파이프·터미널이면 막힐 수 있다) */
    struct cache *cache; /* NULL = 끔 */
    long long started_ms;
    atomic_llong spool_bytes; /* 지금 임시 파일에 있는 요청 본문 바이트 */
};

/* 연결이 들어온 문 */
enum origin { ORIGIN_WEB, ORIGIN_ADMIN };

#endif
