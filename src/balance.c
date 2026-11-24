/* 백엔드 고르기와 장애 판단. 소켓을 모르고 시각도 인자로 받으므로 그대로 단위 시험할 수 있다 */
#include "balance.h"

#define BAD_LIMIT 5        /* 실패 응답이 이만큼 이어지면 제외 */
#define EJECT_MS 10000
#define TIMEOUT_LIMIT 3    /* 연결 시간 초과가 이만큼 이어지면 제외 */

bool backend_usable(const struct backend *b, long long now)
{
    return !atomic_load(&b->down) && atomic_load(&b->ejected_until) <= now;
}

struct backend *pool_pick(struct pool *p, unsigned tried, long long now)
{
    unsigned start = atomic_fetch_add(&p->next, 1);
    /* 1차: 건강한 것 중에서. 2차: 건강한 것이 하나도 없으면 그래도 시도해 본다
       (헬스 체크 설정 실수 하나로 전체가 503이 되는 것을 막는다) */
    for (int pass = 0; pass < 2; pass++) {
        bool any;
        for (;;) {
            struct backend *best = NULL;
            int best_load = 0;
            any = false;
            for (int k = 0; k < p->n; k++) {
                int i = (int)((start + (unsigned)k) % (unsigned)p->n);
                struct backend *b = &p->be[i];
                if ((tried & (1u << i)) || (pass == 0 && !backend_usable(b, now)))
                    continue;
                any = true;
                int load = atomic_load(&b->inflight);
                if (load >= p->max_inflight || (best && load >= best_load))
                    continue;
                best = b;
                best_load = load;
                if (p->policy == ROUND_ROBIN)
                    break;
            }
            if (!best)
                break;
            if (atomic_fetch_add(&best->inflight, 1) < p->max_inflight)
                return best;
            atomic_fetch_sub(&best->inflight, 1); /* 그 사이 다른 루프가 마지막 자리를 가져갔다 */
        }
        if (any)
            return NULL; /* 건강한 백엔드가 있지만 모두 한도까지 찼다 */
    }
    return NULL;
}

/* 하나를 더 빼도 절반은 남는가. 연쇄로 전부 빠지는 것을 막는다 */
static bool may_exclude(const struct pool *p, const struct backend *b, long long now)
{
    int out = 0;
    for (int i = 0; i < p->n; i++)
        out += &p->be[i] != b && !backend_usable(&p->be[i], now);
    return out + 1 <= p->n / 2;
}

const char *pool_done(struct pool *p, struct backend *b, enum outcome o, long long now)
{
    atomic_fetch_sub(&b->inflight, 1);
    switch (o) {
    case OUT_OK:
        atomic_store(&b->bad, 0);
        atomic_store(&b->timeouts, 0);
        return NULL;
    case OUT_NEUTRAL:
        return NULL;
    case OUT_REFUSED: /* 거절은 확실한 신호다. 바로 빼고, 헬스 체크가 되돌린다 */
        atomic_fetch_add(&b->failures, 1);
        return atomic_exchange(&b->down, true) ? NULL : "연결 거절";
    case OUT_CONNECT_TIMEOUT: /* 바쁠 때도 나올 수 있으므로 이어질 때만 */
        atomic_fetch_add(&b->failures, 1);
        if (atomic_fetch_add(&b->timeouts, 1) + 1 >= TIMEOUT_LIMIT && may_exclude(p, b, now) &&
            !atomic_exchange(&b->down, true))
            return "연결 시간 초과 3번 연속";
        return NULL;
    case OUT_BAD:
        atomic_fetch_add(&b->failures, 1);
        if (atomic_fetch_add(&b->bad, 1) + 1 >= BAD_LIMIT && may_exclude(p, b, now)) {
            atomic_store(&b->bad, 0);
            atomic_store(&b->ejected_until, now + EJECT_MS);
            return "실패 응답 5번 연속";
        }
        return NULL;
    }
    return NULL;
}
