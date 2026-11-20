/*
 * static_files.h — document root 아래의 파일을 GET/HEAD 로 돌려준다.
 */
#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"
#include "http_response.h"

/*
 * 요청 target("/a/../b%20c.html?x=1")을 안전한 상대 경로("/b c.html")로 바꾼다.
 *   1) '?' 와 '#' 뒤는 버린다 (query/fragment)
 *   2) %XX 를 한 번만 디코딩한다 (잘못된 %XX, %00 → 400)
 *   3) '/' 로 나눠 "." 은 버리고 ".." 은 한 단계 위로. 루트 위로 가려 하면 403
 *   4) allow_hidden=false 이면 '.' 으로 시작하는 이름(.git, .env)은 403
 * 원래 경로가 '/' 로 끝나면 결과도 '/' 로 끝난다 (디렉터리 요청 표시).
 * 반환: 0 성공, 400/403/414 오류 코드.
 */
int static_normalize_target(const char *target, char *out, size_t outsz,
                            bool allow_hidden);

/* 확장자로 Content-Type 을 고른다 (대소문자 무시). 모르면 application/octet-stream */
const char *mime_type_for_path(const char *path);

/* resolved 가 root 와 같거나 root 아래에 있는가 (둘 다 realpath 결과여야 함) */
bool path_is_within(const char *root, const char *resolved);

/* 정적 파일 요청 1개를 처리하고 응답까지 보낸다. root_real 은 realpath() 된 루트. */
response_info_t static_serve(int fd, const char *root_real, const http_request_t *req,
                             bool keep_alive, int send_timeout_ms);

#endif /* STATIC_FILES_H */
