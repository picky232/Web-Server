#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int net_listen(const char *addr, int port, int *actual_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    /* 재시작할 때 TIME_WAIT 상태의 옛 연결이 포트를 잡고 있어도 bind 되게 한다 */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "잘못된 주소: %s\n", addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    /* 이 순간부터 커널이 3-way handshake를 대신 처리하고 accept 큐에 쌓는다 */
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    socklen_t len = sizeof sa;
    getsockname(fd, (struct sockaddr *)&sa, &len); /* --port 0 이면 커널이 고른 포트 */
    *actual_port = ntohs(sa.sin_port);
    return fd;
}

int net_wait(int fd, short events, int timeout_ms)
{
    struct pollfd p = {.fd = fd, .events = events};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1; /* 시그널로 깼으면 "아직 아님"으로 본다 */
    return r;
}

int net_send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        /* send는 요청보다 적게 쓰고 돌아올 수 있다 (짧은 쓰기). 남은 만큼 다시 보낸다 */
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* EPIPE·ECONNRESET(끊김) 또는 EAGAIN(SO_SNDTIMEO 시간 초과) */
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void net_close_lingering(int fd)
{
    /* 읽지 않은 데이터가 남은 채로 close하면 커널이 FIN 대신 RST를 보내서
       클라이언트가 방금 보낸 응답을 못 읽을 수 있다. 그래서 FIN을 먼저 보내고 잠깐 비운다 */
    shutdown(fd, SHUT_WR);
    char junk[4096];
    long long end = net_now_ms() + 1000;
    for (;;) {
        long long left = end - net_now_ms();
        if (left <= 0 || net_wait(fd, POLLIN, (int)left) <= 0)
            break;
        ssize_t r = recv(fd, junk, sizeof junk, 0);
        if (r <= 0)
            break;
    }
    close(fd);
}

long long net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
