#include "config.h"
#include "conn.h"
#include "net.h"
#include "static.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1; /* 핸들러에서는 깃발만 세운다. 정리는 메인 루프가 한다 */
}

int main(int argc, char **argv)
{
    int rc = config_parse(argc, argv);
    if (rc != 0)
        return rc > 0 ? 0 : 2;
    if (static_init(g_cfg.root) < 0)
        return 2;

    /* 끊긴 연결에 write하면 SIGPIPE로 프로세스가 죽는다. 무시하고 EPIPE 에러로 받는다 */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int port;
    int listenfd = net_listen(g_cfg.bind_addr, g_cfg.port, &port);
    if (listenfd < 0)
        return 1;
    fprintf(stderr, "listening on http://%s:%d\n", g_cfg.bind_addr, port);

    while (!g_stop) {
        /* accept에서 계속 멈춰 있으면 Ctrl+C를 늦게 알아챈다. 0.2초마다 깃발을 확인한다 */
        if (net_wait(listenfd, POLLIN, 200) <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int connfd = accept(listenfd, (struct sockaddr *)&peer, &plen);
        if (connfd < 0)
            continue;
        /* 응답을 읽지 않는 클라이언트 때문에 send가 영원히 멈추지 않게 한다 */
        struct timeval tv = {.tv_sec = g_cfg.read_timeout_ms / 1000,
                             .tv_usec = (g_cfg.read_timeout_ms % 1000) * 1000};
        setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        conn_handle(connfd, ip);
    }

    close(listenfd);
    fprintf(stderr, "shutdown complete\n");
    return 0;
}
