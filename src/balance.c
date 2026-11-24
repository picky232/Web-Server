#include "balance.h"

bool backend_usable(const struct backend *b, long long now)
{
    return !atomic_load(&b->down) && atomic_load(&b->ejected_until) <= now;
}

struct backend *pool_pick(struct pool *p, unsigned tried, long long now)
{
    (void)now;
    for (int i = 0; i < p->n; i++)
        if (!(tried & (1u << i))) {
            atomic_fetch_add(&p->be[i].inflight, 1);
            return &p->be[i];
        }
    return NULL;
}

void pool_done(struct pool *p, struct backend *b, enum outcome o, long long now)
{
    (void)p;
    (void)o;
    (void)now;
    atomic_fetch_sub(&b->inflight, 1);
}
