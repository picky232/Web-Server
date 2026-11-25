#include "app.h"

#include <stdio.h>

const struct handler *const app_handler = NULL;

bool app_open(const char *arg, char *err, size_t cap)
{
    (void)arg;
    snprintf(err, cap, "route: 이 실행 파일에는 앱이 들어 있지 않습니다");
    return false;
}
