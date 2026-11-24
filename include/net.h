#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

struct addr {
    struct sockaddr_storage ss;
    socklen_t len;
};

/* "127.0.0.1:8080", "[::1]:8080", "localhost:8080" → 주소. DNS를 쓸 수 있으므로 시작할 때만 부른다 */
bool net_parse_addr(const char *s, struct addr *out);
bool net_parse_hostport(const char *host, const char *port, bool numeric, struct addr *out);

int net_listen(const struct addr *a);                 /* socket → SO_REUSEADDR → bind → listen */
int net_accept(int lfd, struct addr *peer);           /* 안 기다리는 소켓으로 돌려준다 */
int net_connect(const struct addr *a);                /* 연결을 시작만 한다. 결과는 쓰기 준비 이벤트 뒤 net_error */
int net_error(int fd);                                /* 연결 결과. 0 = 성공, 그 외 errno */
int net_pipe(int p[2]);                               /* 안 기다리는 파이프 */
int net_pipe_for_child(int p[2]);                     /* 읽는 쪽만 안 기다림. 자식이 쓰는 쪽은 평범한 파이프 */
int net_nonblock(int fd);                             /* O_NONBLOCK + FD_CLOEXEC */
void net_nodelay(int fd);

void net_ip(const struct addr *a, char *out, size_t cap);
int net_port(const struct addr *a);
int net_local_port(int fd);
bool net_is_bound_to(int fd, const struct addr *a); /* fd가 정말 그 주소에서 듣고 있나 (포트 0은 아무 포트) */
bool net_is_loopback(const struct addr *a);
bool net_is_private(const struct addr *a); /* 바깥에서 닿으면 안 되는 내부·특수 주소 */

#endif
