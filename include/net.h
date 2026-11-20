#ifndef NET_H
#define NET_H

#include <stddef.h>

/* socket → setsockopt(SO_REUSEADDR) → bind → listen. 실제로 열린 포트를 *actual_port에 적는다 */
int net_listen(const char *addr, int port, int *actual_port);

/* fd가 읽기/쓰기 가능해질 때까지 최대 timeout_ms 기다린다. 1 = 준비됨, 0 = 시간 초과, -1 = 에러 */
int net_wait(int fd, short events, int timeout_ms);

/* 백엔드에 연결한다. timeout_ms 안에 안 되면 -1 (errno = ETIMEDOUT), 거절되면 -1 (ECONNREFUSED 등) */
int net_connect(const char *host, const char *port, int timeout_ms);

/* n바이트를 전부 쓸 때까지 send를 반복한다. 0 = 성공, -1 = 연결이 끊김/시간 초과 */
int net_send_all(int fd, const void *buf, size_t n);

/* 응답을 다 쓴 뒤 닫기: 쓰기 방향만 먼저 닫고, 클라이언트가 보낸 남은 바이트를 잠깐 버린 뒤 close */
void net_close_lingering(int fd);

/* 단조 증가 시계 (밀리초). 제한 시간 계산용 */
long long net_now_ms(void);

#endif
