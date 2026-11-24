#ifndef FORWARD_H
#define FORWARD_H

#include "exchange.h"

extern const struct handler forward_handler; /* GET http://... : 해석해서 중계 (upstream.c의 중계를 그대로 씀) */
extern const struct handler tunnel_handler;  /* CONNECT host:443 : 바이트만 그대로 중계 */

void forward_resolved(struct resolve *r); /* 이름 해석이 끝났다는 쪽지를 받은 루프가 부른다 */

#endif
