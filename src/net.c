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

bool net_parse_hostport(const char *host, const char *port, bool numeric, struct addr *out)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | (numeric ? AI_NUMERICHOST : 0);
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
    return net_parse_hostport(host, colon + 1, false, out);
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

bool net_is_loopback(const struct addr *a)
{
    if (a->ss.ss_family == AF_INET6)
        return IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *)&a->ss)->sin6_addr);
    return (ntohl(((const struct sockaddr_in *)&a->ss)->sin_addr.s_addr) >> 24) == 127;
}

static bool private_v4(unsigned ip)
{
    unsigned a = ip >> 24, b = (ip >> 16) & 255;
    return a == 0 || a == 10 || a == 127 || a >= 224 || (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254) ||
           (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 198 && (b == 18 || b == 19));
}

bool net_is_private(const struct addr *a)
{
    if (a->ss.ss_family == AF_INET)
        return private_v4(ntohl(((const struct sockaddr_in *)&a->ss)->sin_addr.s_addr));
    const unsigned char *p = ((const struct sockaddr_in6 *)&a->ss)->sin6_addr.s6_addr;
    static const unsigned char zero[12];
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (!memcmp(p, mapped, 12) || !memcmp(p, zero, 12)) /* ::ffff:10.0.0.1 처럼 감싼 IPv4, ::, ::1 */
        return !memcmp(p, zero, 12) || private_v4((unsigned)p[12] << 24 | (unsigned)p[13] << 16 | (unsigned)p[14] << 8 | p[15]);
    return (p[0] & 0xfe) == 0xfc || (p[0] == 0xfe && (p[1] & 0xc0) == 0x80) || p[0] == 0xff ||
           (p[0] == 0x00 && p[1] == 0x64 && p[2] == 0xff && p[3] == 0x9b); /* IPv4로 다시 풀리는 64:ff9b:: */
}
