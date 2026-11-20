#include "router.h"

#include <string.h>

/* "scheme://authority" 다음 위치(권한부 끝)를 찾는다. 없으면 -1 */
static long authority_end(const char *uri, size_t len) {
    size_t i = 0;
    while (i + 2 < len && !(uri[i] == ':' && uri[i + 1] == '/' && uri[i + 2] == '/')) i++;
    if (i + 2 >= len) return -1;
    size_t s = i + 3;
    while (s < len && uri[s] != '/') s++;
    return (long)s;
}

size_t router_origin_off(const char *uri, size_t len) {
    if (len > 0 && uri[0] == '/') return 0;
    long a = authority_end(uri, len);
    return a < 0 ? len : (size_t)a;
}

int router_path_span(const char *uri, size_t len, size_t *off, size_t *plen) {
    if (len == 0) return -1;
    size_t s = router_origin_off(uri, len);
    if (uri[0] != '/' && authority_end(uri, len) < 0) return -1;
    if (s == len) { *off = 0; *plen = 0; return 0; }  /* "http://host" */
    size_t e = s;
    while (e < len && uri[e] != '?' && uri[e] != '#') e++;
    *off  = s;
    *plen = e - s;
    return 0;
}

const route *router_match(const app_config *c, const char *path, size_t plen) {
    if (plen == 0) { path = "/"; plen = 1; }
    const route *best = NULL;
    for (int i = 0; i < c->nroutes; i++) {
        const route *r = &c->routes[i];
        if (plen >= r->plen && memcmp(path, r->prefix, r->plen) == 0)
            if (!best || r->plen > best->plen) best = r;
    }
    return best;
}

int upstream_pick(upstream *u, long long now_ms, unsigned tried) {
    unsigned n = (unsigned)u->nservers;
    if (n == 0) return -1;
    unsigned start = u->rr++;
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < n; i++) {
            unsigned idx = (start + i) % n;
            if (tried & (1u << idx)) continue;
            if (pass == 0 && u->servers[idx].down_until_ms > now_ms) continue;
            return (int)idx;
        }
    }
    return -1;
}

void upstream_mark_down(upstream *u, int idx, long long until_ms) {
    if (idx >= 0 && idx < u->nservers) u->servers[idx].down_until_ms = until_ms;
}
