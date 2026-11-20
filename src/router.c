/*
 * router.c — 경로 접두사 기반의 아주 단순한 라우팅.
 *
 * 라우팅은 "원본 target 문자열" 기준이다. 퍼센트 인코딩된 "/%61pi" 는
 * "/api" 로 취급하지 않고 정적 파일 쪽으로 간다(거기서 404). 반대로 프록시로
 * 가는 요청은 proxy.c 에서 ".." 로 접두사를 벗어나지 않는지 한 번 더 검사한다.
 */
#include "router.h"

#include <string.h>

bool router_prefix_matches(const char *path, const char *prefix)
{
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl) != 0)
        return false;
    char next = path[pl];
    return next == '\0' || next == '/' || next == '?';
}

route_kind_t router_route(const server_config_t *cfg, const char *target)
{
    if (cfg->backend_count > 0 && router_prefix_matches(target, cfg->proxy_prefix))
        return ROUTE_PROXY;
    return ROUTE_STATIC;
}

bool router_method_allowed(route_kind_t route, http_method_t method, const char **allow)
{
    if (route == ROUTE_STATIC) {
        *allow = "GET, HEAD";
        return method == HTTP_METHOD_GET || method == HTTP_METHOD_HEAD;
    }
    *allow = "GET, HEAD, POST";
    return method == HTTP_METHOD_GET || method == HTTP_METHOD_HEAD ||
           method == HTTP_METHOD_POST;
}
