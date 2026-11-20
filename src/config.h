#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <netinet/in.h>
#include <stddef.h>

/* 설정 파일 형식 (줄 단위, '#' 주석):
 *
 *   listen 8080
 *   docroot ./public
 *   timeout_header  10s     # 요청 헤더/본문을 다 받기까지 (Slowloris 방어)
 *   timeout_idle    30s     # keep-alive 대기 / 응답 쓰기 정체
 *   timeout_connect 3s      # 백엔드 connect
 *   timeout_upstream 30s    # 백엔드 응답 무반응
 *   pool_size 16            # 백엔드 서버당 keep-alive 유휴 연결 수 (0: 풀 끔, 요청마다 새 연결)
 *   timeout_pool 5s         # 유휴 연결 유지 시간 (백엔드의 keep-alive timeout 보다 짧게)
 *
 *   upstream api {
 *       server 127.0.0.1:8001
 *       server 127.0.0.1:8002
 *   }
 *   route /api/ api            # 접두사가 일치하면 백엔드로 전달 (최장 일치 우선)
 *   route /static/ files strip # strip: 접두사를 떼고 전달
 *   route /v1/ api strip cache # cache: 이 경로의 GET 응답을 캐시 (strip/cache 순서 무관)
 *
 *   cache_size 32m          # 캐시 메모리 상한 (k/m/g 단위, 0: 캐시 끔 = 기본값)
 *   cache_max_object 1m     # 캐시할 응답 본문 하나의 최대 크기
 *   cache_ttl 10s           # Cache-Control 이 없는 200/301 응답의 기본 TTL (0: 명시적 신선도만 캐시)
 *
 * 어떤 route 에도 맞지 않는 요청은 docroot 에서 정적으로 서빙한다.
 */

#define MAX_UPSTREAMS         16
#define MAX_UPSTREAM_SERVERS  16
#define MAX_ROUTES            64

typedef struct {
    struct sockaddr_in addr;
    char               label[64];     /* "127.0.0.1:8001" */
    long long          down_until_ms; /* 이 시각까지는 장애로 간주해 건너뜀 (passive health check) */

    /* 런타임 상태: 서버가 관리하는 유휴 연결 풀 (설정 파일과 무관) */
    void              *idle_head;     /* 가장 최근에 반납된 연결부터 이어지는 목록 (LIFO) */
    int                idle_n;
} up_server;

typedef struct {
    char      name[32];
    up_server servers[MAX_UPSTREAM_SERVERS];
    int       nservers;
    unsigned  rr;                     /* round-robin 시작점 */
} upstream;

typedef struct {
    char   prefix[256];
    size_t plen;
    int    up;                        /* app_config.ups 의 인덱스 */
    int    strip;
    int    cache;                     /* 이 경로의 GET 응답을 캐시한다 */
} route;

typedef struct {
    int  port;
    char docroot[PATH_MAX];
    int  header_timeout_ms, idle_timeout_ms, connect_timeout_ms, upstream_timeout_ms;
    int  pool_max;                    /* 백엔드 서버당 유휴 연결 최대 개수 (0 이면 풀 비활성) */
    int  pool_idle_ms;                /* 유휴 연결을 유지하는 시간 */
    size_t cache_max_bytes;           /* 캐시 메모리 상한 (0 이면 캐시 비활성) */
    size_t cache_max_object;          /* 캐시할 본문 하나의 최대 크기 */
    int    cache_ttl_ms;              /* Cache-Control 이 없는 응답에 적용할 기본 TTL (0 이면 명시적 신선도만 캐시) */
    upstream ups[MAX_UPSTREAMS];
    int      nups;
    route    routes[MAX_ROUTES];
    int      nroutes;
} app_config;

void config_defaults(app_config *c);

/* 성공 0, 실패 -1 (err 에 "line N: ..." 형태의 메시지). 기존 값 위에 덮어쓰므로 defaults 를 먼저 호출한다. */
int config_parse(app_config *c, const char *text, char *err, size_t errsz);
int config_load(app_config *c, const char *path, char *err, size_t errsz);

#endif
