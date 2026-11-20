#include "unity.h"
#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hp_parser p;
static char rb[4096];
static pm_resp r;
static char sb[4096];

void setUp(void)    {}
void tearDown(void) {}

static void req(const char *raw) {
    snprintf(rb, sizeof rb, "%s", raw);
    hp_init(&p, 16384, 8192, 1 << 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HP_COMPLETE, hp_feed(&p, rb, strlen(rb)), raw);
}

static int resp(const char *raw, long default_ttl, cache_resp_info *info) {
    snprintf(sb, sizeof sb, "%s", raw);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, pm_resp_parse(sb, strlen(sb), &r), raw);
    return cache_response_policy(&r, sb, default_ttl, info);
}

#define HDR(extra) "GET /a?x=1 HTTP/1.1\r\nHost: Example.COM\r\n" extra "\r\n"

/* -------------------------------- 키 -------------------------------- */

void test_key_lowercases_host_and_keeps_query(void) {
    req(HDR(""));
    char k[256];
    int n = cache_make_key(&p, rb, k, sizeof k);
    TEST_ASSERT_EQUAL_INT((int)strlen("example.com\n/a?x=1"), n);
    TEST_ASSERT_EQUAL_STRING("example.com\n/a?x=1", k);
}

void test_key_absolute_form_uses_origin_form(void) {
    req("GET http://example.com/a?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    char k[256];
    cache_make_key(&p, rb, k, sizeof k);
    TEST_ASSERT_EQUAL_STRING("example.com\n/a?x=1", k);
}

void test_key_differs_by_query_and_host(void) {
    char k1[256], k2[256], k3[256];
    req("GET /a?x=1 HTTP/1.1\r\nHost: h\r\n\r\n");  cache_make_key(&p, rb, k1, sizeof k1);
    req("GET /a?x=2 HTTP/1.1\r\nHost: h\r\n\r\n");  cache_make_key(&p, rb, k2, sizeof k2);
    req("GET /a?x=1 HTTP/1.1\r\nHost: g\r\n\r\n");  cache_make_key(&p, rb, k3, sizeof k3);
    TEST_ASSERT_TRUE(strcmp(k1, k2) != 0);
    TEST_ASSERT_TRUE(strcmp(k1, k3) != 0);
}

void test_key_buffer_too_small(void) {
    req(HDR(""));
    char k[8];
    TEST_ASSERT_EQUAL_INT(-1, cache_make_key(&p, rb, k, sizeof k));
}

/* ---------------------------- 요청 정책 ---------------------------- */

static void req_policy(const char *extra, int want_lookup, int want_store) {
    char raw[512];
    snprintf(raw, sizeof raw, "GET /a HTTP/1.1\r\nHost: h\r\n%s\r\n", extra);
    req(raw);
    int lk = -1, st = -1;
    cache_request_policy(&p, rb, &lk, &st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want_lookup, lk, extra);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want_store, st, extra);
}

void test_request_policy(void) {
    req_policy("", 1, 1);
    req_policy("Authorization: Bearer x\r\n", 0, 0);       /* 사용자별 응답일 수 있음 */
    req_policy("Range: bytes=0-10\r\n", 0, 0);
    req_policy("Cache-Control: no-store\r\n", 0, 0);
    req_policy("Cache-Control: no-cache\r\n", 0, 1);        /* 새로 받아 오되 갱신은 허용 */
    req_policy("Cache-Control: max-age=0\r\n", 0, 1);
    req_policy("Cache-Control: max-age=60\r\n", 1, 1);
    req_policy("Cache-Control: NO-CACHE\r\n", 0, 1);        /* 대소문자 무시 */
    req_policy("Pragma: no-cache\r\n", 0, 1);
    req_policy("Cache-Control: max-age=60, no-cache\r\n", 0, 1);
}

/* ---------------------------- 응답 정책 ---------------------------- */

/* headers 는 줄 사이에만 \r\n 을 넣고 끝에는 넣지 않는다. 마지막 헤더 뒤에 \r\n\r\n (빈 줄)을 붙여 헤더를 끝낸다. */
#define RESP(status, headers) "HTTP/1.1 " status "\r\nContent-Length: 1\r\n" headers "\r\n\r\n"

void test_response_max_age(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=60"), 0, &i));
    TEST_ASSERT_EQUAL_INT(60, i.ttl_s);
    TEST_ASSERT_EQUAL_STRING("", i.vary);
}

void test_response_smaxage_beats_max_age(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=10, s-maxage=99"), 0, &i));
    TEST_ASSERT_EQUAL_INT(99, i.ttl_s);
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=0, s-maxage=30"), 0, &i));
    TEST_ASSERT_EQUAL_INT(30, i.ttl_s);   /* 공유 캐시 전용 지시가 우선 */
}

void test_response_directives_case_and_quotes(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "CACHE-CONTROL: Public, Max-Age=\"45\""), 0, &i));
    TEST_ASSERT_EQUAL_INT(45, i.ttl_s);
}

void test_response_multiple_cache_control_headers(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: public\r\nCache-Control: max-age=20"), 0, &i));
    TEST_ASSERT_EQUAL_INT(20, i.ttl_s);
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=20\r\nCache-Control: no-store"), 0, &i));
}

void test_response_default_ttl(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", ""), 0, &i));         /* 기본 TTL 0: 명시적 신선도만 */
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", ""), 10, &i));
    TEST_ASSERT_EQUAL_INT(10, i.ttl_s);
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("301 Moved", "Location: /x"), 10, &i));
}

void test_response_malformed_max_age_falls_back_to_default(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=abc"), 7, &i));
    TEST_ASSERT_EQUAL_INT(7, i.ttl_s);
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=abc"), 0, &i));
}

void test_response_status_rules(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("404 Not Found", ""), 10, &i));                       /* 부정 응답은 명시적 신선도 필요 */
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("404 Not Found", "Cache-Control: max-age=30"), 0, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("500 Error", "Cache-Control: max-age=30"), 0, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("206 Partial", "Cache-Control: max-age=30"), 0, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("302 Found", "Cache-Control: max-age=30"), 0, &i));
}

void test_response_not_cacheable_directives(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: no-store"), 10, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: no-cache"), 10, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: private, max-age=60"), 10, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=0"), 10, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nSet-Cookie: sid=1"), 10, &i));
}

void test_response_expires_only_is_not_cached(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Expires: Thu, 01 Jan 2099 00:00:00 GMT"), 10, &i));
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Expires: Thu, 01 Jan 2099 00:00:00 GMT\r\nCache-Control: max-age=5"), 10, &i));
}

void test_response_vary(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nVary: Accept-Language, ACCEPT-Encoding"), 0, &i));
    TEST_ASSERT_EQUAL_STRING("accept-language,accept-encoding", i.vary);
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nVary: A\r\nVary: B"), 0, &i));
    TEST_ASSERT_EQUAL_STRING("a,b", i.vary);
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nVary: *"), 0, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nVary: Accept, *"), 0, &i));
}

void test_response_backend_age_is_subtracted(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nAge: 50"), 0, &i));
    TEST_ASSERT_EQUAL_INT(10, i.ttl_s);
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nAge: 60"), 0, &i));
    TEST_ASSERT_EQUAL_INT(0, resp(RESP("200 OK", "Cache-Control: max-age=60\r\nAge: 99"), 0, &i));
}

void test_response_ttl_is_capped(void) {
    cache_resp_info i;
    TEST_ASSERT_EQUAL_INT(1, resp(RESP("200 OK", "Cache-Control: max-age=99999999999"), 0, &i));
    TEST_ASSERT_EQUAL_INT(86400, i.ttl_s);
}

/* ------------------------------ Vary 키 ------------------------------ */

void test_vary_key(void) {
    char k[256];
    req("GET / HTTP/1.1\r\nHost: h\r\nAccept-Language: en\r\n\r\n");
    TEST_ASSERT_EQUAL_INT((int)strlen("accept-language:en\n"), cache_vary_key(&p, rb, "accept-language", k, sizeof k));
    TEST_ASSERT_EQUAL_STRING("accept-language:en\n", k);
    cache_vary_key(&p, rb, "accept-language,accept-encoding", k, sizeof k);
    TEST_ASSERT_EQUAL_STRING("accept-language:en\naccept-encoding:\n", k);   /* 없는 헤더는 빈 값 */
    TEST_ASSERT_EQUAL_INT(-1, cache_vary_key(&p, rb, "accept-language", k, 4));
}

/* ------------------------- 저장 / 조회 / 만료 ------------------------- */

static const char *HEAD = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";

static cache_resp_info info(long ttl, const char *vary) {
    cache_resp_info i;
    i.ttl_s = ttl;
    snprintf(i.vary, sizeof i.vary, "%s", vary);
    return i;
}

static int lookup(cache *c, const char *key, long long now, cache_hit *h) {
    req("GET /a HTTP/1.1\r\nHost: h\r\n\r\n");
    return cache_lookup(c, key, strlen(key), &p, rb, now, h);
}

static int store(cache *c, const char *key, const char *body, long ttl, long long now) {
    cache_resp_info i = info(ttl, "");
    return cache_store(c, key, strlen(key), &i, "", HEAD, strlen(HEAD), body, strlen(body), now);
}

void test_store_and_hit(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    TEST_ASSERT_EQUAL_INT(1, store(c, "k1", "hello", 60, 1000));
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "k1", 1000, &h));
    TEST_ASSERT_EQUAL_STRING_LEN(HEAD, h.head, h.head_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", h.body, h.body_len);
    TEST_ASSERT_EQUAL_INT(0, h.age_s);
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "k1", 4999, &h));
    TEST_ASSERT_EQUAL_INT(3, h.age_s);   /* 3.999초 -> 3 */
    cache_destroy(c);
}

void test_miss_on_unknown_key(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(0, lookup(c, "nope", 0, &h));
    cache_destroy(c);
}

void test_expiry_is_exact_and_counted(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    store(c, "k", "x", 2, 10000);                       /* 만료: 12000ms */
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "k", 11999, &h));
    TEST_ASSERT_EQUAL_INT(0, lookup(c, "k", 12000, &h));
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(1, s.expired);
    TEST_ASSERT_EQUAL_UINT(0, s.entries);               /* 만료 항목은 제거되어 메모리를 돌려준다 */
    TEST_ASSERT_EQUAL_UINT(0, s.bytes);
    cache_destroy(c);
}

void test_same_key_replaces_entry(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    store(c, "k", "old", 60, 0);
    store(c, "k", "newer", 60, 0);
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "k", 1, &h));
    TEST_ASSERT_EQUAL_STRING_LEN("newer", h.body, h.body_len);
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(1, s.entries);
    cache_destroy(c);
}

void test_invalidate(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    store(c, "k", "x", 60, 0);
    cache_invalidate(c, "k", 1);
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(0, lookup(c, "k", 1, &h));
    cache_invalidate(c, "missing", 7);                  /* 없는 키는 무시 */
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(1, s.invalidations);
    TEST_ASSERT_EQUAL_UINT(0, s.bytes);
    cache_destroy(c);
}

void test_empty_body_and_binary_body(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    cache_resp_info i = info(60, "");
    cache_hit h;
    TEST_ASSERT_EQUAL_INT(1, cache_store(c, "e", 1, &i, "", HEAD, strlen(HEAD), "", 0, 0));
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "e", 1, &h));
    TEST_ASSERT_EQUAL_UINT(0, h.body_len);

    const char bin[] = { 'a', 0, 'b', (char)0xff, 'c' };           /* NUL 을 포함한 본문 */
    TEST_ASSERT_EQUAL_INT(1, cache_store(c, "b", 1, &i, "", HEAD, strlen(HEAD), bin, sizeof bin, 0));
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "b", 1, &h));
    TEST_ASSERT_EQUAL_UINT(sizeof bin, h.body_len);
    TEST_ASSERT_EQUAL_MEMORY(bin, h.body, sizeof bin);
    cache_destroy(c);
}

void test_key_with_nul_safe_length(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    cache_resp_info i = info(60, "");
    cache_store(c, "ab\0c", 4, &i, "", HEAD, strlen(HEAD), "1", 1, 0);
    cache_hit h;
    req("GET /a HTTP/1.1\r\nHost: h\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(1, cache_lookup(c, "ab\0c", 4, &p, rb, 1, &h));
    TEST_ASSERT_EQUAL_INT(0, cache_lookup(c, "ab\0d", 4, &p, rb, 1, &h));   /* 길이 기반 비교 */
    TEST_ASSERT_EQUAL_INT(0, cache_lookup(c, "ab", 2, &p, rb, 1, &h));
    cache_destroy(c);
}

/* -------------------------------- Vary -------------------------------- */

void test_vary_variants(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    req("GET /a HTTP/1.1\r\nHost: h\r\nAccept-Language: en\r\n\r\n");
    char vk[128];
    cache_vary_key(&p, rb, "accept-language", vk, sizeof vk);
    cache_resp_info i = info(60, "accept-language");
    TEST_ASSERT_EQUAL_INT(1, cache_store(c, "k", 1, &i, vk, HEAD, strlen(HEAD), "english", 7, 0));

    cache_hit h;
    TEST_ASSERT_EQUAL_INT(1, cache_lookup(c, "k", 1, &p, rb, 1, &h));        /* 같은 언어: 적중 */
    TEST_ASSERT_EQUAL_STRING_LEN("english", h.body, h.body_len);

    req("GET /a HTTP/1.1\r\nHost: h\r\nAccept-Language: fr\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(0, cache_lookup(c, "k", 1, &p, rb, 1, &h));        /* 다른 언어: 영어 응답을 주면 안 됨 */
    req("GET /a HTTP/1.1\r\nHost: h\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(0, cache_lookup(c, "k", 1, &p, rb, 1, &h));        /* 헤더 없음도 다른 변형 */
    cache_destroy(c);
}

/* --------------------------- 크기 제한 / LRU --------------------------- */

void test_object_larger_than_max_object_not_stored(void) {
    cache *c = cache_create(1 << 20, 100);
    char big[101];
    memset(big, 'x', sizeof big);
    cache_resp_info i = info(60, "");
    TEST_ASSERT_EQUAL_INT(0, cache_store(c, "k", 1, &i, "", HEAD, strlen(HEAD), big, 101, 0));
    TEST_ASSERT_EQUAL_INT(1, cache_store(c, "k", 1, &i, "", HEAD, strlen(HEAD), big, 100, 0));   /* 경계값은 허용 */
    cache_destroy(c);
}

void test_object_larger_than_total_budget_not_stored(void) {
    cache *c = cache_create(500, 1 << 20);
    char big[600];
    memset(big, 'x', sizeof big);
    cache_resp_info i = info(60, "");
    TEST_ASSERT_EQUAL_INT(0, cache_store(c, "k", 1, &i, "", HEAD, strlen(HEAD), big, sizeof big, 0));
    cache_destroy(c);
}

void test_lru_evicts_least_recently_used(void) {
    cache *c = cache_create(3800, 1 << 16);
    char body[1000];
    memset(body, 'x', sizeof body);
    cache_resp_info i = info(600, "");
    cache_hit h;
    cache_store(c, "A", 1, &i, "", HEAD, strlen(HEAD), body, sizeof body, 0);
    cache_store(c, "B", 1, &i, "", HEAD, strlen(HEAD), body, sizeof body, 0);
    cache_store(c, "C", 1, &i, "", HEAD, strlen(HEAD), body, sizeof body, 0);
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(3, s.entries);

    TEST_ASSERT_EQUAL_INT(1, lookup(c, "A", 1, &h));                 /* A 를 최근에 사용: LRU 는 B */
    cache_store(c, "D", 1, &i, "", HEAD, strlen(HEAD), body, sizeof body, 0);

    TEST_ASSERT_EQUAL_INT(1, lookup(c, "A", 2, &h));
    TEST_ASSERT_EQUAL_INT(0, lookup(c, "B", 2, &h));                 /* 쫓겨남 */
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "C", 2, &h));
    TEST_ASSERT_EQUAL_INT(1, lookup(c, "D", 2, &h));
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(1, s.evictions);
    TEST_ASSERT_TRUE(s.bytes <= 3800);
    cache_destroy(c);
}

void test_memory_never_exceeds_budget(void) {
    const size_t budget = 20000;
    cache *c = cache_create(budget, 4096);
    cache_resp_info i = info(600, "");
    srand(12345);
    for (int n = 0; n < 2000; n++) {
        char key[32], body[4096];
        int klen = snprintf(key, sizeof key, "key-%d", rand() % 300);
        size_t bl = (size_t)(rand() % 3000);
        memset(body, 'a' + n % 26, bl);
        cache_store(c, key, (size_t)klen, &i, "", HEAD, strlen(HEAD), body, bl, n);
        cache_stats s;
        cache_get_stats(c, &s);
        TEST_ASSERT_TRUE_MESSAGE(s.bytes <= budget, "메모리 상한 초과");
    }
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_TRUE(s.evictions > 0);
    cache_destroy(c);
}

void test_bytes_accounting_returns_to_zero(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    char key[16];
    for (int n = 0; n < 50; n++) { snprintf(key, sizeof key, "k%d", n); store(c, key, "payload", 60, 0); }
    for (int n = 0; n < 50; n++) { snprintf(key, sizeof key, "k%d", n); cache_invalidate(c, key, strlen(key)); }
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(0, s.entries);
    TEST_ASSERT_EQUAL_UINT(0, s.bytes);
    cache_destroy(c);
}

void test_hit_miss_counters(void) {
    cache *c = cache_create(1 << 20, 1 << 16);
    store(c, "k", "x", 60, 0);
    cache_hit h;
    lookup(c, "k", 1, &h); lookup(c, "k", 1, &h); lookup(c, "z", 1, &h);
    cache_stats s;
    cache_get_stats(c, &s);
    TEST_ASSERT_EQUAL_UINT(2, s.hits);
    TEST_ASSERT_EQUAL_UINT(1, s.misses);
    TEST_ASSERT_EQUAL_UINT(1, s.stores);
    cache_destroy(c);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_key_lowercases_host_and_keeps_query);
    RUN_TEST(test_key_absolute_form_uses_origin_form);
    RUN_TEST(test_key_differs_by_query_and_host);
    RUN_TEST(test_key_buffer_too_small);
    RUN_TEST(test_request_policy);
    RUN_TEST(test_response_max_age);
    RUN_TEST(test_response_smaxage_beats_max_age);
    RUN_TEST(test_response_directives_case_and_quotes);
    RUN_TEST(test_response_multiple_cache_control_headers);
    RUN_TEST(test_response_default_ttl);
    RUN_TEST(test_response_malformed_max_age_falls_back_to_default);
    RUN_TEST(test_response_status_rules);
    RUN_TEST(test_response_not_cacheable_directives);
    RUN_TEST(test_response_expires_only_is_not_cached);
    RUN_TEST(test_response_vary);
    RUN_TEST(test_response_backend_age_is_subtracted);
    RUN_TEST(test_response_ttl_is_capped);
    RUN_TEST(test_vary_key);
    RUN_TEST(test_store_and_hit);
    RUN_TEST(test_miss_on_unknown_key);
    RUN_TEST(test_expiry_is_exact_and_counted);
    RUN_TEST(test_same_key_replaces_entry);
    RUN_TEST(test_invalidate);
    RUN_TEST(test_empty_body_and_binary_body);
    RUN_TEST(test_key_with_nul_safe_length);
    RUN_TEST(test_vary_variants);
    RUN_TEST(test_object_larger_than_max_object_not_stored);
    RUN_TEST(test_object_larger_than_total_budget_not_stored);
    RUN_TEST(test_lru_evicts_least_recently_used);
    RUN_TEST(test_memory_never_exceeds_budget);
    RUN_TEST(test_bytes_accounting_returns_to_zero);
    RUN_TEST(test_hit_miss_counters);
    return UNITY_END();
}
