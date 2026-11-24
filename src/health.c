#include "health.h"
#include "log.h"
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FALL 3 /* 연속 실패 3번이면 제외 */
#define RISE 2 /* 연속 성공 2번이면 복귀 */

struct check {
    struct io io; /* 맨 앞 */
    struct pool *pool;
    struct backend *be;
    bool active, connected;
    char buf[16]; /* 응답 첫머리 "HTTP/1.1 200"까지만 본다 */
    size_t got;
    long long started, next_at;
    int fails, oks;
    bool was_down;
};

static struct check *g_checks;
static int g_n, g_evfd;
static const struct config *g_cfg;

int health_start(struct config *cfg, int evfd)
{
    g_cfg = cfg;
    g_evfd = evfd;
    g_checks = calloc((size_t)cfg->nbackends + 1, sizeof *g_checks);
    if (!g_checks)
        return -1;
    for (int i = 0; i < cfg->npools; i++)
        for (int k = 0; k < cfg->pools[i].n; k++)
            g_checks[g_n++] = (struct check){.io = {.fd = -1, .kind = IO_HEALTH},
                                             .pool = &cfg->pools[i], .be = &cfg->pools[i].be[k]};
    return 0;
}

static void close_check(struct check *k)
{
    if (k->io.fd >= 0) {
        ev_set(g_evfd, &k->io, 0);
        close(k->io.fd);
    }
    k->io.fd = -1;
    k->active = false;
}

static void verdict(struct check *k, bool ok, long long now)
{
    close_check(k);
    k->next_at = now + g_cfg->health_ms;
    k->fails = ok ? 0 : k->fails + 1;
    k->oks = ok ? k->oks + 1 : 0;
    if (k->oks >= RISE && atomic_exchange(&k->be->down, false)) {
        atomic_store(&k->be->timeouts, 0);
        log_note("백엔드 복귀: %s %s (헬스 체크 %d번 연속 성공)", k->pool->name, k->be->name, RISE);
    }
    if (k->fails >= FALL && !atomic_exchange(&k->be->down, true))
        log_note("백엔드 제외: %s %s (헬스 체크 %d번 연속 실패)", k->pool->name, k->be->name, FALL);
}

static void begin(struct check *k, long long now)
{
    k->io.fd = net_connect(&k->be->addr);
    k->io.want = 0;
    if (k->io.fd < 0) {
        /* 우리 쪽 자원 부족은 백엔드 탓이 아니다. 이번 차례만 건너뛴다 */
        bool ours = errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == EADDRNOTAVAIL;
        if (ours)
            k->next_at = now + g_cfg->health_ms;
        else
            verdict(k, false, now);
        return;
    }
    k->active = true;
    k->connected = false;
    k->got = 0;
    k->started = now;
    ev_set(g_evfd, &k->io, IO_W);
}

void health_tick(long long now)
{
    long long limit = g_cfg->health_ms < 1000 ? g_cfg->health_ms : 1000;
    for (int i = 0; i < g_n; i++) {
        struct check *k = &g_checks[i];
        /* 요청 처리 중에 제외됐다면 그 뒤의 성공부터 다시 센다 */
        bool down = atomic_load(&k->be->down);
        if (down && !k->was_down) {
            k->oks = 0;
            close_check(k);
        }
        k->was_down = down;
        if (k->active && now - k->started > limit)
            verdict(k, false, now);
        if (!k->active && now >= k->next_at)
            begin(k, now);
    }
}

void health_event(struct io *io, unsigned ready, long long now)
{
    struct check *k = (struct check *)io;
    if (!k->active)
        return; /* 같은 묶음에 남아 있던 낡은 이벤트 */
    if (!k->connected && (ready & IO_W)) {
        if (net_error(io->fd)) {
            verdict(k, false, now);
            return;
        }
        k->connected = true;
        if (!k->pool->health[0]) { /* 연결만 되면 살아 있는 것으로 본다 */
            verdict(k, true, now);
            return;
        }
        char req[512];
        int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: health-check\r\nConnection: close\r\n\r\n",
                         k->pool->health, k->be->name);
        if (write(io->fd, req, (size_t)n) != n) { /* 방금 연결한 소켓이라 이 정도는 한 번에 들어간다 */
            verdict(k, false, now);
            return;
        }
        ev_set(g_evfd, io, IO_R);
        return;
    }
    if (k->connected && (ready & IO_R)) {
        ssize_t r = read(io->fd, k->buf + k->got, 12 - k->got);
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        if (r > 0)
            k->got += (size_t)r;
        if (k->got == 12) /* "HTTP/1.1 2xx" 또는 3xx */
            verdict(k, !memcmp(k->buf, "HTTP/1.", 7) && (k->buf[9] == '2' || k->buf[9] == '3'), now);
        else if (r <= 0)
            verdict(k, false, now);
    }
}

void health_stop(void)
{
    for (int i = 0; i < g_n; i++)
        close_check(&g_checks[i]);
    free(g_checks);
}
