/*
 * config.h — 서버 설정 값과 명령행(CLI) 파싱.
 *
 * 모든 설정은 프로그램 시작 시 main()에서 한 번 채워지고, 그 뒤로는
 * 읽기 전용(read-only)으로만 사용된다. 그래서 여러 worker 스레드가
 * 동시에 읽어도 lock이 필요 없다.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CONFIG_MAX_BACKENDS 4    /* --backend 를 최대 몇 개까지 받을지 */
#define CONFIG_HOST_MAX 256
#define CONFIG_PATH_MAX 4096
#define CONFIG_PREFIX_MAX 128

/* "127.0.0.1:9000" 같은 백엔드 주소 한 개 (아직 DNS 해석 전) */
typedef struct {
    char host[CONFIG_HOST_MAX];
    int port;
} backend_spec_t;

typedef struct {
    char bind_addr[64];          /* 리스닝 IPv4 주소 (기본 0.0.0.0) */
    int port;                    /* 리스닝 포트, 0이면 OS가 빈 포트를 고름 */
    char doc_root[CONFIG_PATH_MAX]; /* 정적 파일 루트 디렉터리 */
    int threads;                 /* worker 스레드 수 (고정) */
    int queue_capacity;          /* accept 후 worker를 기다리는 연결 큐 크기 */

    char proxy_prefix[CONFIG_PREFIX_MAX]; /* 이 경로로 시작하면 reverse proxy */
    backend_spec_t backends[CONFIG_MAX_BACKENDS];
    int backend_count;           /* 0이면 프록시 비활성 */

    int read_timeout_ms;         /* 요청 1개(헤더+본문)를 다 받는 데 허용하는 시간 */
    int keepalive_timeout_ms;    /* keep-alive 연결에서 다음 요청을 기다리는 시간 */
    int proxy_timeout_ms;        /* 백엔드 connect / 응답 대기 시간 */
    unsigned long long max_body_bytes; /* 요청 본문 상한 → 넘으면 413 */
    int max_requests_per_conn;   /* 한 연결에서 처리할 최대 요청 수 */
    bool quiet;                  /* true면 access log 출력 생략 */
} server_config_t;

typedef enum {
    CONFIG_OK = 0,
    CONFIG_HELP = 1,   /* --help 요청: usage 출력 후 정상 종료 */
    CONFIG_ERROR = -1  /* 잘못된 인자: err 버퍼에 이유가 들어 있음 */
} config_result_t;

void config_set_defaults(server_config_t *cfg);
config_result_t config_parse_args(server_config_t *cfg, int argc, char **argv,
                                  char *err, size_t errlen);
void config_print_usage(FILE *out, const char *prog);

#endif /* CONFIG_H */
