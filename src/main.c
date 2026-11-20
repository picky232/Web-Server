#include "config.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_config g_cfg;  /* 크기가 커서 스택이 아닌 정적 영역에 둔다 */

static void on_signal(int sig) {
    (void)sig;
    server_request_stop();
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-c config] [-p port] [-r docroot]\n"
                    "  CLI 옵션(-p, -r)은 설정 파일 값보다 우선한다.\n", prog);
}

int main(int argc, char **argv) {
    config_defaults(&g_cfg);

    /* 1차: 설정 파일 먼저 읽기 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            char err[256];
            if (config_load(&g_cfg, argv[++i], err, sizeof err) < 0) {
                fprintf(stderr, "config error: %s\n", err);
                return 2;
            }
        }
    }
    /* 2차: CLI 로 덮어쓰기 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { i++; }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) g_cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof g_cfg.docroot) { fprintf(stderr, "docroot too long\n"); return 2; }
            strcpy(g_cfg.docroot, argv[++i]);
        }
        else { usage(argv[0]); return 2; }
    }
    if (g_cfg.port <= 0 || g_cfg.port > 65535) { usage(argv[0]); return 2; }

    /* 쓰기 도중 상대가 끊겨도 프로세스가 죽지 않게 한다. */
    signal(SIGPIPE, SIG_IGN);

    /* SA_RESTART 를 쓰지 않아 kevent/epoll_wait 가 EINTR 로 깨어난다. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return server_run(&g_cfg) == 0 ? 0 : 1;
}
