#ifndef BALANCE_H
#define BALANCE_H

#include "net.h"

#include <stdatomic.h>

#define POOL_MAX_BACKENDS 32

/* 백엔드 하나. 루프 스레드들이 같이 쓰므로 바뀌는 값은 전부 atomic */
struct backend {
    char name[64]; /* "127.0.0.1:8001" */
    struct addr addr;
    int index;             /* 서버 전체에서의 번호 (루프별 연결 보관함의 칸) */
    atomic_int inflight;   /* 지금 맡긴 요청 수 */
    atomic_bool down;      /* 연결이 안 되는 상태. 헬스 체크가 되돌린다 */
    atomic_llong ejected_until; /* 연결은 되지만 응답이 계속 실패: 이 시각까지 제외 */
    atomic_int bad;        /* 연속으로 502/503을 주거나 응답 전에 끊은 횟수 */
    atomic_int timeouts;   /* 연속 연결 시간 초과 횟수 */
    /* 상태 페이지용 */
    atomic_llong requests, failures, reused;
};

enum policy { LEAST_CONN, ROUND_ROBIN };

struct pool {
    char name[32];
    enum policy policy;
    int max_inflight;   /* 백엔드 하나에 동시에 맡기는 한도 */
    char health[128];   /* 빈 문자열 = 연결만 확인, "/path" = HTTP로 확인 */
    struct backend be[POOL_MAX_BACKENDS];
    int n;
    atomic_uint next;   /* 동점일 때 돌아가며 고르기 */
};

/* 요청 하나가 백엔드에서 어떻게 끝났나 */
enum outcome {
    OUT_OK,
    OUT_BAD,       /* 502·503 응답, 또는 응답 전에 끊김 */
    OUT_REFUSED,   /* 연결 거절 */
    OUT_CONNECT_TIMEOUT,
    OUT_NEUTRAL,   /* 백엔드 탓이 아니거나 판단할 수 없음: 우리 쪽 자원 부족, 응답 시간 초과, 클라이언트가 떠남 */
};

/* 백엔드를 하나 골라 자리를 예약한다. tried: 이번 요청에서 이미 써 본 백엔드 비트.
   모두 한도까지 찼으면 NULL */
struct backend *pool_pick(struct pool *p, unsigned tried, long long now);
/* 자리를 돌려주고 결과를 기록한다. 이번 결과로 백엔드가 제외됐으면 그 이유, 아니면 NULL */
const char *pool_done(struct pool *p, struct backend *b, enum outcome o, long long now);

bool backend_usable(const struct backend *b, long long now);

#endif
