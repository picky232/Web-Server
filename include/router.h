/*
 * router.h — 요청을 정적 파일로 보낼지, 백엔드(reverse proxy)로 보낼지 결정.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stdbool.h>

#include "config.h"
#include "http.h"

typedef enum { ROUTE_STATIC, ROUTE_PROXY } route_kind_t;

/* path 가 prefix 와 같거나 "prefix/..." "prefix?..." 로 시작하는가.
 * 예) prefix "/api": "/api" O, "/api/x" O, "/api?q" O, "/apix" X */
bool router_prefix_matches(const char *path, const char *prefix);

/* 백엔드가 설정되어 있고 target 이 proxy_prefix 에 맞으면 ROUTE_PROXY */
route_kind_t router_route(const server_config_t *cfg, const char *target);

/* 이 경로 종류에서 허용하는 메서드인가. 아니면 *allow 에 "GET, HEAD" 같은 값. */
bool router_method_allowed(route_kind_t route, http_method_t method, const char **allow);

#endif /* ROUTER_H */
