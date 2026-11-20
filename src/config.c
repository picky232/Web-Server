#include "config.h"

#include <ctype.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void config_defaults(app_config *c) {
    memset(c, 0, sizeof *c);
    c->port = 8080;
    snprintf(c->docroot, sizeof c->docroot, "./public");
    c->header_timeout_ms   = 10000;
    c->idle_timeout_ms     = 30000;
    c->connect_timeout_ms  = 3000;
    c->upstream_timeout_ms = 30000;
    c->pool_max            = 16;
    c->pool_idle_ms        = 5000;
    c->cache_max_bytes     = 0;                 /* 기본은 꺼짐: route 에 cache 를 쓰려면 cache_size 를 지정해야 한다 */
    c->cache_max_object    = 1024 * 1024;
    c->cache_ttl_ms        = 0;
}

static void seterr(char *err, size_t n, int line, const char *fmt, ...) {
    if (!err || n == 0) return;
    int k = snprintf(err, n, "line %d: ", line);
    if (k < 0 || (size_t)k >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + k, n - (size_t)k, fmt, ap);
    va_end(ap);
}

static int tokenize(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n == max) return -1;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* "3", "3s" (초) 또는 "500ms" */
static int parse_duration(const char *s, int *ms) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return -1;
    long out;
    if (*end == '\0' || strcmp(end, "s") == 0) out = v * 1000;
    else if (strcmp(end, "ms") == 0) out = v;
    else return -1;
    if (out > 86400000L) return -1;
    *ms = (int)out;
    return 0;
}

/* "0", "4096", "64k", "32m", "1g" */
static int parse_size(const char *s, size_t *out) {
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s || v < 0) return -1;
    long long mul = 1;
    if (*end == 'k' || *end == 'K') { mul = 1024LL; end++; }
    else if (*end == 'm' || *end == 'M') { mul = 1024LL * 1024; end++; }
    else if (*end == 'g' || *end == 'G') { mul = 1024LL * 1024 * 1024; end++; }
    if (*end != '\0') return -1;
    if (v > (1LL << 40) / mul) return -1;   /* 1TB 초과는 거부 (오입력/오버플로 방지) */
    *out = (size_t)(v * mul);
    return 0;
}

static int parse_port(const char *s, int *port) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 65535) return -1;
    *port = (int)v;
    return 0;
}

/* "HOST:PORT" 를 IPv4 주소로 해석 (시작 시 한 번만 getaddrinfo 를 호출한다) */
static int resolve_server(const char *spec, up_server *s) {
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec) return -1;
    char host[128];
    size_t hl = (size_t)(colon - spec);
    if (hl >= sizeof host) return -1;
    memcpy(host, spec, hl);
    host[hl] = '\0';
    int port;
    if (parse_port(colon + 1, &port) < 0) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || !res) return -1;
    memset(s, 0, sizeof *s);
    memcpy(&s->addr, res->ai_addr, sizeof s->addr);
    freeaddrinfo(res);
    snprintf(s->label, sizeof s->label, "%s", spec);
    return 0;
}

int config_parse(app_config *c, const char *text, char *err, size_t errsz) {
    char route_up[MAX_ROUTES][32];
    int  cur = -1;  /* 열려 있는 upstream 블록 인덱스 */
    int  lineno = 0;
    const char *p = text;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        lineno++;
        if (len >= sizeof line) { seterr(err, errsz, lineno, "line too long"); return -1; }
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *tok[8];
        int n = tokenize(line, tok, 8);
        if (n < 0) { seterr(err, errsz, lineno, "too many tokens"); return -1; }
        if (n == 0) continue;

        if (cur >= 0) {  /* upstream 블록 안 */
            if (strcmp(tok[0], "}") == 0 && n == 1) { cur = -1; continue; }
            if (strcmp(tok[0], "server") == 0 && n == 2) {
                upstream *u = &c->ups[cur];
                if (u->nservers >= MAX_UPSTREAM_SERVERS) { seterr(err, errsz, lineno, "too many servers"); return -1; }
                if (resolve_server(tok[1], &u->servers[u->nservers]) < 0) {
                    seterr(err, errsz, lineno, "invalid server '%s' (expected HOST:PORT)", tok[1]);
                    return -1;
                }
                u->nservers++;
                continue;
            }
            seterr(err, errsz, lineno, "unexpected '%s' inside upstream block", tok[0]);
            return -1;
        }

        if (strcmp(tok[0], "listen") == 0 && n == 2) {
            if (parse_port(tok[1], &c->port) < 0) { seterr(err, errsz, lineno, "invalid port '%s'", tok[1]); return -1; }
        } else if (strcmp(tok[0], "docroot") == 0 && n == 2) {
            if (strlen(tok[1]) >= sizeof c->docroot) { seterr(err, errsz, lineno, "docroot too long"); return -1; }
            strcpy(c->docroot, tok[1]);
        } else if (n == 2 && (!strcmp(tok[0], "timeout_header") || !strcmp(tok[0], "timeout_idle") ||
                              !strcmp(tok[0], "timeout_connect") || !strcmp(tok[0], "timeout_upstream") ||
                              !strcmp(tok[0], "timeout_pool"))) {
            int *dst = !strcmp(tok[0], "timeout_header")  ? &c->header_timeout_ms
                     : !strcmp(tok[0], "timeout_idle")    ? &c->idle_timeout_ms
                     : !strcmp(tok[0], "timeout_connect") ? &c->connect_timeout_ms
                     : !strcmp(tok[0], "timeout_pool")    ? &c->pool_idle_ms
                                                          : &c->upstream_timeout_ms;
            if (parse_duration(tok[1], dst) < 0) { seterr(err, errsz, lineno, "invalid duration '%s'", tok[1]); return -1; }
        } else if (strcmp(tok[0], "cache_size") == 0 && n == 2) {
            if (parse_size(tok[1], &c->cache_max_bytes) < 0) { seterr(err, errsz, lineno, "invalid cache_size '%s'", tok[1]); return -1; }
        } else if (strcmp(tok[0], "cache_max_object") == 0 && n == 2) {
            if (parse_size(tok[1], &c->cache_max_object) < 0 || c->cache_max_object == 0) {
                seterr(err, errsz, lineno, "invalid cache_max_object '%s'", tok[1]);
                return -1;
            }
        } else if (strcmp(tok[0], "cache_ttl") == 0 && n == 2) {
            if (strcmp(tok[1], "0") == 0) c->cache_ttl_ms = 0;     /* 0: 기본 TTL 사용 안 함 */
            else if (parse_duration(tok[1], &c->cache_ttl_ms) < 0) { seterr(err, errsz, lineno, "invalid duration '%s'", tok[1]); return -1; }
        } else if (strcmp(tok[0], "pool_size") == 0 && n == 2) {
            char *end;
            long v = strtol(tok[1], &end, 10);
            if (end == tok[1] || *end != '\0' || v < 0 || v > 1024) {
                seterr(err, errsz, lineno, "invalid pool_size '%s' (0..1024)", tok[1]);
                return -1;
            }
            c->pool_max = (int)v;
        } else if (strcmp(tok[0], "upstream") == 0 && n == 3 && strcmp(tok[2], "{") == 0) {
            if (c->nups >= MAX_UPSTREAMS) { seterr(err, errsz, lineno, "too many upstreams"); return -1; }
            if (strlen(tok[1]) >= sizeof c->ups[0].name) { seterr(err, errsz, lineno, "upstream name too long"); return -1; }
            for (int i = 0; i < c->nups; i++)
                if (strcmp(c->ups[i].name, tok[1]) == 0) { seterr(err, errsz, lineno, "duplicate upstream '%s'", tok[1]); return -1; }
            cur = c->nups++;
            memset(&c->ups[cur], 0, sizeof c->ups[cur]);
            strcpy(c->ups[cur].name, tok[1]);
        } else if (strcmp(tok[0], "route") == 0 && n >= 3) {
            if (c->nroutes >= MAX_ROUTES) { seterr(err, errsz, lineno, "too many routes"); return -1; }
            int strip = 0, cache = 0;
            for (int k = 3; k < n; k++) {      /* 옵션: strip, cache (순서 무관, 중복 불가) */
                if (strcmp(tok[k], "strip") == 0 && !strip) strip = 1;
                else if (strcmp(tok[k], "cache") == 0 && !cache) cache = 1;
                else { seterr(err, errsz, lineno, "unknown route option '%s'", tok[k]); return -1; }
            }
            if (tok[1][0] != '/') { seterr(err, errsz, lineno, "route prefix must start with '/'"); return -1; }
            if (strlen(tok[1]) >= sizeof c->routes[0].prefix || strlen(tok[2]) >= sizeof route_up[0]) {
                seterr(err, errsz, lineno, "route too long");
                return -1;
            }
            route *r = &c->routes[c->nroutes];
            strcpy(r->prefix, tok[1]);
            r->plen  = strlen(tok[1]);
            r->strip = strip;
            r->cache = cache;
            strcpy(route_up[c->nroutes], tok[2]);
            c->nroutes++;
        } else {
            seterr(err, errsz, lineno, "unknown or malformed directive '%s'", tok[0]);
            return -1;
        }
    }

    if (cur >= 0) { seterr(err, errsz, lineno, "unterminated upstream block"); return -1; }

    for (int i = 0; i < c->nups; i++)
        if (c->ups[i].nservers == 0) { seterr(err, errsz, lineno, "upstream '%s' has no servers", c->ups[i].name); return -1; }

    /* route 가 upstream 보다 먼저 나올 수 있으므로 마지막에 이름을 인덱스로 해석한다 */
    for (int i = 0; i < c->nroutes; i++) {
        int found = -1;
        for (int j = 0; j < c->nups; j++)
            if (strcmp(c->ups[j].name, route_up[i]) == 0) { found = j; break; }
        if (found < 0) { seterr(err, errsz, lineno, "route '%s' references unknown upstream '%s'", c->routes[i].prefix, route_up[i]); return -1; }
        c->routes[i].up = found;
        if (c->routes[i].cache && c->cache_max_bytes == 0) {
            seterr(err, errsz, lineno, "route '%s' uses 'cache' but cache_size is 0 (set cache_size)", c->routes[i].prefix);
            return -1;
        }
    }
    return 0;
}

int config_load(app_config *c, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errsz, "cannot open '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024 * 1024) { fclose(f); snprintf(err, errsz, "config file too large or unreadable"); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); snprintf(err, errsz, "out of memory"); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    int rc = config_parse(c, buf, err, errsz);
    free(buf);
    return rc;
}
