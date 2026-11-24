#ifndef STATIC_H
#define STATIC_H

#include "exchange.h"

#include <sys/stat.h>

extern const struct handler static_handler;

int status_from_errno(int e); /* 파일을 열지 못한 이유 → 상태 코드 */

/* 루트 폴더에서 시작해 경로 조각을 하나씩 연다. 심볼릭 링크는 전부 거절. 실패하면 -1과 errno */
int open_under(int root_fd, const char *rel, struct stat *st);
bool etag_matches(struct sl header, const char *etag); /* If-None-Match */

#endif
