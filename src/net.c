#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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
    long long deadline = net_now_ms() + timeout_ms;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = events};
        int r = poll(&p, 1, timeout_ms);
        if (r >= 0)
            return r;
        if (errno != EINTR)
            return -1;
        /* 시그널(Ctrl+C 등)에 깨어난 것뿐이면 남은 시간만큼 다시 기다린다 */
        timeout_ms = (int)(deadline - net_now_ms());
        if (timeout_ms <= 0)
            return 0;
    }
}

int net_connect(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int err = fd < 0 ? errno : 0;
    if (fd >= 0) {
        /* 그냥 connect하면 응답 없는 상대에게 수십 초 멈춘다. non-blocking으로 걸고 poll로 기다린다 */
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            err = errno;
            if (err == EINPROGRESS) {
                socklen_t elen = sizeof err;
                int w = net_wait(fd, POLLOUT, timeout_ms);
                if (w == 0)
                    err = ETIMEDOUT;
                else if (w < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
                    err = errno;
            }
        }
        fcntl(fd, F_SETFL, flags); /* 연결된 뒤에는 다시 blocking */
    }
    freeaddrinfo(res);
    if (err) {
        if (fd >= 0)
            close(fd);
        errno = err;
        return -1;
    }
    return fd;
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
