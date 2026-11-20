#ifndef CONN_H
#define CONN_H

#include "http.h"

#include <signal.h>

#define CONN_BUF 16384

/* 연결 하나의 상태. 작업 스레드 하나가 이 연결을 끝까지 맡는다 */
struct conn {
    int fd;
    char ip[46];
    char buf[CONN_BUF]; /* 받았지만 아직 처리하지 않은 바이트 (다음 요청 앞부분일 수도 있다) */
    size_t len;
    struct request req;
    long long body_left;  /* 아직 읽지 않은 본문 바이트 수 */
    long long req_start;  /* 이번 요청의 첫 바이트가 온 시각 → read-timeout 기준 */
    int sent_continue;
    int keep;             /* 응답 후 연결을 유지할지 (Connection 헤더 값) */
};

extern volatile sig_atomic_t g_stop; /* SIGINT/SIGTERM을 받으면 1 */

/* accept로 받은 연결 하나를 keep-alive가 끝날 때까지 처리하고 닫는다 */
void conn_handle(int fd, const char *ip);

/* 남은 본문을 읽어 out_fd로 보낸다 (out_fd < 0이면 버린다).
   0 = 성공, -1 = 클라이언트 쪽 실패, -2 = out_fd 쪽 실패 */
int conn_read_body(struct conn *c, int out_fd);

#endif
