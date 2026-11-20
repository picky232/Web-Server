#ifndef PROXY_H
#define PROXY_H

#include "conn.h"

/* 요청을 백엔드로 넘기고, 받은 응답을 클라이언트에게 흘려보낸다.
   보낸 상태 코드를 돌려주고, 클라이언트 연결이 끊겼으면 -1 */
int proxy_forward(struct conn *c);

#endif
