#ifndef CONFIG_H
#define CONFIG_H

#define MAX_BACKENDS 4

/* 프록시가 요청을 넘길 백엔드 하나 (예: 127.0.0.1:9000) */
struct backend {
    char host[64];
    char port[8];
    char hostport[80]; /* 백엔드로 보낼 Host 헤더 값 */
};

/* 실행 옵션 전체. main()이 한 번 채우고 그 뒤로는 읽기만 한다 */
struct config {
    const char *bind_addr;
    int port;
    const char *root;
    int threads;
    int queue;
    struct backend backends[MAX_BACKENDS];
    int nbackends;
    const char *proxy_prefix;
    int read_timeout_ms;
    int keepalive_ms;
    int proxy_timeout_ms;
    long max_body;
    int max_requests;
    int quiet;
};

extern struct config g_cfg;

/* 0 = 정상, 1 = --help 출력 후 종료, -1 = 잘못된 옵션 */
int config_parse(int argc, char **argv);

#endif
