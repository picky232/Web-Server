#ifndef ROUTER_H
#define ROUTER_H

/* path가 프록시 접두사(기본 /api) 아래인지. "/api", "/api/..."는 예, "/apix"는 아니오 */
int router_is_proxy(const char *path);

/* 이번 요청을 보낼 백엔드 번호 (Round-Robin). 백엔드가 없으면 -1 */
int router_next_backend(void);

#endif
