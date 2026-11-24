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
#define START_LIMIT_MS 30000

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

struct sup {
    char **argv;
    int door[2], ndoors;
    int alive[2];
    pid_t current, starting, old[MAX_OLD];
    int nold;
    int ready_fd;        /* starting 워커의 준비 신호 */
    long long started_at, respawn_at;
    int backoff_ms;
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
        perror("pipe");
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
    s->starting = pid;
    s->ready_fd = r;
    s->started_at = now_ms();
}

/* 다시 띄울 때를 정한다. 거듭 실패하면 간격을 두 배로 늘려 쉬지 않고 되살리느라 CPU를 쓰지 않게 한다 */
static void schedule(struct sup *s)
{
    bool quick = now_ms() - s->started_at < 5000;
    s->backoff_ms = !quick || !s->backoff_ms ? 100 : s->backoff_ms < 5000 ? s->backoff_ms * 2 : 5000;
    s->respawn_at = now_ms() + s->backoff_ms;
}

static void on_ready(struct sup *s)
{
    char b;
    if (read(s->ready_fd, &b, 1) != 1)
        return; /* 준비하기 전에 죽었다. SIGCHLD에서 처리한다 */
    close(s->ready_fd);
    s->ready_fd = -1;
    s->ever_ready = true;
    log_note("감독: 워커 %d 준비됨", (int)s->starting);
    if (s->current) {
        if (!s->stopping) /* 멈추는 중이면 이미 SIGTERM을 보냈다. 또 보내면 "당장 끝내라"가 된다 */
            kill(s->current, SIGTERM);
        if (s->nold < MAX_OLD)
            s->old[s->nold++] = s->current;
    }
    s->current = s->starting;
    s->starting = 0;
}

static void on_child(struct sup *s)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (pid == s->starting) {
            s->starting = 0;
            if (s->ready_fd >= 0)
                close(s->ready_fd);
            s->ready_fd = -1;
            log_note("감독: 새 워커가 시작하지 못했습니다.%s", s->current ? " 옛 워커를 그대로 둡니다" : "");
            if (!s->ever_ready)
                s->stopping = true; /* 첫 시작부터 실패: 설정이 잘못됐다 */
            else if (!s->current && !s->stopping)
                schedule(s);
        } else if (pid == s->current) {
            s->current = 0;
            if (s->stopping || s->starting) /* 새 워커가 뜨는 중이면 그것이 이어받는다 */
                continue;
            schedule(s);
            log_note("감독: 워커 %d이(가) 죽었습니다(%s %d). %dms 뒤 다시 띄웁니다", (int)pid,
                     WIFSIGNALED(st) ? "시그널" : "종료 코드", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), s->backoff_ms);
        } else {
            for (int i = 0; i < s->nold; i++)
                if (s->old[i] == pid)
                    s->old[i] = s->old[--s->nold];
        }
    }
}

static void stop_all(struct sup *s)
{
    /* 마무리 중인 옛 워커는 이미 SIGTERM을 받았다. 두 번째로 멈추라고 할 때만 또 보낸다 */
    for (int i = 0; s->stopping && i < s->nold; i++)
        kill(s->old[i], SIGTERM);
    if (s->current)
        kill(s->current, SIGTERM);
    if (s->starting)
        kill(s->starting, SIGTERM);
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
            fprintf(stderr, "포트 %d: listen 실패: %s\n", net_port(a[i]), strerror(errno));
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
    s.ready_fd = -1;
    if (open_doors(&s, cfg) < 0 || net_pipe(g_pipe) < 0 || pipe(s.alive) < 0)
        return 1;
    s.alive[0] = move_up(s.alive[0]);
    s.alive[1] = move_up(s.alive[1]);
    config_free(cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    const int sigs[] = {SIGHUP, SIGTERM, SIGINT, SIGCHLD};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);

    sa.sa_handler = SIG_IGN; /* 로그를 받던 쪽이 사라져도 감독이 죽지 않게 */
    sigaction(SIGPIPE, &sa, NULL);

    log_note("감독 프로세스 %d: 설정을 다시 읽으려면 kill -HUP %d", (int)getpid(), (int)getpid());
    spawn_worker(&s);
    if (!s.starting)
        return 1;
    while (!s.stopping || s.current || s.starting || s.nold) {
        struct pollfd p[2] = {{.fd = g_pipe[0], .events = POLLIN}, {.fd = s.ready_fd, .events = POLLIN}};
        poll(p, 2, s.respawn_at || s.starting ? 50 : -1);
        unsigned char sig[32];
        ssize_t n = read(g_pipe[0], sig, sizeof sig);
        if (s.ready_fd >= 0 && (p[1].revents & POLLIN))
            on_ready(&s);
        for (ssize_t i = 0; i < n; i++) {
            if (sig[i] == SIGCHLD)
                on_child(&s);
            else if (sig[i] == SIGHUP)
                s.reload = true;
            else
                stop_all(&s);
        }
        if (s.starting && now_ms() - s.started_at > START_LIMIT_MS)
            kill(s.starting, SIGKILL); /* 시작하다 멈춘 워커. SIGCHLD에서 정리한다 */
        if (s.stopping || s.starting)
            continue;
        if (s.reload && s.current) { /* 뜨는 중에 온 SIGHUP도 여기서 이어서 처리된다 */
            s.reload = false;
            log_note("감독: 새 설정으로 워커를 띄웁니다");
            spawn_worker(&s);
        } else if (!s.current && s.respawn_at && now_ms() >= s.respawn_at) {
            s.respawn_at = 0;
            spawn_worker(&s);
            if (!s.starting) /* 실행 파일 교체 중이거나 프로세스 한도. 포기하지 않고 다시 */
                schedule(&s);
        }
    }
    log_note("감독: 끝");
    return s.ever_ready ? 0 : 2;
}
