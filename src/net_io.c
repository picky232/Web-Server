/*
 * net_io.c — 짧은 읽기/쓰기와 타임아웃을 다루는 소켓 유틸리티.
 *
 * 핵심 아이디어: 소켓 자체는 블로킹 모드로 두되, recv/send 에는 MSG_DONTWAIT 를
 * 줘서 "지금 가능한 만큼만" 처리하고, 더 기다려야 하면 poll() 로 남은 시간만큼
 * 기다린다. 이렇게 하면 어떤 호출도 deadline 을 넘겨 영원히 막히지 않는다.
 */
#include "net_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int create_listen_socket(const char *bind_addr, int port, int backlog,
                         char *err, size_t errlen)
{
    /* 1) socket(): IPv4 + TCP 스트림 소켓을 만든다. 아직 주소는 없다. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket(): %s", strerror(errno));
        return -1;
    }

    /* 2) SO_REUSEADDR: 서버를 재시작할 때 TIME_WAIT 상태의 이전 연결 때문에
     *    bind() 가 EADDRINUSE 로 실패하는 것을 막는다. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        snprintf(err, errlen, "setsockopt(SO_REUSEADDR): %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 3) bind(): "이 IP:포트로 오는 연결은 내 것" 이라고 커널에 등록 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port); /* 네트워크 바이트 순서(big endian) */
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        snprintf(err, errlen, "invalid IPv4 bind address '%s'", bind_addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        snprintf(err, errlen, "bind(%s:%d): %s%s", bind_addr, port, strerror(e),
                 e == EADDRINUSE ? " (is another server already running on this port?)" : "");
        close(fd);
        return -1;
    }

    /* 4) listen(): 연결 요청을 받을 수 있는 "리스닝 소켓" 으로 전환.
     *    backlog = 커널이 accept() 전까지 보관하는 완료된 연결 수의 힌트 */
    if (listen(fd, backlog) < 0) {
        snprintf(err, errlen, "listen(): %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int wait_fd(int fd, short events, int64_t deadline_ms)
{
    for (;;) {
        int64_t remaining = deadline_ms - now_ms();
        if (remaining <= 0)
            return 0;
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int r = poll(&p, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (r > 0)
            return 1; /* POLLHUP/POLLERR 도 "준비됨" — 실제 상태는 recv/send 가 알려준다 */
        if (r == 0)
            return 0;
        if (errno == EINTR)
            continue; /* 시그널로 깨어남: 남은 시간만큼 다시 기다린다 */
        return -1;
    }
}

ssize_t recv_until_deadline(int fd, void *buf, size_t len, int64_t deadline_ms)
{
    for (;;) {
        int w = wait_fd(fd, POLLIN, deadline_ms);
        if (w == 0)
            return IO_TIMEOUT;
        if (w < 0)
            return -1;
        ssize_t n = recv(fd, buf, len, MSG_DONTWAIT);
        if (n >= 0)
            return n; /* n == 0 이면 상대가 FIN 을 보냄 (EOF) */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1; /* ECONNRESET 등 */
    }
}

int send_all(int fd, const void *buf, size_t len, int64_t deadline_ms)
{
    const char *p = buf;
    while (len > 0) {
        /* MSG_NOSIGNAL: 상대가 이미 닫은 소켓에 써도 SIGPIPE 로 죽지 않고 EPIPE 를 받는다.
         * MSG_DONTWAIT: 커널 송신 버퍼에 들어갈 만큼만 쓰고 즉시 반환 (부분 send). */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 송신 버퍼가 가득 참: 상대가 읽어서 공간이 생길 때까지 기다린다 */
            int w = wait_fd(fd, POLLOUT, deadline_ms);
            if (w == 0)
                return IO_TIMEOUT;
            if (w < 0)
                return -1;
            continue;
        }
        return -1; /* EPIPE, ECONNRESET ... */
    }
    return 0;
}

int connect_with_timeout(const struct sockaddr *addr, socklen_t addrlen,
                         int timeout_ms, int *err_out)
{
    int fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *err_out = errno;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *err_out = errno;
        close(fd);
        return -1;
    }

    int r = connect(fd, addr, addrlen);
    if (r < 0 && errno != EINPROGRESS) {
        *err_out = errno; /* 예: ECONNREFUSED (백엔드 미기동) */
        close(fd);
        return -1;
    }
    if (r < 0) {
        /* 논블로킹 connect 는 진행 중(EINPROGRESS). 쓰기 가능해지면 완료된 것. */
        int w = wait_fd(fd, POLLOUT, now_ms() + timeout_ms);
        if (w <= 0) {
            *err_out = (w == 0) ? ETIMEDOUT : errno;
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0)
            soerr = errno;
        if (soerr != 0) {
            *err_out = soerr;
            close(fd);
            return -1;
        }
    }
    /* 이후 입출력은 MSG_DONTWAIT + poll 로 하므로 블로킹 모드로 되돌려도 된다 */
    if (fcntl(fd, F_SETFL, flags) < 0) {
        *err_out = errno;
        close(fd);
        return -1;
    }
    return fd;
}

void set_tcp_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* 실패해도 기능상 문제 없음 */
}

void close_gracefully(int fd, int linger_ms)
{
    shutdown(fd, SHUT_WR); /* 우리 쪽 전송 끝(FIN). 응답은 이미 버퍼에 있다. */
    char junk[4096];
    int64_t deadline = now_ms() + linger_ms;
    size_t drained = 0;
    while (drained < 256 * 1024) {
        ssize_t n = recv_until_deadline(fd, junk, sizeof junk, deadline);
        if (n <= 0)
            break; /* 상대도 닫음(0), 오류, 또는 시간 초과 */
        drained += (size_t)n;
    }
    close(fd);
}
