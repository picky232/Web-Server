#include "supervisor.h"
#include "log.h"
#include "net.h"
#include "str.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* 워커가 물려받는 fd 번호. 감독이 쓰는 번호와 겹치지 않게 떨어뜨려 둔다 */
#define FD_DOOR 10
#define FD_ALIVE 14
#define FD_READY 15
#define FD_SOURCE 32
#define ENV_NAME "WEBSERVER_FDS"
#define MAX_OLD 32
#define START_LIMIT_MS 30000 /* 백엔드 이름 해석이 느릴 수 있어 넉넉히 */
#define BACKOFF_MAX_MS 30000
#define GRACE_MS 5000        /* 워커가 스스로 정한 마무리 시간을 넘긴 뒤 더 봐주는 시간 */
#ifdef FAULT_INJECT
#define MISS_LIMIT 3 /* 시험이 오래 걸리지 않게 */
#else
#define MISS_LIMIT 10
#endif

struct inherited supervisor_inherit(void)
{
    struct inherited in = {.alive = -1, .ready = -1};
    const char *v = getenv(ENV_NAME);
    if (!v)
        return in;
    in.n = atoi(v);
    unsetenv(ENV_NAME); /* CGI 자식까지 내려가지 않게 */
    if (in.n < 1 || in.n > 2) {
        in.n = 0;
        return in;
    }
    for (int i = 0; i < in.n; i++) {
        in.fd[i] = FD_DOOR + i;
        net_nonblock(in.fd[i]);
    }
    in.alive = FD_ALIVE;
    in.ready = FD_READY;
    net_nonblock(in.alive);
    net_nonblock(in.ready);
    return in;
}

static int g_pipe[2];

static void on_signal(int sig)
{
    int e = errno;
    unsigned char b = (unsigned char)sig;
    (void)!write(g_pipe[1], &b, 1);
    errno = e;
}

struct worker {
    pid_t pid;         /* 0 = 없음 */
    int fd;            /* 워커가 쓰는 파이프. 첫 바이트 = 준비됨, 그 뒤 = 심장 박동 */
    long long since;   /* 띄운 시각 */
    long long kill_at; /* 이 시각까지 안 끝나면 강제 종료. 0 = 정하지 않음 */
};

struct sup {
    char **argv;
    int door[2], ndoors;
    int alive[2];
    struct worker current, starting, old[MAX_OLD];
    int nold;
    long long started_at, respawn_at;
    int backoff_ms;
    int misses;          /* 심장 박동이 연속으로 안 온 횟수 */
    long long next_tick;
    bool watch;
    int grace_ms;        /* 마무리할 시간(timeout_drain) + 여유 */
    bool ever_ready;     /* 한 번이라도 워커가 준비된 적이 있다 */
    bool reload;         /* 새 워커가 뜨는 중에 SIGHUP이 또 왔다 */
    bool stopping;
};

/* 앞으로 쓸 fd를 높은 번호로 옮긴다 (닫힘 표시 포함) */
static int move_up(int fd)
{
    int n = fcntl(fd, F_DUPFD_CLOEXEC, FD_SOURCE);
    close(fd);
    return n;
}

static void spawn_worker(struct sup *s)
{
    int rp[2];
    if (pipe(rp) < 0) {
        log_note("감독: pipe: %s", strerror(errno));
        return;
    }
    int r = move_up(rp[0]), w = move_up(rp[1]);
    fcntl(r, F_SETFL, O_NONBLOCK);

    char env[64];
    snprintf(env, sizeof env, ENV_NAME "=%d", s->ndoors);
    int n = 0;
    while (environ[n])
        n++;
    char **envp = calloc((size_t)n + 2, sizeof *envp);
    if (!envp) {
        close(r);
        close(w);
        return;
    }
    memcpy(envp, environ, (size_t)n * sizeof *envp);
    envp[n] = env;

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    for (int i = 0; i < s->ndoors; i++)
        posix_spawn_file_actions_adddup2(&fa, s->door[i], FD_DOOR + i);
    posix_spawn_file_actions_adddup2(&fa, s->alive[0], FD_ALIVE);
    posix_spawn_file_actions_adddup2(&fa, w, FD_READY);
    /* 그룹을 나누지 않으면 Ctrl+C가 워커에도 바로 가서, 감독이 보내는 것과 합쳐 두 번이 된다 */
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    pid_t pid;
    int err = posix_spawnp(&pid, s->argv[0], &fa, &at, s->argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(envp);
    close(w);
    if (err) {
        log_note("감독: 워커를 띄우지 못했습니다: %s", strerror(err));
        close(r);
        return;
    }
    s->starting = (struct worker){.pid = pid, .fd = r, .since = now_ms()};
    s->started_at = s->starting.since;
}

/* 다시 띄울 때를 정한다. 첫 번째는 바로(0.1초), 거듭 실패하면 두 배씩 늘려 쉬지 않고 되살리느라 CPU를 쓰지 않게 한다 */
static void schedule(struct sup *s)
{
    bool quick = now_ms() - s->started_at < 5000;
    s->backoff_ms = !quick || !s->backoff_ms ? 100 : s->backoff_ms * 2 < BACKOFF_MAX_MS ? s->backoff_ms * 2 : BACKOFF_MAX_MS;
    s->respawn_at = now_ms() + s->backoff_ms;
}

static void tell(struct sup *s, struct worker *w)
{
    kill(w->pid, SIGTERM);
    if (!w->kill_at) /* 멈춘 워커는 SIGTERM을 듣지 못할 수 있다 */
        w->kill_at = now_ms() + s->grace_ms;
}

static void drop_fd(struct worker *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
}

static void on_ready(struct sup *s)
{
    char b;
    if (read(s->starting.fd, &b, 1) != 1)
        return; /* 준비하기 전에 죽었다. SIGCHLD에서 처리한다 */
    s->ever_ready = true;
    log_note("감독: 워커 %d 준비됨", (int)s->starting.pid);
    if (s->current.pid) {
        if (!s->stopping) /* 멈추는 중이면 이미 SIGTERM을 보냈다. 또 보내면 "당장 끝내라"가 된다 */
            tell(s, &s->current);
        drop_fd(&s->current);
        if (s->nold < MAX_OLD)
            s->old[s->nold++] = s->current;
    }
    s->current = s->starting;
    s->starting = (struct worker){.fd = -1};
    s->misses = 0;
    s->respawn_at = 0;
}

/* 1초에 한 번. 시간이 아니라 "확인했는데 소식이 없던 횟수"를 센다.
   노트북이 잠들었다 깨면 시간은 훌쩍 지나 있지만, 그동안 감독도 세지 않았으므로 오판하지 않는다 */
static void tick(struct sup *s)
{
    char buf[256];
    bool heard = s->current.fd >= 0 && read(s->current.fd, buf, sizeof buf) > 0;
    s->misses = heard ? 0 : s->misses + 1;
    if (s->misses == MISS_LIMIT)
        log_note("감독: 워커 %d이(가) %d번 연속 소식이 없습니다. 새 워커로 바꿉니다", (int)s->current.pid, MISS_LIMIT);
}

static void enforce(struct worker *w)
{
    if (!w->pid || !w->kill_at || now_ms() < w->kill_at)
        return;
    log_note("감독: 워커 %d을(를) 강제로 끝냅니다", (int)w->pid);
    kill(w->pid, SIGKILL);
    w->kill_at = now_ms() + 1000;
}

static void on_child(struct sup *s)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        const char *how = WIFSIGNALED(st) ? "시그널" : "종료 코드";
        int code = WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st);
        if (pid == s->starting.pid) {
            drop_fd(&s->starting);
            s->starting = (struct worker){.fd = -1};
            log_note("감독: 새 워커가 시작하지 못했습니다(%s %d).%s", how, code, s->current.pid ? " 옛 워커를 그대로 둡니다" : "");
            if (!s->ever_ready)
                s->stopping = true; /* 첫 시작부터 실패: 설정이 잘못됐다 */
            else if (!s->stopping && (!s->current.pid || s->misses >= MISS_LIMIT))
                schedule(s); /* 멀쩡한 워커가 있으면 다시 시도하지 않는다. 다음 SIGHUP을 기다린다 */
        } else if (pid == s->current.pid) {
            drop_fd(&s->current);
            s->current = (struct worker){.fd = -1};
            s->misses = 0;
            if (s->stopping || s->starting.pid) /* 새 워커가 뜨는 중이면 그것이 이어받는다 */
                continue;
            schedule(s);
            log_note("감독: 워커 %d이(가) 죽었습니다(%s %d). %dms 뒤 다시 띄웁니다", (int)pid, how, code, s->backoff_ms);
        } else {
            for (int i = 0; i < s->nold; i++)
                if (s->old[i].pid == pid)
                    s->old[i] = s->old[--s->nold];
        }
    }
}

static void stop_all(struct sup *s)
{
    /* 마무리 중인 옛 워커는 이미 SIGTERM을 받았다. 두 번째로 멈추라고 할 때만 또 보낸다 */
    for (int i = 0; s->stopping && i < s->nold; i++)
        tell(s, &s->old[i]);
    if (s->current.pid)
        tell(s, &s->current);
    if (s->starting.pid)
        tell(s, &s->starting);
    s->stopping = true;
}

static int open_doors(struct sup *s, const struct config *cfg)
{
    const struct addr *a[2] = {&cfg->listen};
    int n = 1;
    if (cfg->has_admin)
        a[n++] = &cfg->admin;
    for (int i = 0; i < n; i++) {
        int fd = net_listen(a[i]);
        if (fd < 0) {
            log_note("포트 %d: listen 실패: %s", net_port(a[i]), strerror(errno));
            return -1;
        }
        s->door[s->ndoors++] = move_up(fd);
    }
    return 0;
}

int supervisor_run(struct config *cfg, char **argv)
{
    struct sup s;
    memset(&s, 0, sizeof s);
    s.argv = argv;
    s.current.fd = s.starting.fd = -1;
    s.watch = cfg->watchdog;
    s.grace_ms = cfg->drain_ms + GRACE_MS;
    bool ok = open_doors(&s, cfg) == 0 && net_pipe(g_pipe) == 0 && pipe(s.alive) == 0;
    config_free(cfg);
    if (!ok)
        return 1;
    s.alive[0] = move_up(s.alive[0]);
    s.alive[1] = move_up(s.alive[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    const int sigs[] = {SIGHUP, SIGTERM, SIGINT, SIGCHLD};
    for (size_t k = 0; k < sizeof sigs / sizeof sigs[0]; k++)
        sigaction(sigs[k], &sa, NULL);
    sa.sa_handler = SIG_IGN; /* 로그를 받던 쪽이 사라져도 감독이 죽지 않게 */
    sigaction(SIGPIPE, &sa, NULL);

    log_note("감독 프로세스 %d: 설정을 다시 읽으려면 kill -HUP %d", (int)getpid(), (int)getpid());
    spawn_worker(&s);
    if (!s.starting.pid)
        return 1;
    while (!s.stopping || s.current.pid || s.starting.pid || s.nold) {
        struct pollfd p[2] = {{.fd = g_pipe[0], .events = POLLIN}, {.fd = s.starting.fd, .events = POLLIN}};
        bool soon = s.respawn_at || s.starting.pid || s.nold || s.stopping;
        poll(p, 2, soon ? 50 : 1000);
        unsigned char sig[32];
        ssize_t n = read(g_pipe[0], sig, sizeof sig);
        if (s.starting.pid && (p[1].revents & POLLIN))
            on_ready(&s);
        for (ssize_t k = 0; k < n; k++) {
            if (sig[k] == SIGCHLD)
                on_child(&s);
            else if (sig[k] == SIGHUP)
                s.reload = true;
            else
                stop_all(&s);
        }
        enforce(&s.current);
        enforce(&s.starting);
        for (int k = 0; k < s.nold; k++)
            enforce(&s.old[k]);
        long long now = now_ms();
        if (s.starting.pid && !s.starting.kill_at && now - s.starting.since > START_LIMIT_MS)
            s.starting.kill_at = now; /* 시작하다 멈춘 워커 */
        if (s.current.pid && now >= s.next_tick) {
            s.next_tick = now + 1000;
            if (s.watch && !s.stopping)
                tick(&s);
        }
        if (s.stopping || s.starting.pid)
            continue;
        bool hung = s.watch && s.current.pid && s.misses >= MISS_LIMIT;
        bool reload = s.reload && s.current.pid; /* 뜨는 중에 온 SIGHUP도 여기서 이어서 처리된다 */
        if (reload || ((hung || !s.current.pid) && now >= s.respawn_at)) {
            if (reload)
                log_note("감독: 새 설정으로 워커를 띄웁니다");
            s.reload = false;
            s.respawn_at = 0;
            spawn_worker(&s);
            if (!s.starting.pid) /* 실행 파일 교체 중이거나 프로세스 한도. 포기하지 않고 다시 */
                schedule(&s);
        }
    }
    log_note("감독: 끝");
    return s.ever_ready ? 0 : 2;
}
