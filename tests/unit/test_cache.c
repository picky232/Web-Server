#include "cache.h"
#include "test.h"

#include <stdlib.h>

static bool req(const char *s, struct http_req *r)
{
    char b[512];
    snprintf(b, sizeof b, "GET / HTTP/1.1\r\nHost: x\r\n%s\r\n", s);
    return http_parse_request(b, strlen(b), r) == 0 && cache_may_lookup(r);
}

static long long fresh(const char *method, const char *status, const char *headers)
{
    static struct http_req r;
    static struct http_res s;
    static char a[256], b[512];
    long long age = 0;
    snprintf(a, sizeof a, "%s / HTTP/1.1\r\nHost: x\r\n\r\n", method);
    snprintf(b, sizeof b, "HTTP/1.1 %s OK\r\n%s\r\n", status, headers);
    if (http_parse_request(a, strlen(a), &r) || !http_parse_response(b, strlen(b), &s))
        return -1;
    return cache_fresh_ms(&r, &s, &age);
}

static struct cached *obj(const char *key, size_t body, long long now, long long fresh_ms)
{
    struct cached *o = calloc(1, sizeof *o);
    o->key = malloc(strlen(key) + 1);
    strcpy(o->key, key);
    o->body = calloc(1, body + 1);
    o->body_n = body;
    o->stored_ms = now;
    o->fresh_ms = fresh_ms;
    return o;
}

static bool has(struct cache *c, const char *key, long long now)
{
    struct cached *o = cache_get(c, key, now);
    cached_unref(o);
    return o != NULL;
}

void test_cache(void)
{
    struct http_req *r = malloc(sizeof *r);
    CHECK(req("", r));
    const char *no[] = {"Authorization: x\r\n", "Cookie: a\r\n", "Range: bytes=0-1\r\n", "Cache-Control: no-cache\r\n",
                        "Cache-Control: no-store\r\n", "Cache-Control: max-age=0\r\n", "Pragma: no-cache\r\n"};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        CHECK_MSG(!req(no[i], r), no[i]);
    free(r);

    CHECK(fresh("GET", "200", "Cache-Control: max-age=60\r\n") == 60000);
    CHECK(fresh("GET", "200", "Cache-Control: public, max-age=60, s-maxage=10\r\n") == 10000);
    CHECK(fresh("GET", "200", "Cache-Control: max-age=60\r\nAge: 20\r\n") == 60000);
    CHECK(fresh("GET", "200", "Cache-Control: max-age=999999999\r\n") == 86400000);
    const char *never[] = {"", "Cache-Control: max-age=0\r\n", "Cache-Control: max-age=x\r\n", "Cache-Control: no-store, max-age=9\r\n",
                           "Cache-Control: private, max-age=9\r\n", "Cache-Control: no-cache, max-age=9\r\n",
                           "Cache-Control: max-age=9\r\nSet-Cookie: a=b\r\n", "Cache-Control: max-age=9\r\nVary: Accept\r\n",
                           "Cache-Control: max-age=9\r\nAge: 9\r\n", "Cache-Control: max-age=9\r\nAge: x\r\n",
                           "Cache-Control: max-age=-5\r\n", "Expires: Tue, 24 Nov 2099 00:00:00 GMT\r\n"};
    for (size_t i = 0; i < sizeof never / sizeof never[0]; i++)
        CHECK_MSG(fresh("GET", "200", never[i]) == 0, never[i]);
    CHECK(fresh("POST", "200", "Cache-Control: max-age=9\r\n") == 0);
    CHECK(fresh("HEAD", "200", "Cache-Control: max-age=9\r\n") == 0);
    CHECK(fresh("GET", "404", "Cache-Control: max-age=9\r\n") == 0);

    struct cache *c = cache_new(16 * 10000); /* 조각 하나에 10000바이트 */
    cache_put(c, obj("a", 100, 0, 1000));
    CHECK(has(c, "a", 999) && !has(c, "b", 0) && !has(c, "a", 1000)); /* 1000에서 만료 */
    CHECK(!has(c, "a", 0)); /* 만료된 것은 없어진다 */
    cache_put(c, obj("a", 100, 0, 1000));
    cache_put(c, obj("a", 200, 0, 1000)); /* 덮어쓰기 */
    struct cached *o = cache_get(c, "a", 1);
    CHECK(o && o->body_n == 200);
    cache_remove(c, "a"); /* 보내는 중인 응답은 지워져도 끝까지 쓸 수 있다 */
    CHECK(!has(c, "a", 1) && o->body_n == 200 && o->body[199] == 0);
    cached_unref(o);
    cache_put(c, obj("huge", 20000, 0, 1000)); /* 조각보다 크면 저장하지 않는다 */
    CHECK(!has(c, "huge", 1));
    long long hits, misses, bytes, count;
    char key[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        cache_put(c, obj(key, 1000, 0, 100000));
        CHECK(has(c, "k0", 1) || i > 0); /* 계속 쓰이는 것은 */
        (void)has(c, "k0", 1);
    }
    cache_stats(c, &hits, &misses, &bytes, &count);
    CHECK(bytes <= 16 * 10000 && count > 50); /* 상한을 지킨다 */
    CHECK(has(c, "k0", 1));                   /* 쓰이는 것은 남아 있다 */
    CHECK(!has(c, "k1", 1));
    cache_free(c);
}
