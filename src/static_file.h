#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <limits.h>
#include <stddef.h>

/* 요청 URI 를 docroot 안의 실제 파일 경로로 안전하게 변환한다.
 *
 * 방어 항목:
 *  - 퍼센트 인코딩(%2e%2e 포함)을 디코딩한 "뒤에" 검사한다
 *  - '.' 으로 시작하는 모든 경로 세그먼트 거부 (".." 와 .env/.git 같은 dotfile)
 *  - NUL 바이트 거부
 *  - realpath 로 심볼릭 링크를 풀어 docroot 밖이면 거부
 */

typedef enum {
    SR_OK,          /* t->file 에 일반 파일의 절대 경로 */
    SR_REDIRECT,    /* 디렉터리인데 URI 가 '/' 로 끝나지 않음 -> 301 (t->path_off/len 으로 Location 구성) */
    SR_BAD,         /* 400: 잘못된 인코딩 등 */
    SR_FORBIDDEN,   /* 403 */
    SR_NOT_FOUND,   /* 404 */
    SR_ERROR        /* 500 */
} sr_result;

typedef struct {
    char   file[PATH_MAX];
    size_t path_off, path_len;  /* uri 안에서 쿼리/프래그먼트를 뗀 경로 구간 */
} static_target;

/* docroot 는 realpath 로 정규화된 절대 경로여야 한다. */
sr_result static_resolve(const char *docroot, const char *uri, size_t uri_len, static_target *t);

#endif
