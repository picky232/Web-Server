/* 응답 캐시. 저장 규칙을 일부러 좁게 잡았다: 틀리면 남의 응답이 다른 손님에게 나간다.
   GET 200이고, 백엔드가 max-age/s-maxage로 명시했고, 개인 것일 가능성이 있는 표시가 하나도 없을 때만 저장한다 */
#include "cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SHARDS 16
#define BUCKETS 512

/* 락 하나에 모든 루프가 몰리지 않게 16조각으로 나눈다 */
struct shard {
    pthread_mutex_t lock;
    struct cached *bucket[BUCKETS];
    struct cached *newest, *oldest, *hand;
    long long bytes, max, count;
};
struct cache {
    struct shard s[SHARDS];
    atomic_llong hits, misses;
};

struct cache *cache_new(long long max_bytes)
{
    struct cache *c = calloc(1, sizeof *c);
    for (int i = 0; c && i < SHARDS; i++) {
        pthread_mutex_init(&c->s[i].lock, NULL);
        c->s[i].max = max_bytes / SHARDS;
    }
    return c;
}

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u; /* FNV-1a */
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static long long size_of(const struct cached *o)
{
    return (long long)(sizeof *o + strlen(o->key) + o->head_n + o->body_n);
}

void cached_unref(struct cached *o)
{
    if (o && atomic_fetch_sub(&o->refs, 1) == 1) {
        free(o->key);
        free(o->head);
        free(o->body);
        free(o);
    }
}

static struct cached **find(struct shard *s, unsigned h, const char *key)
{
    struct cached **p = &s->bucket[(h / SHARDS) % BUCKETS];
    while (*p && strcmp((*p)->key, key))
        p = &(*p)->hash_next;
    return p;
}

/* 표와 목록에서 뺀다. 지금 이 응답을 보내는 중인 루프가 있으면 그쪽이 다 쓴 뒤 해제된다 */
static void unlink_one(struct shard *s, struct cached *o)
{
    struct cached **p = find(s, hash(o->key), o->key);
    *p = o->hash_next;
    if (s->hand == o)
        s->hand = o->prev;
    if (o->prev)
        o->prev->next = o->next;
    else
        s->newest = o->next;
    if (o->next)
        o->next->prev = o->prev;
    else
        s->oldest = o->prev;
    s->bytes -= size_of(o);
    s->count--;
    cached_unref(o);
}

struct cached *cache_get(struct cache *c, const char *key, long long now)
{
    unsigned h = hash(key);
    struct shard *s = &c->s[h % SHARDS];
    pthread_mutex_lock(&s->lock);
    struct cached *o = *find(s, h, key);
    if (o && now - o->stored_ms + o->age_ms >= o->fresh_ms) {
        unlink_one(s, o);
        o = NULL;
    }
    if (o) {
        o->visited = true;
        atomic_fetch_add(&o->refs, 1);
    }
    pthread_mutex_unlock(&s->lock);
    atomic_fetch_add(o ? &c->hits : &c->misses, 1);
    return o;
}

/* SIEVE: 오래된 쪽부터 훑으며, 그 사이 쓰인 적 있으면 표시만 지우고 지나가고 아니면 내보낸다 */
static void evict(struct shard *s)
{
    struct cached *o = s->hand ? s->hand : s->oldest;
    while (o && o->visited) {
        o->visited = false;
        o = o->prev ? o->prev : s->oldest;
    }
    if (!o)
        return;
    s->hand = o->prev;
    unlink_one(s, o);
}

void cache_put(struct cache *c, struct cached *o)
{
    unsigned h = hash(o->key);
    struct shard *s = &c->s[h % SHARDS];
    long long size = size_of(o);
    atomic_store(&o->refs, 1);
    pthread_mutex_lock(&s->lock);
    struct cached *old = *find(s, h, o->key);
    if (old)
        unlink_one(s, old);
    while (s->bytes + size > s->max && s->oldest)
        evict(s);
    if (size > s->max) {
        pthread_mutex_unlock(&s->lock);
        cached_unref(o);
        return;
    }
    struct cached **p = find(s, h, o->key);
    o->hash_next = *p;
    *p = o;
    o->next = s->newest;
    if (s->newest)
        s->newest->prev = o;
    s->newest = o;
    if (!s->oldest)
        s->oldest = o;
    s->bytes += size;
    s->count++;
    pthread_mutex_unlock(&s->lock);
}

void cache_remove(struct cache *c, const char *key)
{
    unsigned h = hash(key);
    struct shard *s = &c->s[h % SHARDS];
    pthread_mutex_lock(&s->lock);
    struct cached *o = *find(s, h, key);
    if (o)
        unlink_one(s, o);
    pthread_mutex_unlock(&s->lock);
}

void cache_stats(struct cache *c, long long *hits, long long *misses, long long *bytes, long long *count)
{
    *hits = atomic_load(&c->hits);
    *misses = atomic_load(&c->misses);
    *bytes = *count = 0;
    for (int i = 0; i < SHARDS; i++) {
        pthread_mutex_lock(&c->s[i].lock);
        *bytes += c->s[i].bytes;
        *count += c->s[i].count;
        pthread_mutex_unlock(&c->s[i].lock);
    }
}

void cache_free(struct cache *c)
{
    if (!c)
        return;
    for (int i = 0; i < SHARDS; i++) {
        while (c->s[i].oldest)
            unlink_one(&c->s[i], c->s[i].oldest);
        pthread_mutex_destroy(&c->s[i].lock);
    }
    free(c);
}

/* ---- 판정 ---- */

bool cache_may_lookup(const struct http_req *r)
{
    static const char *const no[] = {"Authorization", "Cookie", "Range"}; /* 손님마다 답이 다를 수 있다 */
    if (r->method != M_GET && r->method != M_HEAD)
        return false;
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        if (fields_find(&r->h, no[i]))
            return false;
    return !fields_token(&r->h, "Cache-Control", "no-store") && !fields_token(&r->h, "Cache-Control", "no-cache") &&
           !fields_token(&r->h, "Cache-Control", "max-age=0") && !fields_token(&r->h, "Pragma", "no-cache");
}

/* Cache-Control에서 "이름=초"를 찾는다. 없으면 -1 */
static long long seconds_of(const struct fields *h, const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < h->n; i++) {
        if (!sl_eqi(h->f[i].name, "Cache-Control"))
            continue;
        struct sl v = h->f[i].value;
        while (v.n) {
            const char *comma = memchr(v.p, ',', v.n);
            size_t len = comma ? (size_t)(comma - v.p) : v.n;
            struct sl one = sl_trim((struct sl){v.p, len});
            long long sec;
            if (one.n > n + 1 && one.p[n] == '=' && sl_eqi((struct sl){one.p, n}, name))
                return sl_to_ll((struct sl){one.p + n + 1, one.n - n - 1}, &sec) ? sec : 0;
            v.p += comma ? len + 1 : len;
            v.n -= comma ? len + 1 : len;
        }
    }
    return -1;
}

long long cache_fresh_ms(const struct http_req *r, const struct http_res *s, long long *age_ms)
{
    if (r->method != M_GET || s->status != 200 || !cache_may_lookup(r))
        return 0;
    static const char *const tokens[] = {"no-store", "no-cache", "private", "must-understand"};
    for (size_t i = 0; i < sizeof tokens / sizeof tokens[0]; i++)
        if (fields_token(&s->h, "Cache-Control", tokens[i]))
            return 0;
    /* Vary가 있으면 요청 헤더마다 다른 답이라는 뜻, Set-Cookie가 있으면 개인 응답일 수 있다 */
    if (fields_find(&s->h, "Set-Cookie") || fields_find(&s->h, "Vary") || fields_find(&s->h, "Pragma"))
        return 0;
    long long sec = seconds_of(&s->h, "s-maxage");
    if (sec < 0)
        sec = seconds_of(&s->h, "max-age");
    long long age = 0;
    const struct field *f = fields_find(&s->h, "Age");
    if (f && !sl_to_ll(f->value, &age))
        return 0;
    if (sec <= age)
        return 0;
    if (sec > 86400)
        sec = 86400; /* 하루를 넘겨 들고 있지 않는다 */
    *age_ms = age * 1000;
    return sec * 1000;
}
