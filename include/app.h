#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stddef.h>

struct handler;

/* 애플리케이션을 끼우는 자리 (route ... app 대상).
   보통 빌드는 비어 있는 src/app.c를, 데모 빌드는 그 자리에 demo/seats.c를 링크한다 */
extern const struct handler *const app_handler;        /* NULL = 앱이 없는 빌드 */
bool app_open(const char *arg, char *err, size_t cap); /* 설정을 읽을 때 한 번. arg는 route에 적은 대상 */

#endif
