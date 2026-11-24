#include "net.h"
#include "server.h"
#include "str.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void accept_some(struct server *s, int lfd, int origin)
{
    for (int i = 0; i < 64; i++) { /* 한 번에 64개까지. 나머지는 다음 바퀴에 다시 알려 준다 */
        struct msg m = {.type = MSG_CONN, .origin = origin};
        m.fd = net_accept(lfd, &m.peer);
        if (m.fd < 0)
            return;
        net_nodelay(m.fd);
        struct loop *l = pick_loop(s);
        atomic_fetch_add(&l->nconn, 1);
        if (!loop_send(l, &m)) {
            atomic_fetch_sub(&l->nconn, 1);
            close(m.fd);
        }
    }
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
    for (bool stop = false; !stop;) {
        struct ev_event ev[16];
        int n = ev_wait(evfd, ev, 16, -1);
        for (int i = 0; i < n; i++) {
            if (ev[i].io == &lis_io)
                accept_some(&s, lfd, ORIGIN_WEB);
            else
                stop = true;
        }
    }
    close(lfd);
    for (int i = 0; i < s.nloops; i++)
        loop_send(&s.loops[i], &(struct msg){.type = MSG_DRAIN});
    for (int i = 0; i < s.nloops; i++)
        loop_join(&s.loops[i]);
    free(s.loops);
    config_free(cfg);
    fprintf(stderr, "shutdown complete\n");
    return 0;
}
