#include "cache.h"
#include "router.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NBUCKETS     16384          /* 2의 거듭제곱 */
#define MAX_TTL_S    86400L         /* 서버가 아무리 길게 말해도 하루 이상 보관하지 않는다 */

/* ------------------------------ 유틸 ------------------------------ */

static unsigned char lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

static int ci_eq(const char *s, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)s[i]) != (unsigned char)lit[i]) return 0;
    return 1;
}

static int ci_contains(const char *s, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (n == 0 || len < n) return 0;
    for (size_t i = 0; i + n <= len; i++)
        if (ci_eq(s + i, n, lit)) return 1;
    return 0;
}

static uint64_t fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

/* ------------------------- Cache-Control 해석 ------------------------- */

typedef struct {
    int  no_store, no_cache, priv;
    long max_age, s_maxage;   /* 없으면 -1 */
} cc_t;

static long parse_delta(const char *s, size_t n) {   /* 숫자만 허용, 너무 크면 상한 처리, 형식 오류는 -1 */
    if (n && s[0] == '"') { s++; n--; if (n && s[n - 1] == '"') n--; }
    if (n == 0) return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
        if (v > 1000000000L) return 1000000000L;
    }
    return v;
}

static void cc_parse(const hp_header *h, int nh, const char *buf, cc_t *cc) {
    memset(cc, 0, sizeof *cc);
    cc->max_age = cc->s_maxage = -1;
    for (int k = 0; k < nh; k++) {
        if (!ci_eq(buf + h[k].name_off, h[k].name_len, "cache-control")) continue;
        const char *v = buf + h[k].value_off;
        size_t len = h[k].value_len, i = 0;
        while (i < len) {
            while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
            size_t ns = i;
            while (i < len && v[i] != '=' && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
            size_t nl = i - ns;
            long num = -1;
            if (i < len && v[i] == '=') {
                size_t vs = ++i;
                if (i < len && v[i] == '"') { i++; while (i < len && v[i] != '"') i++; if (i < len) i++; }
                else while (i < len && v[i] != ',') i++;
                size_t ve = i;
                while (ve > vs && (v[ve - 1] == ' ' || v[ve - 1] == '\t')) ve--;
                num = parse_delta(v + vs, ve - vs);
            }
            const char *n = v + ns;
            if (ci_eq(n, nl, "no-store"))        cc->no_store = 1;
            else if (ci_eq(n, nl, "no-cache"))   cc->no_cache = 1;
            else if (ci_eq(n, nl, "private"))    cc->priv = 1;
            else if (ci_eq(n, nl, "max-age"))    cc->max_age = num;
            else if (ci_eq(n, nl, "s-maxage"))   cc->s_maxage = num;
        }
    }
}

/* ------------------------------ 키/정책 ------------------------------ */

int cache_make_key(const hp_parser *p, const char *rbuf, char *out, size_t cap) {
    size_t hl = 0;
    const char *host = hp_header_get(p, rbuf, "host", &hl);
    if (!host) hl = 0;
    const char *uri = rbuf + p->uri_off;
    size_t off = router_origin_off(uri, p->uri_len);
    const char *o = uri + off;
    size_t ol = p->uri_len - off;
    int slash = (ol == 0);                    /* "http://host" -> "/" */
    size_t need = hl + 1 + (slash ? 1 : ol);
    if (need + 1 > cap) return -1;
    size_t n = 0;
    for (size_t i = 0; i < hl; i++) out[n++] = (char)lower((unsigned char)host[i]);
    out[n++] = '\n';
    if (slash) out[n++] = '/';
    else { memcpy(out + n, o, ol); n += ol; }
    out[n] = '\0';
    return (int)n;
}

void cache_request_policy(const hp_parser *p, const char *rbuf, int *allow_lookup, int *allow_store) {
    *allow_lookup = *allow_store = 1;
    cc_t cc;
    cc_parse(p->headers, p->nheaders, rbuf, &cc);

    /* 공유 캐시는 사용자별 응답일 수 있는 Authorization 요청을 저장/제공하지 않는다. Range 는 부분 응답이라 제외. */
    if (hp_header_get(p, rbuf, "authorization", NULL) || hp_header_get(p, rbuf, "range", NULL)) {
        *allow_lookup = *allow_store = 0;
        return;
    }
    if (cc.no_store) { *allow_lookup = *allow_store = 0; return; }
    if (cc.no_cache || cc.max_age == 0) *allow_lookup = 0;   /* 새로 받아 오되 결과는 저장해도 된다 */
    size_t pl = 0;
    const char *pragma = hp_header_get(p, rbuf, "pragma", &pl);
    if (pragma && ci_contains(pragma, pl, "no-cache")) *allow_lookup = 0;
}

int cache_response_policy(const pm_resp *r, const char *buf, long default_ttl_s, cache_resp_info *out) {
    if (r->status != 200 && r->status != 301 && r->status != 404) return 0;

    cc_t cc;
    cc_parse(r->headers, r->nheaders, buf, &cc);
    if (cc.no_store || cc.no_cache || cc.priv) return 0;

    int has_expires = 0;
    long backend_age = 0;
    out->vary[0] = '\0';
    size_t vlen = 0;

    for (int i = 0; i < r->nheaders; i++) {
        const hp_header *h = &r->headers[i];
        const char *nm = buf + h->name_off, *v = buf + h->value_off;
        if (ci_eq(nm, h->name_len, "set-cookie")) return 0;
        if (ci_eq(nm, h->name_len, "expires")) has_expires = 1;
        else if (ci_eq(nm, h->name_len, "age")) { long a = parse_delta(v, h->value_len); if (a > 0) backend_age = a; }
        else if (ci_eq(nm, h->name_len, "vary")) {
            size_t j = 0;
            while (j < h->value_len) {
                while (j < h->value_len && (v[j] == ' ' || v[j] == '\t' || v[j] == ',')) j++;
                size_t s = j;
                while (j < h->value_len && v[j] != ',' && v[j] != ' ' && v[j] != '\t') j++;
                size_t l = j - s;
                if (l == 0) continue;
                if (l == 1 && v[s] == '*') return 0;                       /* Vary: * 는 어떤 요청에도 재사용 불가 */
                if (vlen + l + 2 > sizeof out->vary) return 0;              /* 너무 길면 저장하지 않는다 */
                if (vlen) out->vary[vlen++] = ',';
                for (size_t k = 0; k < l; k++) out->vary[vlen++] = (char)lower((unsigned char)v[s + k]);
                out->vary[vlen] = '\0';
            }
        }
    }

    long ttl;
    if (cc.s_maxage >= 0)      ttl = cc.s_maxage;      /* 공유 캐시 전용 지시가 우선 */
    else if (cc.max_age >= 0)  ttl = cc.max_age;
    else if (has_expires)      return 0;               /* 날짜를 해석하지 않으므로 보수적으로 저장하지 않음 */
    else if (r->status == 404) return 0;               /* 부정 응답은 명시적 신선도가 있을 때만 */
    else                       ttl = default_ttl_s;    /* 휴리스틱 기본값 (0 이면 사용 안 함) */

    ttl -= backend_age;
    if (ttl <= 0) return 0;
    if (ttl > MAX_TTL_S) ttl = MAX_TTL_S;
    out->ttl_s = ttl;
    return 1;
}

int cache_vary_key(const hp_parser *p, const char *rbuf, const char *vary, char *out, size_t cap) {
    size_t n = 0;
    const char *s = vary;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        char name[64];
        if (l >= sizeof name) return -1;
        memcpy(name, s, l);
        name[l] = '\0';
        size_t vl = 0;
        const char *v = hp_header_get(p, rbuf, name, &vl);
        if (!v) vl = 0;
        if (n + l + 1 + vl + 1 + 1 > cap) return -1;
        memcpy(out + n, name, l); n += l;
        out[n++] = ':';
        if (vl) { memcpy(out + n, v, vl); n += vl; }
        out[n++] = '\n';
        s += l;
        if (*s == ',') s++;
    }
    out[n] = '\0';
    return (int)n;
}

/* ------------------------- 저장소: 해시 + LRU ------------------------- */

typedef struct centry {
    uint64_t hash;
    char    *key;       size_t key_len;
    char    *vary;                         /* Vary 헤더 이름들 ("" 가능) */
    char    *vary_key;                     /* 저장 당시 요청의 해당 헤더 값 */
    char    *head;      size_t head_len;
    char    *body;      size_t body_len;
    long long stored_ms, expire_ms;
    size_t   cost;
    struct centry *hnext;                  /* 해시 체인 */
    struct centry *lprev, *lnext;          /* LRU (lprev 쪽이 최근) */
} centry;

struct cache {
    centry **buckets;
    size_t   max_bytes, max_object, bytes, count;
    centry  *mru, *lru;
    cache_stats st;
};

cache *cache_create(size_t max_bytes, size_t max_object) {
    cache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->buckets = calloc(NBUCKETS, sizeof *c->buckets);
    if (!c->buckets) { free(c); return NULL; }
    c->max_bytes = max_bytes;
    c->max_object = max_object;
    return c;
}

static void entry_free(centry *e) {
    free(e->key); free(e->vary); free(e->vary_key); free(e->head); free(e->body);
    free(e);
}

static void lru_unlink(cache *c, centry *e) {
    if (e->lprev) e->lprev->lnext = e->lnext; else c->mru = e->lnext;
    if (e->lnext) e->lnext->lprev = e->lprev; else c->lru = e->lprev;
    e->lprev = e->lnext = NULL;
}

static void lru_push_front(cache *c, centry *e) {
    e->lprev = NULL;
    e->lnext = c->mru;
    if (c->mru) c->mru->lprev = e;
    c->mru = e;
    if (!c->lru) c->lru = e;
}

static void entry_remove(cache *c, centry *e) {
    centry **pp = &c->buckets[e->hash & (NBUCKETS - 1)];
    while (*pp && *pp != e) pp = &(*pp)->hnext;
    if (*pp) *pp = e->hnext;
    lru_unlink(c, e);
    c->bytes -= e->cost;
    c->count--;
    entry_free(e);
}

void cache_destroy(cache *c) {
    if (!c) return;
    while (c->mru) entry_remove(c, c->mru);
    free(c->buckets);
    free(c);
}

void cache_get_stats(const cache *c, cache_stats *out) {
    *out = c->st;
    out->entries = c->count;
    out->bytes = c->bytes;
}

static centry *find(cache *c, const char *key, size_t klen, uint64_t h) {
    for (centry *e = c->buckets[h & (NBUCKETS - 1)]; e; e = e->hnext)
        if (e->hash == h && e->key_len == klen && memcmp(e->key, key, klen) == 0) return e;
    return NULL;
}

static char *dup_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int cache_lookup(cache *c, const char *key, size_t klen, const hp_parser *p, const char *rbuf,
                 long long now_ms, cache_hit *out) {
    centry *e = find(c, key, klen, fnv1a(key, klen));
    if (!e) { c->st.misses++; return 0; }

    if (now_ms >= e->expire_ms) {           /* 만료: 조회 시점에 지연 제거 */
        entry_remove(c, e);
        c->st.expired++;
        c->st.misses++;
        return 0;
    }
    if (e->vary[0]) {                       /* 저장 당시와 같은 변형(variant)을 요구하는 요청인가 */
        char vk[1024];
        if (cache_vary_key(p, rbuf, e->vary, vk, sizeof vk) < 0 || strcmp(vk, e->vary_key) != 0) {
            c->st.misses++;
            return 0;
        }
    }
    lru_unlink(c, e);
    lru_push_front(c, e);
    c->st.hits++;
    out->head = e->head;  out->head_len = e->head_len;
    out->body = e->body;  out->body_len = e->body_len;
    out->age_s = (long)((now_ms - e->stored_ms) / 1000);
    return 1;
}

int cache_store(cache *c, const char *key, size_t klen, const cache_resp_info *info, const char *vary_key,
                const char *head, size_t head_len, const char *body, size_t body_len, long long now_ms) {
    if (body_len > c->max_object) return 0;
    size_t vlen = strlen(info->vary), vklen = vary_key ? strlen(vary_key) : 0;
    size_t cost = sizeof(centry) + klen + vlen + vklen + head_len + body_len + 5;
    if (cost > c->max_bytes) return 0;

    uint64_t h = fnv1a(key, klen);
    centry *old = find(c, key, klen, h);
    if (old) entry_remove(c, old);          /* 같은 키는 교체 (변형 하나만 보관) */

    while (c->bytes + cost > c->max_bytes && c->lru) {   /* 가장 오래 쓰이지 않은 것부터 쫓아낸다 */
        entry_remove(c, c->lru);
        c->st.evictions++;
    }

    centry *e = calloc(1, sizeof *e);
    if (!e) return 0;
    e->key = dup_n(key, klen);
    e->vary = dup_n(info->vary, vlen);
    e->vary_key = dup_n(vary_key ? vary_key : "", vklen);
    e->head = dup_n(head, head_len);
    e->body = dup_n(body ? body : "", body_len);
    if (!e->key || !e->vary || !e->vary_key || !e->head || !e->body) { entry_free(e); return 0; }
    e->key_len = klen;
    e->head_len = head_len;
    e->body_len = body_len;
    e->hash = h;
    e->stored_ms = now_ms;
    e->expire_ms = now_ms + info->ttl_s * 1000LL;
    e->cost = cost;

    e->hnext = c->buckets[h & (NBUCKETS - 1)];
    c->buckets[h & (NBUCKETS - 1)] = e;
    lru_push_front(c, e);
    c->bytes += cost;
    c->count++;
    c->st.stores++;
    return 1;
}

void cache_invalidate(cache *c, const char *key, size_t klen) {
    centry *e = find(c, key, klen, fnv1a(key, klen));
    if (e) { entry_remove(c, e); c->st.invalidations++; }
}
