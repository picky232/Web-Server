#ifndef URI_H
#define URI_H

#include "http.h"

/* 요청 줄의 두 번째 칸(요청 대상)을 풀어낸 것. 모두 원본을 가리킨다 */
struct target {
    bool absolute; /* "http://host/..." 로 왔다 */
    bool asterisk; /* "OPTIONS *" */
    struct sl authority;
    struct sl path;  /* '/'로 시작 */
    struct sl query; /* '?' 포함. 없으면 n = 0 */
};

int uri_split(struct sl target, enum http_method m, struct target *t); /* 0 또는 400 */

/* "Example.COM:8080" → "example.com", 8080. 문법이 틀리면 400 */
int uri_host(struct sl authority, int default_port, char *host, size_t cap, int *port);

/* 경로를 하나의 모양으로 정리한다: 여러 '/'를 합치고 . 과 .. 을 풀고 %41 같은 불필요한 인코딩을 푼다.
   라우팅 판정과 백엔드 전달에 이 결과 하나만 쓴다. 둘이 다르면 "막아 둔 경로"를 돌아서 갈 수 있다.
   %2f %2e %5c %00 처럼 서버마다 다르게 읽는 것은 400 */
int uri_normalize(struct sl path, char *out, size_t cap);

/* 정리된 경로의 %XX를 풀어 파일 이름으로 (정적 파일 전용) */
bool uri_decode(const char *path, char *out, size_t cap);

#endif
