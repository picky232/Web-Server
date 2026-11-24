#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int net_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC); /* CGI 자식 프로세스가 우리 소켓을 물려받지 않게 */
}

void net_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static bool resolve(const char *host, const char *port, struct addr *out)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return false;
    memset(out, 0, sizeof *out);
    memcpy(&out->ss, res->ai_addr, res->ai_addrlen);
    out->len = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

bool net_parse_addr(const char *s, struct addr *out)
{
    char host[256];
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s || !colon[1])
        return false;
    size_t n = (size_t)(colon - s);
    if (s[0] == '[' && s[n - 1] == ']') { /* IPv6는 [::1]:8080 */
        s++;
        n -= 2;
    }
    if (n == 0 || n >= sizeof host)
        return false;
    memcpy(host, s, n);
    host[n] = '\0';
    return resolve(host, colon + 1, out);
}

int net_listen(const struct addr *a)
{
    int fd = socket(a->ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    /* 방금 닫은 연결이 TIME_WAIT로 남아 있어도 다시 bind할 수 있게 한다.
       없으면 서버를 재시작할 때 "Address already in use"로 실패한다 */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (net_nonblock(fd) < 0 || bind(fd, (const struct sockaddr *)&a->ss, a->len) < 0 ||
        /* 이 순간부터 handshake는 커널이 혼자 끝내고, 완성된 연결을 대기열에 쌓아 둔다 */
        listen(fd, 1024) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int net_accept(int lfd, struct addr *peer)
{
    peer->len = sizeof peer->ss;
#ifdef __linux__
    return accept4(lfd, (struct sockaddr *)&peer->ss, &peer->len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = accept(lfd, (struct sockaddr *)&peer->ss, &peer->len);
    if (fd >= 0 && net_nonblock(fd) < 0) {
        close(fd);
        errno = ECONNABORTED;
        return -1;
    }
    return fd;
#endif
}

int net_connect(const struct addr *a)
{
#ifdef __linux__
    int fd = socket(a->ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    int fd = socket(a->ss.ss_family, SOCK_STREAM, 0);
#endif
    if (fd < 0)
        return -1;
    if (net_nonblock(fd) < 0 ||
        (connect(fd, (const struct sockaddr *)&a->ss, a->len) < 0 && errno != EINPROGRESS)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    net_nodelay(fd);
    return fd;
}

int net_error(int fd)
{
    int err = 0;
    socklen_t len = sizeof err;
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 ? errno : err;
}

int net_pipe(int p[2])
{
    if (pipe(p) < 0)
        return -1;
    if (net_nonblock(p[0]) < 0 || net_nonblock(p[1]) < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    return 0;
}

int net_pipe_for_child(int p[2])
{
#ifdef __linux__
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
#else
    if (pipe(p) < 0)
        return -1;
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
#endif
    return net_nonblock(p[0]);
}

void net_ip(const struct addr *a, char *out, size_t cap)
{
    const void *src = a->ss.ss_family == AF_INET6
        ? (const void *)&((const struct sockaddr_in6 *)&a->ss)->sin6_addr
        : (const void *)&((const struct sockaddr_in *)&a->ss)->sin_addr;
    if (!inet_ntop(a->ss.ss_family, src, out, (socklen_t)cap))
        snprintf(out, cap, "?");
}

int net_port(const struct addr *a)
{
    return ntohs(a->ss.ss_family == AF_INET6 ? ((const struct sockaddr_in6 *)&a->ss)->sin6_port
                                             : ((const struct sockaddr_in *)&a->ss)->sin_port);
}

int net_local_port(int fd)
{
    struct addr a = {.len = sizeof a.ss};
    return getsockname(fd, (struct sockaddr *)&a.ss, &a.len) < 0 ? 0 : net_port(&a);
}

bool net_is_bound_to(int fd, const struct addr *a)
{
    struct addr b = {.len = sizeof b.ss};
    char x[64], y[64];
    if (getsockname(fd, (struct sockaddr *)&b.ss, &b.len) < 0 || a->ss.ss_family != b.ss.ss_family)
        return false;
    net_ip(a, x, sizeof x);
    net_ip(&b, y, sizeof y);
    return !strcmp(x, y) && (net_port(a) == 0 || net_port(a) == net_port(&b));
}
