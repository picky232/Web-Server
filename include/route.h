#ifndef ROUTE_H
#define ROUTE_H

#include "config.h"

/* 호스트가 정확히 일치하는 규칙 먼저, 없으면 "*" 규칙. 그 안에서는 가장 긴 경로 접두사.
   접두사는 조각 경계에서만 맞는다: /api는 /api, /api/x에 맞고 /apix에는 안 맞는다 */
const struct route *route_find(const struct config *c, const char *host, const char *path);

#endif
