#ifndef CONFIG_H
#define CONFIG_H

#include "balance.h"
#include "net.h"

enum action { ACT_STATIC, ACT_PROXY, ACT_CGI };

struct route {
    char host[256];   /* "*" = 아무 호스트나 */
    char prefix[256]; /* 끝의 '/'를 뗀 모양. "/" → "" */
    size_t prefix_len;
    enum action act;
    int root_fd;        /* static, cgi: 폴더 */
    char dir[512];
    struct pool *pool;  /* proxy */
    bool cache;
    bool cache_cookies; /* 쿠키가 있는 요청도 캐시를 쓴다. 모두에게 같은 응답인 경로에만 */
};

/* 시작할 때 한 번 채우고 그 뒤로는 읽기만 한다 → 모든 스레드가 락 없이 같이 쓴다 */
struct config {
    struct addr listen, admin;
    bool has_admin;
    bool supervise; /* -s: 감독 프로세스로 실행 */
    bool watchdog;  /* 감독이 워커의 멈춤까지 감시하나 (디버거로 멈춰 둘 때는 끈다) */
    int loops; /* 0 = 코어 수 */
    int max_connections;
    long long max_body, max_disk, cache_size;
    int max_cgi;
    int min_rate; /* 이보다 느리게(초당 바이트) 보내거나 받는 연결은 끊는다 */
    int head_ms, idle_ms, connect_ms, response_ms, stall_ms, drain_ms, cgi_ms, health_ms;
    char access_log[512]; /* 빈 문자열 = 끔, "-" = 표준 오류 */
    struct pool pools[16];
    int npools, nbackends;
    struct route routes[64];
    int nroutes;
};

void config_defaults(struct config *c);
bool config_line(struct config *c, char *line, char *err, size_t errcap); /* 설정 파일 한 줄 */
bool config_load(struct config *c, const char *path);                     /* 오류는 파일:줄과 함께 출력 */
int config_args(struct config *c, int argc, char **argv); /* 0 = 계속, 1 = 도움말 출력, 2 = 오류 */
void config_free(struct config *c);

#endif
