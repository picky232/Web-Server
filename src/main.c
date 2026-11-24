#include "health.h"
#include "net.h"
#include "server.h"
#include "str.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int g_sig_pipe[2];

/* 시그널 처리 함수 안에서는 할 수 있는 일이 거의 없다. 파이프에 번호 한 바이트만 쓰고 나온다 */
static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;
    (void)!write(g_sig_pipe[1], &b, 1);
    errno = e;
}

static void setup_signals(void)
{
    if (net_pipe(g_sig_pipe) < 0) {
        perror("pipe");
        exit(1);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN; /* 끊긴 연결에 쓰면 프로세스가 죽는 대신 EPIPE를 받는다 */
    sigaction(SIGPIPE, &sa, NULL);
}

/* 연결이 가장 적은 루프 */
static struct loop *pick_loop(struct server *s)
{
    struct loop *best = &s->loops[0];
    for (int i = 1; i < s->nloops; i++)
        if (atomic_load(&s->loops[i].nconn) < atomic_load(&best->nconn))
            best = &s->loops[i];
    return best;
}

static int g_spare_fd; /* fd가 바닥났을 때 잠깐 내놓을 예비 fd */

/* 새 연결을 받아 루프에 나눠 준다. fd가 바닥나서 쉬어야 하면 false */
static bool accept_some(struct server *s, int lfd, int origin)
{
    for (int i = 0; i < 64; i++) { /* 한 번에 64개까지. 나머지는 다음 바퀴에 다시 알려 준다 */
        struct msg m = {.type = MSG_CONN, .origin = origin};
        m.fd = net_accept(lfd, &m.peer);
        if (m.fd < 0 && (errno == EMFILE || errno == ENFILE)) {
            /* 그대로 두면 연결이 대기열에 남아 커널이 계속 깨우고 CPU를 다 쓴다.
               예비 fd를 내놓고 하나를 받아 바로 끊은 뒤 잠깐 쉰다 */
            close(g_spare_fd);
            int fd = net_accept(lfd, &m.peer);
            if (fd >= 0)
                close(fd);
            g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            return false;
        }
        if (m.fd < 0)
            return errno != ENOBUFS && errno != ENOMEM;
        net_nodelay(m.fd);
        struct loop *l = pick_loop(s);
        atomic_fetch_add(&l->nconn, 1);
        if (!loop_send(l, &m)) { /* 루프가 너무 밀려 쪽지함이 찼다 */
            atomic_fetch_sub(&l->nconn, 1);
            close(m.fd);
        }
    }
    return true;
}

/* 연결 하나가 fd를 둘(클라이언트, 백엔드)까지 쓴다. 모자라면 한도를 올리고, 안 되면 상한을 줄인다 */
static void plan_fds(struct config *cfg)
{
    rlim_t need = (rlim_t)cfg->max_connections * 2 + 256;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0 || rl.rlim_cur >= need)
        return;
    struct rlimit want = {.rlim_cur = rl.rlim_max < need ? rl.rlim_max : need, .rlim_max = rl.rlim_max};
    if (setrlimit(RLIMIT_NOFILE, &want) == 0)
        rl = want;
    if (rl.rlim_cur < need) {
        int fit = rl.rlim_cur > 320 ? (int)((rl.rlim_cur - 256) / 2) : 16;
        fprintf(stderr, "fd 한도 %llu: max_connections를 %d에서 %d로 줄입니다\n",
                (unsigned long long)rl.rlim_cur, cfg->max_connections, fit);
        cfg->max_connections = fit;
    }
}

static void tell_loops(struct server *s, enum msg_type type)
{
    for (int i = 0; i < s->nloops; i++)
        loop_send(&s->loops[i], &(struct msg){.type = type});
}

int main(int argc, char **argv)
{
    struct config *cfg = malloc(sizeof *cfg);
    if (!cfg)
        return 1;
    config_defaults(cfg);
    int rc = config_args(cfg, argc, argv);
    if (rc) {
        config_free(cfg);
        return rc == 1 ? 0 : rc;
    }
    plan_fds(cfg);
    g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    setup_signals();
    int lfd = net_listen(&cfg->listen);
    if (lfd < 0) {
        char ip[64];
        net_ip(&cfg->listen, ip, sizeof ip);
        fprintf(stderr, "%s 포트 %d: listen 실패: %s\n", ip, net_port(&cfg->listen), strerror(errno));
        return 1;
    }
    struct server s;
    memset(&s, 0, sizeof s);
    s.cfg = cfg;
    snprintf(s.via, sizeof s.via, "jiho-webserver-%d", net_local_port(lfd));
    s.nloops = cfg->loops ? cfg->loops : (int)sysconf(_SC_NPROCESSORS_ONLN);
    s.loops = calloc((size_t)s.nloops, sizeof *s.loops);
    for (int i = 0; i < s.nloops; i++)
        if (!s.loops || loop_start(&s.loops[i], i, &s) < 0) {
            perror("loop_start");
            return 1;
        }
    fprintf(stderr, "listening on port %d (loops %d)\n", net_local_port(lfd), s.nloops);

    int evfd = ev_open();
    struct io sig_io = {.fd = g_sig_pipe[0], .kind = IO_PIPE}, lis_io = {.fd = lfd, .kind = IO_LISTEN};
    ev_set(evfd, &sig_io, IO_R);
    ev_set(evfd, &lis_io, IO_R);
    if (health_start(cfg, evfd) < 0)
        return 1;

    long long paused_until = 0, kill_at = 0;
    int signals = 0;
    while (signals == 0 || atomic_load(&s.loops_alive) > 0) {
        struct ev_event ev[64];
        int n = ev_wait(evfd, ev, 64, 100);
        long long now = now_ms();
        for (int i = 0; i < n; i++) {
            if (ev[i].io->kind == IO_LISTEN) {
                if (signals == 0 && !accept_some(&s, lfd, ORIGIN_WEB)) {
                    paused_until = now + 100;
                    ev_set(evfd, ev[i].io, 0);
                }
            } else if (ev[i].io->kind == IO_HEALTH) {
                health_event(ev[i].io, ev[i].ready, now);
            } else {
                char buf[16];
                if (read(g_sig_pipe[0], buf, sizeof buf) <= 0)
                    continue;
                if (signals++ == 0) {
                    /* 이미 handshake가 끝나 대기열에 있는 연결까지 받은 뒤 문을 닫는다 */
                    accept_some(&s, lfd, ORIGIN_WEB);
                    ev_set(evfd, &lis_io, 0);
                    close(lfd);
                    fprintf(stderr, "종료 시작: 처리 중인 요청을 마칩니다\n");
                    tell_loops(&s, MSG_DRAIN);
                    kill_at = now + cfg->drain_ms;
                } else {
                    kill_at = now; /* 두 번째 시그널: 기다리지 않는다 */
                }
            }
        }
        if (paused_until && now >= paused_until && signals == 0) {
            paused_until = 0;
            ev_set(evfd, &lis_io, IO_R);
        }
        if (kill_at && now >= kill_at) {
            kill_at = 0;
            tell_loops(&s, MSG_KILL);
        }
        if (signals == 0)
            health_tick(now);
    }
    health_stop();
    for (int i = 0; i < s.nloops; i++)
        loop_join(&s.loops[i]);
    free(s.loops);
    config_free(cfg);
    fprintf(stderr, "shutdown complete\n");
    return 0;
}
