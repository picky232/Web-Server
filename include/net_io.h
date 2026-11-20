/*
 * net_io.h — 소켓 입출력의 "바이트 스트림" 처리를 한곳에 모은 모듈.
 *
 * TCP는 메시지 경계가 없다. recv() 한 번이 요청 전체를 주지 않을 수 있고,
 * send() 한 번이 버퍼 전체를 보내지 않을 수 있다. 이 파일의 함수들은
 * 그 사실을 전제로 "마감 시각(deadline)까지 반복"하는 루프를 제공한다.
 *
 * 모든 시간은 CLOCK_MONOTONIC 기준 밀리초(ms)다.
 */
#ifndef NET_IO_H
#define NET_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/* recv/send 계열 함수가 마감 시각을 넘겼을 때 돌려주는 값 */
#define IO_TIMEOUT (-2)

/* 단조 증가 시계의 현재 시각 (ms). 벽시계 변경에 영향받지 않는다. */
int64_t now_ms(void);

/*
 * socket() → setsockopt(SO_REUSEADDR) → bind() → listen() 을 수행한다.
 * 성공하면 리스닝 소켓 fd, 실패하면 -1 과 함께 err 에 errno 기반 설명.
 */
int create_listen_socket(const char *bind_addr, int port, int backlog,
                         char *err, size_t errlen);

/* fd 가 events(POLLIN/POLLOUT)를 만족할 때까지 deadline 까지 기다린다.
 * 반환: 1 준비됨, 0 시간 초과, -1 오류 */
int wait_fd(int fd, short events, int64_t deadline_ms);

/* 최대 len 바이트를 한 번 읽는다(짧게 읽힐 수 있음).
 * 반환: >0 읽은 바이트, 0 상대가 연결을 닫음(EOF), -1 오류, IO_TIMEOUT */
ssize_t recv_until_deadline(int fd, void *buf, size_t len, int64_t deadline_ms);

/* len 바이트를 "전부" 보낼 때까지 반복한다(부분 send 처리).
 * 반환: 0 성공, -1 오류(EPIPE 등), IO_TIMEOUT */
int send_all(int fd, const void *buf, size_t len, int64_t deadline_ms);

/* 논블로킹 connect + poll 로 타임아웃이 있는 연결.
 * 성공하면 연결된 fd, 실패하면 -1 이고 *err_out 에 errno(시간 초과는 ETIMEDOUT). */
int connect_with_timeout(const struct sockaddr *addr, socklen_t addrlen,
                         int timeout_ms, int *err_out);

/*
 * 오류 응답을 보낸 뒤 연결을 닫는 "lingering close".
 * 읽지 않은 데이터가 남은 소켓을 바로 close() 하면 커널이 RST 를 보내서
 * 클라이언트가 방금 보낸 오류 응답을 못 읽을 수 있다. 그래서
 * shutdown(SHUT_WR) 으로 FIN 을 먼저 보내고, 남은 입력을 잠깐 버린 뒤 닫는다.
 */
void close_gracefully(int fd, int linger_ms);

/* TCP_NODELAY: 작은 send() 들을 모으느라 지연시키는 Nagle 알고리즘을 끈다. */
void set_tcp_nodelay(int fd);

#endif /* NET_IO_H */
