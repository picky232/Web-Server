#ifndef STATIC_H
#define STATIC_H

#include "conn.h"

/* 시작할 때 한 번: 문서 루트의 실제 경로(심볼릭 링크를 푼 절대 경로)를 기억해 둔다 */
int static_init(const char *root);

/* path(디코딩·검사가 끝난 "/a/b.html")에 해당하는 파일을 보낸다.
   보낸 상태 코드를 돌려주고, 연결이 끊겼으면 -1 */
int static_serve(struct conn *c, const char *path);

#endif
