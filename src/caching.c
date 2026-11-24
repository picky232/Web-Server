/* 프록시와 캐시를 잇는다: 저장된 응답으로 답하기, 중계하면서 사본 만들기 */
#include "caching.h"
#include "cache.h"
#include "conn.h"
#include "loop.h"
#include "server.h"
#include "static.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_OBJECT_MAX (1 << 20)

bool caching_on(const struct conn *c)
{
    return c->ex->route && c->ex->route->cache && c->loop->srv->cache;
}

/* "호스트 경로?쿼리". 같은 주소라도 호스트가 다르면 다른 응답이다 */
static char *cache_key(const struct exchange *ex)
{
    size_t n = strlen(ex->host) + strlen(ex->path) + ex->target.query.n + 32;
    char *k = malloc(n);
    if (k)
        snprintf(k, n, "%s:%d %s%.*s", ex->host, ex->port, ex->path, (int)ex->target.query.n,
                 ex->target.query.n ? ex->target.query.p : "");
    return k;
}

void caching_drop(struct proxy_st *px)
{
    if (!px->saving)
        return;
    atomic_store(&px->saving->refs, 1);
    cached_unref(px->saving);
    px->saving = NULL;
}

/* 이 응답을 저장해도 되면 사본을 만들기 시작한다 */
void caching_begin(struct conn *c, const struct http_res *res, const struct buf *extra, unsigned flags)
{
    struct proxy_st *px = &c->ex->u.proxy;
    long long age = 0, fresh = caching_on(c) ? cache_fresh_ms(&c->ex->req, res, &age) : 0;
    if (!fresh || res->length > CACHE_OBJECT_MAX)
        return;
    struct cached *o = calloc(1, sizeof *o);
    if (!o)
        return;
    px->saving = o;
    px->saving_cap = 0;
    o->key = cache_key(c->ex);
    o->head = malloc(buf_len(extra) + 1);
    if (!o->key || !o->head) {
        caching_drop(px);
        return;
    }
    /* Age는 꺼낼 때 새로 계산해서 붙이므로 저장하지 않는다 */
    for (const char *p = buf_head(extra), *end = p + buf_len(extra); p < end;) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p)) + 1;
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        struct sl name = {p, (size_t)(colon - p)};
        if (!sl_eqi(name, "Age")) {
            memcpy(o->head + o->head_n, p, (size_t)(eol - p));
            o->head_n += (size_t)(eol - p);
        }
        p = eol;
    }
    const struct field *etag = fields_find(&res->h, "ETag");
    if (etag)
        sl_copy(etag->value, o->etag, sizeof o->etag);
    o->flags = flags;
    o->stored_ms = c->loop->now;
    o->age_ms = age;
    o->fresh_ms = fresh;
}

void caching_add(struct proxy_st *px, struct sl data)
{
    struct cached *o = px->saving;
    if (!o)
        return;
    if (o->body_n + data.n > CACHE_OBJECT_MAX) {
        caching_drop(px);
        return;
    }
    if (o->body_n + data.n > px->saving_cap) {
        size_t cap = (o->body_n + data.n) * 2;
        char *p = realloc(o->body, cap);
        if (!p) {
            caching_drop(px);
            return;
        }
        o->body = p;
        px->saving_cap = cap;
    }
    memcpy(o->body + o->body_n, data.p, data.n);
    o->body_n += data.n;
}

static void serve(struct conn *c, struct cached *o)
{
    struct exchange *ex = c->ex;
    ex->u.cache = (struct cache_st){.o = o};
    ex->h = &cache_handler;
    ex->deadline = 0;
    snprintf(ex->served_by, sizeof ex->served_by, "cache");
    const struct field *inm = fields_find(&ex->req.h, "If-None-Match");
    bool same = inm && o->etag[0] && etag_matches(inm->value, o->etag);
    struct buf extra;
    buf_init(&extra, HTTP_HEAD_MAX * 2);
    bool ok = buf_append(&extra, o->head, o->head_n) &&
              buf_printf(&extra, "Age: %lld\r\nX-Cache: HIT\r\n", (c->loop->now - o->stored_ms + o->age_ms) / 1000) &&
              reply_start(&ex->reply, &c->out, same ? 304 : 200, (long long)o->body_n, buf_head(&extra), buf_len(&extra),
                          o->flags);
    buf_free(&extra);
    if (!ok)
        exchange_abort(c);
}

static bool cache_pump(struct conn *c)
{
    struct cache_st *s = &c->ex->u.cache;
    size_t n = reply_room(&c->ex->reply, &c->out), left = s->o->body_n - s->off;
    if (c->ex->reply.framing == FRAME_NONE)
        left = 0;
    if (left == 0) {
        exchange_finish(c);
        return true;
    }
    if (n > left)
        n = left;
    reply_write(&c->ex->reply, &c->out, s->o->body + s->off, n);
    s->off += n;
    return n > 0;
}

/* 저장해 둔 신선한 응답이 있으면 그것으로 답하고 true */
bool caching_lookup(struct conn *c)
{
    struct proxy_st *px = &c->ex->u.proxy;
    if (!caching_on(c))
        return false;
    struct cache *cache = c->loop->srv->cache;
    char *key = cache_key(c->ex);
    if (!key)
        return false;
    px->cache_gen = cache_generation(cache, key);
    struct cached *o = cache_may_lookup(&c->ex->req) ? cache_get(cache, key, c->loop->now) : NULL;
    free(key);
    if (o)
        serve(c, o);
    return o != NULL;
}

/* 값을 바꾸는 요청이 성공했으면 저장해 둔 것은 낡았다 */
void caching_invalidate(struct conn *c, int status)
{
    enum http_method m = c->ex->req.method;
    if (!caching_on(c) || m == M_GET || m == M_HEAD || m == M_OPTIONS || status >= 400)
        return;
    char *key = cache_key(c->ex);
    if (key)
        cache_remove(c->loop->srv->cache, key);
    free(key);
}

/* 끝까지 온전히 받은 것만 저장한다 */
void caching_commit(struct conn *c)
{
    struct proxy_st *px = &c->ex->u.proxy;
    if (px->saving)
        cache_put(c->loop->srv->cache, px->saving, px->cache_gen);
    px->saving = NULL;
}

static void cache_stop(struct conn *c)
{
    cached_unref(c->ex->u.cache.o);
}

const struct handler cache_handler = {.name = "cache", .pump = cache_pump, .stop = cache_stop};
