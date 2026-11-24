#include "crash.h"
#include "health.h"
#include "log.h"
#include "net.h"
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
#include <sys/stat.h>
#include <unistd.h>

#define FORCE_MS 2000 /* 즉시 종료를 알린 뒤에도 안 끝나는 루프를 기다려 주는 시간 */

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
    sa.sa_handler = SIG_DFL; /* 무시 상태를 물려받으면 커널이 CGI 자식을 알아서 치워 우리가 회수할 수 없다 */
    sigaction(SIGCHLD, &sa, NULL);
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

enum accepted { ACCEPT_EMPTY, ACCEPT_MORE, ACCEPT_PAUSE };

/* 새 연결을 받아 루프에 나눠 준다. 더 있을 수 있으면 MORE, fd가 바닥나서 쉬어야 하면 PAUSE */
static enum accepted accept_some(struct server *s, int lfd, int origin)
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
            return ACCEPT_PAUSE;
        }
        if (m.fd < 0 && (errno == ECONNABORTED || errno == EINTR))
            continue; /* 받기 전에 손님이 떠났다 */
        if (m.fd < 0)
            return errno == ENOBUFS || errno == ENOMEM ? ACCEPT_PAUSE : ACCEPT_EMPTY;
        net_nodelay(m.fd);
        struct loop *l = pick_loop(s);
        atomic_fetch_add(&l->nconn, 1);
        if (!loop_send(l, &m)) { /* 루프가 너무 밀려 쪽지함이 찼다 */
            atomic_fetch_sub(&l->nconn, 1);
            close(m.fd);
        }
    }
    return ACCEPT_MORE;
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
        log_note("fd 한도 %llu: max_connections를 %d에서 %d로 줄입니다",
                (unsigned long long)rl.rlim_cur, cfg->max_connections, fit);
        cfg->max_connections = fit;
    }
}

static void set_phase(struct server *s, enum phase p)
{
    atomic_store(&s->phase, p);
    for (int i = 0; i < s->nloops; i++)
        loop_send(&s->loops[i], &(struct msg){.type = MSG_WAKE}); /* 쪽지함이 차서 실패해도, 그러면 이미 깨어 있다 */
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
        log_note("%s 포트 %d: listen 실패: %s", ip, net_port(a), strerror(e));
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

/* 모든 루프가 지난번 확인 뒤로 움직였나. 멈춘 루프가 있으면 한 번 알린다 */
static bool all_moving(struct server *s)
{
    bool ok = true;
    for (int i = 0; i < s->nloops; i++) {
        struct loop *l = &s->loops[i];
        unsigned b = atomic_load(&l->beats);
        bool stuck = b == l->seen_beats;
        if (stuck && !l->stuck)
            log_note("루프 %d이(가) 1초 넘게 움직이지 않습니다", i);
        l->stuck = stuck;
        l->seen_beats = b;
        ok = ok && !stuck;
    }
    return ok;
}

/* 메인 스레드가 하는 일: 새 연결 나눠 주기, 시그널, 헬스 체크, 심장 박동. 요청 처리는 하지 않는다 */
static void run(struct server *s, struct config *cfg, int evfd, struct door *doors, int ndoors, int alive_fd, int beat_fd)
{
    long long next_beat = now_ms() + 1000, force_at = 0;
    long long paused_until = 0, kill_at = 0;
    int signals = 0;
    while (signals == 0 || atomic_load(&s->loops_alive) > 0) {
        struct ev_event ev[64];
        int n = ev_wait(evfd, ev, 64, 100);
        long long now = now_ms();
        for (int i = 0; i < n; i++) {
            if (ev[i].io->kind == IO_LISTEN) {
                struct door *d = (struct door *)ev[i].io;
                if (signals == 0 && accept_some(s, d->io.fd, d->origin) == ACCEPT_PAUSE)
                    paused_until = now + 100;
            } else if (ev[i].io->kind == IO_HEALTH) {
                health_event(ev[i].io, ev[i].ready, now);
            }
        }
        char sig[16];
        bool stop = read(g_sig_pipe[0], sig, sizeof sig) > 0;
        if (alive_fd >= 0 && read(alive_fd, sig, 1) == 0) { /* 감독이 죽었다. 혼자 남지 않는다 */
            alive_fd = -1;
            stop = signals == 0; /* 이미 마무리 중이면 "두 번째 시그널"로 치지 않는다 */
        }
        if (stop) {
            if (signals++ == 0) {
                log_note("종료 시작: 처리 중인 요청을 마칩니다");
                for (int k = 0; k < ndoors; k++) {
                    while (accept_some(s, doors[k].io.fd, doors[k].origin) == ACCEPT_MORE)
                        ; /* 이미 handshake가 끝난 연결까지 받고 */
                    ev_set(evfd, &doors[k].io, 0);
                    close(doors[k].io.fd); /* 문을 닫는다. 이제 새 연결은 커널이 거절한다 */
                }
                set_phase(s, PHASE_DRAIN);
                kill_at = now + cfg->drain_ms;
            } else {
                kill_at = now; /* 두 번째 시그널: 기다리지 않는다 */
            }
        }
        for (int k = 0; signals == 0 && k < ndoors; k++)
            ev_set(evfd, &doors[k].io, now < paused_until ? 0 : IO_R);
        if (kill_at && now >= kill_at) {
            kill_at = 0;
            set_phase(s, PHASE_KILL);
            force_at = now + FORCE_MS;
        }
        if (force_at && now >= force_at) {
            /* 멈춘 루프는 "끝내라"를 듣지 못한다. 기다리면 영원히 끝나지 않는다 */
            log_note("멈춘 루프가 있어 강제로 끝냅니다");
            _exit(3);
        }
        if (now >= next_beat && signals == 0) {
            next_beat = now + 1000;
            if (all_moving(s) && beat_fd >= 0)
                (void)!write(beat_fd, ".", 1); /* 감독에게: 모두 살아 있다 */
        }
        if (signals == 0)
            health_tick(now);
    }
}

static int quit(struct config *cfg, int code)
{
    config_free(cfg);
    return code;
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
    if (rc)
        return quit(cfg, rc == 1 ? 0 : rc);
    struct inherited in = supervisor_inherit();
    if (cfg->supervise && !in.n)
        return supervisor_run(cfg, argv);
    plan_fds(cfg);
    g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    setup_signals();

    const struct addr *want[2] = {&cfg->listen};
    int origins[2] = {ORIGIN_WEB}, ndoors = 1;
    if (cfg->has_admin) {
        want[ndoors] = &cfg->admin;
        origins[ndoors++] = ORIGIN_ADMIN;
    }
    struct door doors[2];
    if (in.n && in.n != ndoors) {
        log_note("admin을 더하거나 빼려면 감독 프로세스를 다시 시작해야 합니다");
        return quit(cfg, 2);
    }
    for (int k = 0; k < ndoors; k++) {
        if (!open_door(&doors[k], want[k], origins[k], in.n ? in.fd[k] : -1))
            return quit(cfg, 1);
        if (in.n && !net_is_bound_to(in.fd[k], want[k])) {
            /* 소켓은 옛 주소 그대로인데 설정만 바뀐 채로 뜨면, 접근 제한을 새 주소 기준으로 잘못 판단한다 */
            log_note("포트 %d: listen 주소를 바꾸려면 감독 프로세스를 다시 시작해야 합니다", net_port(want[k]));
            return quit(cfg, 2);
        }
    }

    struct server s;
    memset(&s, 0, sizeof s);
    s.cfg = cfg;
    s.started_ms = now_ms();
    s.log_fd = open_log(cfg);
    for (int i = 0; i < cfg->nroutes; i++)
        if (cfg->routes[i].cache && cfg->cache_size && !s.cache)
            s.cache = cache_new(cfg->cache_size);
    struct stat lst;
    s.log_regular = s.log_fd >= 0 && fstat(s.log_fd, &lst) == 0 && S_ISREG(lst.st_mode);
    /* 다른 컴퓨터의 같은 프로그램과 구별되도록 시작할 때마다 다른 이름 */
    unsigned id = 0;
    int rnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (rnd < 0 || read(rnd, &id, sizeof id) != (ssize_t)sizeof id)
        id = (unsigned)now_us() ^ (unsigned)getpid();
    if (rnd >= 0)
        close(rnd);
    snprintf(s.via, sizeof s.via, "jiho-webserver-%08x", id);
    s.nloops = cfg->loops ? cfg->loops : (int)sysconf(_SC_NPROCESSORS_ONLN);
    s.loops = calloc((size_t)s.nloops, sizeof *s.loops);
    crash_install(&s);
    for (int i = 0; i < s.nloops; i++)
        if (!s.loops || loop_start(&s.loops[i], i, &s) < 0) {
            perror("loop_start");
            return quit(cfg, 1);
        }

    int evfd = ev_open();
    struct io sig_io = {.fd = g_sig_pipe[0], .kind = IO_PIPE};
    ev_set(evfd, &sig_io, IO_R);
    for (int k = 0; k < ndoors; k++)
        ev_set(evfd, &doors[k].io, IO_R);
    if (health_start(cfg, evfd) < 0)
        return quit(cfg, 1);
    log_note("listening on port %d (loops %d)", net_local_port(doors[0].io.fd), s.nloops);

    if (in.ready >= 0) /* 감독에게: 이제 받을 수 있다. 옛 워커를 내보내도 된다 */
        (void)!write(in.ready, "R", 1);
    run(&s, cfg, evfd, doors, ndoors, in.alive, in.ready);

    health_stop();
    for (int i = 0; i < s.nloops; i++)
        loop_join(&s.loops[i]);
    free(s.loops);
    cache_free(s.cache);
    close(evfd);
    close(g_spare_fd);
    if (s.log_fd > 2)
        close(s.log_fd);
    config_free(cfg);
    log_note("shutdown complete"); /* 로그를 받는 쪽이 막혀 있어도 종료가 멈추지 않게 */
    return 0;
}
