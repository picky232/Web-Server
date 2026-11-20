#ifndef MIME_H
#define MIME_H

/* 파일 경로의 확장자(대소문자 무시)로 Content-Type 을 돌려준다.
 * 모르는 확장자는 application/octet-stream. 반환값은 정적 문자열. */
const char *mime_for_path(const char *path);

#endif
