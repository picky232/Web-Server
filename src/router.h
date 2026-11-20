#ifndef ROUTER_H
#define ROUTER_H

#include "config.h"

/* URI 에서 경로 구간(쿼리/프래그먼트 제외)을 구한다. absolute-form("http://h/p?q")도 지원.
 * 성공 0 (경로가 비어 있으면 plen == 0 → 호출자는 "/" 로 취급), 형식 오류 -1. */
int router_path_span(const char *uri, size_t len, size_t *off, size_t *plen);

/* origin-form 이 시작되는 오프셋. "/x" 면 0, "http://h/x" 면 "/x" 위치, 경로가 없으면 len. */
size_t router_origin_off(const char *uri, size_t len);

/* 최장 접두사 일치. 없으면 NULL. plen == 0 은 "/" 로 취급. */
const route *router_match(const app_config *c, const char *path, size_t plen);

/* Round-Robin 으로 서버를 고른다. tried 는 이미 시도한 서버의 비트마스크.
 * 장애 표시(down_until_ms > now)된 서버는 먼저 건너뛰고, 그래도 없으면 장애 서버도 시도한다.
 * 모두 시도했으면 -1. */
int upstream_pick(upstream *u, long long now_ms, unsigned tried);

void upstream_mark_down(upstream *u, int idx, long long until_ms);

#endif
