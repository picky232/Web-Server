#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof *c);
    net_parse_addr("127.0.0.1:8080", &c->listen); /* 기본은 내 컴퓨터에서만. 밖으로 열 때만 명시한다 */
    c->max_connections = 4096;
    c->max_body = 10 << 20;
    c->max_disk = 256 << 20;
    c->cache_size = 64 << 20;
    c->max_cgi = 16;
    c->min_rate = 1024;
    c->head_ms = 10000;
    c->idle_ms = 15000;
    c->connect_ms = 1000;
    c->response_ms = 30000;
    c->stall_ms = 30000;
    c->drain_ms = 10000;
    c->cgi_ms = 10000;
    c->health_ms = 2000;
    c->watchdog = true;
    strcpy(c->access_log, "-");
}

static bool fail(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

/* "10m" → 10485760. 단위는 k, m, g */
static bool parse_size(const char *s, long long lo, long long hi, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    int shift = *end == 'k' ? 10 : *end == 'm' ? 20 : *end == 'g' ? 30 : 0;
    if (shift)
        end++;
    if (errno || end == s || *end || v < 0 || v > (hi >> shift))
        return false;
    *out = v << shift;
    return *out >= lo;
}

/* "10s", "500ms" → 밀리초 */
static bool parse_ms(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || v <= 0 || v > 3600000)
        return false;
    if (!strcmp(end, "s"))
        v *= 1000;
    else if (strcmp(end, "ms"))
        return false;
    *out = (int)v;
    return v <= 3600000;
}

static struct pool *find_pool(struct config *c, const char *name)
{
    for (int i = 0; i < c->npools; i++)
        if (!strcmp(c->pools[i].name, name))
            return &c->pools[i];
    return NULL;
}

/* pool 이름 [least_conn|round_robin] [max=N] [health=tcp|/경로] 주소... */
static bool parse_pool(struct config *c, char **t, int n, char *err, size_t cap)
{
    if (n < 2 || strlen(t[1]) >= sizeof c->pools[0].name)
        return fail(err, cap, "pool: 이름이 필요합니다");
    if (strspn(t[1], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(t[1]))
        return fail(err, cap, "pool: 이름에는 영문, 숫자, _, - 만 쓸 수 있습니다: %s", t[1]);
    if (find_pool(c, t[1]))
        return fail(err, cap, "pool %s: 이미 있는 이름입니다", t[1]);
    if (c->npools == (int)(sizeof c->pools / sizeof c->pools[0]))
        return fail(err, cap, "pool이 너무 많습니다");
    struct pool *p = &c->pools[c->npools];
    memset(p, 0, sizeof *p);
    strcpy(p->name, t[1]);
    p->max_inflight = 256;
    for (int i = 2; i < n; i++) {
        long long v;
        if (!strcmp(t[i], "least_conn"))
            p->policy = LEAST_CONN;
        else if (!strcmp(t[i], "round_robin"))
            p->policy = ROUND_ROBIN;
        else if (!strncmp(t[i], "max=", 4)) {
            if (!parse_size(t[i] + 4, 1, 100000, &v))
                return fail(err, cap, "pool %s: 잘못된 값 %s", p->name, t[i]);
            p->max_inflight = (int)v;
        } else if (!strcmp(t[i], "health=tcp")) {
            p->health[0] = '\0';
        } else if (!strncmp(t[i], "health=/", 8) && strlen(t[i] + 7) < sizeof p->health) {
            strcpy(p->health, t[i] + 7);
        } else {
            struct backend *b = &p->be[p->n];
            if (p->n == POOL_MAX_BACKENDS || strlen(t[i]) >= sizeof b->name || !net_parse_addr(t[i], &b->addr))
                return fail(err, cap, "pool %s: 백엔드 주소를 읽을 수 없습니다: %s", p->name, t[i]);
            strcpy(b->name, t[i]);
            b->index = c->nbackends++;
            p->n++;
        }
    }
    if (p->n == 0)
        return fail(err, cap, "pool %s: 백엔드가 없습니다", p->name);
    c->npools++;
    return true;
}

static bool open_dir(struct route *r, const char *dir, char *err, size_t cap)
{
    r->root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (r->root_fd < 0 || strlen(dir) >= sizeof r->dir)
        return fail(err, cap, "폴더를 열 수 없습니다: %s: %s", dir, strerror(errno));
    strcpy(r->dir, dir);
    return true;
}

/* route 호스트 접두사 static 폴더 | proxy 묶음 [cache [cookies]] | cgi 폴더 */
static bool parse_route(struct config *c, char **t, int n, char *err, size_t cap)
{
    if (n < 5)
        return fail(err, cap, "route: '호스트 접두사 동작 대상' 네 개가 필요합니다");
    if (c->nroutes == (int)(sizeof c->routes / sizeof c->routes[0]))
        return fail(err, cap, "route가 너무 많습니다");
    struct route *r = &c->routes[c->nroutes];
    memset(r, 0, sizeof *r);
    r->root_fd = -1;
    if (strlen(t[1]) >= sizeof r->host || t[2][0] != '/' || strlen(t[2]) >= sizeof r->prefix)
        return fail(err, cap, "route: 접두사는 '/'로 시작해야 합니다: %s", t[2]);
    for (size_t i = 0; t[1][i]; i++)
        r->host[i] = (char)(t[1][i] >= 'A' && t[1][i] <= 'Z' ? t[1][i] + 32 : t[1][i]);
    strcpy(r->prefix, t[2]);
    r->prefix_len = strlen(r->prefix);
    while (r->prefix_len && r->prefix[r->prefix_len - 1] == '/')
        r->prefix[--r->prefix_len] = '\0';
    if (!strcmp(t[3], "static")) {
        r->act = ACT_STATIC;
        if (!open_dir(r, t[4], err, cap))
            return false;
    } else if (!strcmp(t[3], "cgi")) {
        r->act = ACT_CGI;
        if (!open_dir(r, t[4], err, cap))
            return false;
    } else if (!strcmp(t[3], "proxy")) {
        r->act = ACT_PROXY;
        r->pool = find_pool(c, t[4]);
        if (!r->pool)
            return fail(err, cap, "route: 없는 pool입니다: %s (pool 줄을 먼저 적으세요)", t[4]);
        for (int i = 5; i < n; i++) {
            if (!strcmp(t[i], "cache"))
                r->cache = true;
            else if (!strcmp(t[i], "cookies") && r->cache)
                r->cache_cookies = true;
            else
                return fail(err, cap, "route: 모르는 옵션입니다: %s (cache, cache cookies)", t[i]);
        }
    } else {
        return fail(err, cap, "route: 모르는 동작입니다: %s", t[3]);
    }
    c->nroutes++;
    return true;
}

bool config_line(struct config *c, char *line, char *err, size_t cap)
{
    char *t[64];
    int n = 0;
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    char *save;
    for (char *w = strtok_r(line, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == 64)
            return fail(err, cap, "한 줄에 값이 너무 많습니다");
        t[n++] = w;
    }
    if (n == 0)
        return true;
    if (!strcmp(t[0], "pool"))
        return parse_pool(c, t, n, err, cap);
    if (!strcmp(t[0], "route"))
        return parse_route(c, t, n, err, cap);
    if (n != 2)
        return fail(err, cap, "%s: 값 하나가 필요합니다", t[0]);

    const struct {
        const char *name;
        int *ms;
    } times[] = {
        {"timeout_head", &c->head_ms}, {"timeout_idle", &c->idle_ms}, {"timeout_connect", &c->connect_ms},
        {"timeout_response", &c->response_ms}, {"timeout_stall", &c->stall_ms}, {"timeout_drain", &c->drain_ms},
        {"timeout_cgi", &c->cgi_ms}, {"health_interval", &c->health_ms},
    };
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++)
        if (!strcmp(t[0], times[i].name))
            return parse_ms(t[1], times[i].ms) ||
                   fail(err, cap, "%s: 시간은 10s나 500ms처럼 적습니다: %s", t[0], t[1]);

    long long v = 0;
    bool ok;
    if (!strcmp(t[0], "listen")) {
        ok = net_parse_addr(t[1], &c->listen);
    } else if (!strcmp(t[0], "admin")) {
        ok = c->has_admin = net_parse_addr(t[1], &c->admin);
    } else if (!strcmp(t[0], "loops")) {
        ok = !strcmp(t[1], "auto") || parse_size(t[1], 1, 256, &v);
        c->loops = (int)v;
    } else if (!strcmp(t[0], "max_connections")) {
        ok = parse_size(t[1], 1, 1000000, &v);
        c->max_connections = (int)v;
    } else if (!strcmp(t[0], "max_body")) {
        ok = parse_size(t[1], 0, 1LL << 40, &c->max_body);
    } else if (!strcmp(t[0], "max_upload_disk")) {
        ok = parse_size(t[1], 0, 1LL << 40, &c->max_disk);
    } else if (!strcmp(t[0], "cache_size")) {
        ok = parse_size(t[1], 0, 1LL << 40, &c->cache_size);
    } else if (!strcmp(t[0], "max_cgi")) {
        ok = parse_size(t[1], 0, 1024, &v);
        c->max_cgi = (int)v;
    } else if (!strcmp(t[0], "min_rate")) {
        ok = parse_size(t[1], 1, 1 << 30, &v);
        c->min_rate = (int)v;
    } else if (!strcmp(t[0], "watchdog")) {
        ok = !strcmp(t[1], "on") || !strcmp(t[1], "off");
        c->watchdog = !strcmp(t[1], "on");
    } else if (!strcmp(t[0], "access_log")) {
        ok = strlen(t[1]) < sizeof c->access_log;
        if (ok)
            strcpy(c->access_log, !strcmp(t[1], "off") ? "" : t[1]);
    } else {
        return fail(err, cap, "모르는 설정입니다: %s", t[0]);
    }
    return ok || fail(err, cap, "%s: 잘못된 값입니다: %s", t[0], t[1]);
}

bool config_load(struct config *c, const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return false;
    }
    char line[4096], err[512];
    bool ok = true;
    for (int no = 1; ok && fgets(line, sizeof line, f); no++) {
        ok = config_line(c, line, err, sizeof err);
        if (!ok)
            fprintf(stderr, "%s:%d: %s\n", path, no, err);
    }
    fclose(f);
    return ok;
}

static void usage(const char *prog)
{
    printf("사용법: %s [-c 설정파일] [-p 포트] [-r 정적폴더] [-t 루프수] [-s]\n"
           "  -c FILE  설정 파일 (예: conf/example.conf)\n"
           "  -p PORT  포트 (설정 파일보다 우선)\n"
           "  -r DIR   '/'에 정적 파일로 연결할 폴더 (설정 파일보다 우선)\n"
           "  -t N     이벤트 루프 수 (기본: 코어 수)\n"
           "  -s       감독 프로세스로 실행 (SIGHUP으로 무중단 설정 변경, 죽으면 다시 띄움)\n",
           prog);
}

int config_args(struct config *c, int argc, char **argv)
{
    const char *file = NULL, *port = NULL, *root = NULL, *loops = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:p:r:t:sh")) != -1) {
        switch (opt) {
        case 'c': file = optarg; break;
        case 'p': port = optarg; break;
        case 'r': root = optarg; break;
        case 't': loops = optarg; break;
        case 's': c->supervise = true; break;
        case 'h': usage(argv[0]); return 1;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "모르는 인자입니다: %s\n", argv[optind]);
        return 2;
    }
    if (file && !config_load(c, file))
        return 2;
    char err[512];
    if (port) {
        char host[64], line[128];
        net_ip(&c->listen, host, sizeof host);
        snprintf(line, sizeof line, strchr(host, ':') ? "listen [%s]:%s" : "listen %s:%s", host, port);
        if (!config_line(c, line, err, sizeof err)) {
            fprintf(stderr, "-p: %s\n", err);
            return 2;
        }
    }
    if (loops) {
        char line[64];
        snprintf(line, sizeof line, "loops %s", loops);
        if (!config_line(c, line, err, sizeof err)) {
            fprintf(stderr, "-t: %s\n", err);
            return 2;
        }
    }
    if (root || c->nroutes == 0) {
        /* "* /" 규칙이 이미 있으면 뺀다 */
        for (int i = 0; root && i < c->nroutes; i++)
            if (!strcmp(c->routes[i].host, "*") && c->routes[i].prefix_len == 0) {
                if (c->routes[i].root_fd >= 0)
                    close(c->routes[i].root_fd);
                c->routes[i] = c->routes[--c->nroutes];
                break;
            }
        char line[700];
        snprintf(line, sizeof line, "route * / static %s", root ? root : "./www");
        if (!config_line(c, line, err, sizeof err)) {
            fprintf(stderr, "-r: %s\n", err);
            return 2;
        }
    }
    return 0;
}

void config_free(struct config *c)
{
    for (int i = 0; i < c->nroutes; i++)
        if (c->routes[i].root_fd >= 0)
            close(c->routes[i].root_fd);
    free(c);
}
