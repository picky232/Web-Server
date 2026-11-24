#include "health.h"
#include "net.h"
#include "resolver.h"
#include "server.h"
#include "str.h"
#include "supervisor.h"

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

/* 연결이 가장 적은 루프. 동점이면 돌아가며 (안 그러면 짧은 연결이 전부 0번으로 쏠린다) */
static struct loop *pick_loop(struct server *s)
{
    static int next;
    struct loop *best = &s->loops[next++ % s->nloops];
    if (next >= s->nloops)
        next = 0;
    for (int i = 0; i < s->nloops; i++)
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

/* listen 소켓 하나. 어느 문인지 같이 기억한다 */
struct door {
    struct io io; /* 맨 앞 */
    int origin;
};

static bool open_door(struct door *d, const struct addr *a, int origin, int given)
{
    int fd = given >= 0 ? given : net_listen(a);
    if (fd < 0) {
        char ip[64];
        int e = errno;
        net_ip(a, ip, sizeof ip);
        fprintf(stderr, "%s 포트 %d: listen 실패: %s\n", ip, net_port(a), strerror(e));
        return false;
    }
    d->io = (struct io){.fd = fd, .kind = IO_LISTEN};
    d->origin = origin;
    return true;
}

static int open_log(const struct config *cfg)
{
    if (!cfg->access_log[0])
        return -1;
    if (!strcmp(cfg->access_log, "-"))
        return STDERR_FILENO;
    int fd = open(cfg->access_log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        perror(cfg->access_log);
    return fd;
}

/* 메인 스레드가 하는 일: 새 연결 나눠 주기, 시그널, 헬스 체크. 요청 처리는 하지 않는다 */
static void run(struct server *s, struct config *cfg, int evfd, struct door *doors, int ndoors, int alive_fd)
{
    long long paused_until = 0, kill_at = 0;
    int signals = 0;
    while (signals == 0 || atomic_load(&s->loops_alive) > 0) {
        struct ev_event ev[64];
        int n = ev_wait(evfd, ev, 64, 100);
        long long now = now_ms();
        for (int i = 0; i < n; i++) {
            if (ev[i].io->kind == IO_LISTEN) {
                struct door *d = (struct door *)ev[i].io;
                if (signals == 0 && !accept_some(s, d->io.fd, d->origin))
                    paused_until = now + 100;
            } else if (ev[i].io->kind == IO_HEALTH) {
                health_event(ev[i].io, ev[i].ready, now);
            }
        }
        char sig[16];
        bool stop = read(g_sig_pipe[0], sig, sizeof sig) > 0;
        if (alive_fd >= 0 && read(alive_fd, sig, 1) == 0) { /* 감독이 죽었다. 혼자 남지 않는다 */
            alive_fd = -1;
            stop = true;
        }
        if (stop) {
            if (signals++ == 0) {
                fprintf(stderr, "종료 시작: 처리 중인 요청을 마칩니다\n");
                for (int k = 0; k < ndoors; k++) {
                    accept_some(s, doors[k].io.fd, doors[k].origin); /* 이미 handshake가 끝난 연결까지 받고 */
                    ev_set(evfd, &doors[k].io, 0);
                    close(doors[k].io.fd); /* 문을 닫는다. 이제 새 연결은 커널이 거절한다 */
                }
                tell_loops(s, MSG_DRAIN);
                kill_at = now + cfg->drain_ms;
            } else {
                kill_at = now; /* 두 번째 시그널: 기다리지 않는다 */
            }
        }
        for (int k = 0; signals == 0 && k < ndoors; k++)
            ev_set(evfd, &doors[k].io, now < paused_until ? 0 : IO_R);
        if (kill_at && now >= kill_at) {
            kill_at = 0;
            tell_loops(s, MSG_KILL);
        }
        if (signals == 0)
            health_tick(now);
    }
}

int main(int argc, char **argv)
{
    /* 0~2번이 비어 있으면 소켓이 그 번호를 받아, 오류 메시지가 손님에게 나갈 수 있다 */
    int fd;
    while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd <= 2)
        ;
    close(fd);
    struct config *cfg = malloc(sizeof *cfg);
    if (!cfg)
        return 1;
    config_defaults(cfg);
    int rc = config_args(cfg, argc, argv);
    if (rc) {
        config_free(cfg);
        return rc == 1 ? 0 : rc;
    }
    struct inherited in = supervisor_inherit();
    if (cfg->supervise && !in.n)
        return supervisor_run(cfg, argv);
    plan_fds(cfg);
    g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    setup_signals();

    const struct addr *want[3] = {&cfg->listen};
    int origins[3] = {ORIGIN_WEB}, ndoors = 1;
    if (cfg->has_admin) {
        want[ndoors] = &cfg->admin;
        origins[ndoors++] = ORIGIN_ADMIN;
    }
    if (cfg->has_forward) {
        want[ndoors] = &cfg->forward;
        origins[ndoors++] = ORIGIN_FORWARD;
    }
    struct door doors[3];
    if (in.n && in.n != ndoors) {
        fprintf(stderr, "listen, admin, forward를 더하거나 빼려면 감독 프로세스를 다시 시작해야 합니다\n");
        return 2;
    }
    for (int k = 0; k < ndoors; k++) {
        if (!open_door(&doors[k], want[k], origins[k], in.n ? in.fd[k] : -1))
            return 1;
        if (in.n && net_port(want[k]) != net_local_port(in.fd[k]))
            fprintf(stderr, "포트 %d: 감독 프로세스를 다시 시작해야 바뀝니다\n", net_port(want[k]));
    }
    if (cfg->has_forward && resolver_start(2) < 0)
        return 1;

    struct server s;
    memset(&s, 0, sizeof s);
    s.cfg = cfg;
    s.started_ms = now_ms();
    s.log_fd = open_log(cfg);
    for (int i = 0; i < cfg->nroutes; i++)
        if (cfg->routes[i].cache && cfg->cache_size && !s.cache)
            s.cache = cache_new(cfg->cache_size);
    snprintf(s.via, sizeof s.via, "jiho-webserver-%d", net_local_port(doors[0].io.fd));
    s.nloops = cfg->loops ? cfg->loops : (int)sysconf(_SC_NPROCESSORS_ONLN);
    s.loops = calloc((size_t)s.nloops, sizeof *s.loops);
    for (int i = 0; i < s.nloops; i++)
        if (!s.loops || loop_start(&s.loops[i], i, &s) < 0) {
            perror("loop_start");
            return 1;
        }

    int evfd = ev_open();
    struct io sig_io = {.fd = g_sig_pipe[0], .kind = IO_PIPE};
    ev_set(evfd, &sig_io, IO_R);
    for (int k = 0; k < ndoors; k++)
        ev_set(evfd, &doors[k].io, IO_R);
    if (health_start(cfg, evfd) < 0)
        return 1;
    fprintf(stderr, "listening on port %d (loops %d)\n", net_local_port(doors[0].io.fd), s.nloops);

    if (in.ready >= 0) { /* 감독에게: 이제 받을 수 있다. 옛 워커를 내보내도 된다 */
        (void)!write(in.ready, "R", 1);
        close(in.ready);
    }
    run(&s, cfg, evfd, doors, ndoors, in.alive);

    health_stop();
    resolver_stop(); /* 이 뒤로는 루프에 쪽지를 보내지 않는다 */
    for (int i = 0; i < s.nloops; i++)
        loop_join(&s.loops[i]);
    free(s.loops);
    cache_free(s.cache);
    close(evfd);
    close(g_spare_fd);
    if (s.log_fd > 2)
        close(s.log_fd);
    config_free(cfg);
    fprintf(stderr, "shutdown complete\n");
    return 0;
}
