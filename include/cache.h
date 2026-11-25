#ifndef CACHE_H
#define CACHE_H

#include "http.h"

#include <stdatomic.h>

/* 저장된 응답 하나. 저장한 뒤에는 내용이 바뀌지 않으므로 여러 루프가 락 없이 같이 읽는다 */
struct cached {
    atomic_int refs;
    char *key;
    char *head; /* 넘길 헤더 줄들 "이름: 값\r\n..." */
    size_t head_n;
    char *body;
    size_t body_n;
    unsigned flags; /* head에 Date·Server가 있는지 (REPLY_HAS_*) */
    char etag[128];
    long long stored_ms; /* 저장한 시각 */
    long long age_ms;    /* 저장할 때 이미 지난 나이 (백엔드의 Age) */
    long long fresh_ms;  /* 신선한 기간 */
    /* 아래는 조각(shard) 락을 잡고만 만진다 */
    struct cached *hash_next;
    struct cached *prev, *next; /* 들어온 순서 */
    bool visited;
};

struct cache;

struct cache *cache_new(long long max_bytes);
void cache_free(struct cache *c);
struct cached *cache_get(struct cache *c, const char *key, long long now); /* 신선한 것만. 다 쓰면 cache_unref */
/* 소유권을 넘긴다. gen은 요청을 백엔드로 보내기 전에 적어 둔 세대 번호.
   그 사이 값을 바꾸는 요청이 지나갔으면 저장하지 않는다 */
void cache_put(struct cache *c, struct cached *o, unsigned gen);
unsigned cache_generation(struct cache *c, const char *key);
void cache_remove(struct cache *c, const char *key);
void cached_unref(struct cached *o);
void cache_stats(struct cache *c, long long *hits, long long *misses, long long *bytes, long long *count);

/* 순수 판정 함수 */
/* cookies: 쿠키가 있는 요청도 허용하는 경로인가 */
bool cache_may_lookup(const struct http_req *r, bool cookies);
long long cache_fresh_ms(const struct http_req *r, const struct http_res *s, bool cookies, long long *age_ms); /* 0 = 저장하지 않음 */

#endif
