/* 프록시 통합 테스트용 백엔드 서버 (연결마다 fork, HTTP/1.1 keep-alive 지원).
 *
 *   ./backend PORT [-t IDLE_MS] [-m MAX_REQS] [-d DELAY_MS]
 *     -t  연결이 IDLE_MS 동안 유휴면 조용히 닫는다 (keep-alive timeout 이 짧은 백엔드를 흉내)
 *     -m  MAX_REQS 번째 응답을 보낸 직후, 예고 없이 연결을 닫는다 ("Connection: close" 를 알리지 않음.
 *         프록시 풀에 반납된 연결이 이미 닫혀 있는 stale 상황을 재현)
 *     -d  -m 으로 닫기 전에 DELAY_MS 만큼 연결을 유지한다 (닫히는 시점을 늦춰 경쟁 상황을 재현)
 *
 *   GET  /whoami            "backend-PORT"
 *   GET  /conn              "conn=K req=J"  (K: 이 서버가 받은 K번째 연결, J: 그 연결의 J번째 요청)
 *   GET  /headers           받은 요청 헤더를 그대로 돌려줌 (X-Forwarded-For 등 확인용)
 *   POST /echo              요청 본문을 그대로 돌려줌
 *   GET  /chunked           Transfer-Encoding: chunked 응답
 *   GET  /close             Content-Length 없이 연결 종료로 끝나는 응답
 *   GET  /big?n=N           N 바이트 본문
 *   GET  /status?code=N     N 상태 코드
 *   GET  /nocontent         204
 *   GET  /slow?ms=N         N ms 후 응답
 *   GET  /hang              요청을 받고 한참 동안 응답하지 않음
 *   GET  /truncate          Content-Length: 100 을 약속하고 10바이트만 보낸 뒤 끊음
 *   GET  /badresp           HTTP 가 아닌 응답
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int  g_port;
static long g_idle_ms = 0;   /* 0: 무제한 */
static int  g_max_reqs = 0;  /* 0: 무제한 */
static long g_close_delay = 0;  /* -m 으로 닫기 전에 기다리는 시간 */

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static int send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char *conn_hdr(int keep) { return keep ? "keep-alive" : "close"; }

static void reply(int fd, int keep, int status, const char *ctype, const char *body, size_t blen, int head_only) {
    char h[256];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
                     status, ctype, blen, conn_hdr(keep));
    send_all(fd, h, (size_t)n);
    if (!head_only) send_all(fd, body, blen);
}

static long query_num(const char *path, const char *key, long def) {
    const char *q = strchr(path, '?');
    if (!q) return def;
    size_t kl = strlen(key);
    for (q++; *q; ) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') return atol(q + kl + 1);
        q = strchr(q, '&');
        if (!q) break;
        q++;
    }
    return def;
}

/* 요청 헤더 블록 안에 "name: ...value..." 가 있는지 (대소문자 무시) */
static int header_has(const char *buf, const char *hend, const char *name, const char *value) {
    size_t nl = strlen(name);
    for (const char *p = buf; p < hend; p++) {
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, name, nl) == 0) {
            const char *eol = memchr(p, '\r', (size_t)(hend - p));
            size_t len = eol ? (size_t)(eol - p) : (size_t)(hend - p);
            for (size_t i = nl; i + strlen(value) <= len; i++)
                if (strncasecmp(p + i, value, strlen(value)) == 0) return 1;
        }
    }
    return 0;
}

/* 요청마다 달라지는 본문: 같은 본문이 두 번 오면 캐시에서 나온 것이다 */
static void uniq(char *out, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out, n, "u=%ld.%09ld pid=%d\n", (long)ts.tv_sec, ts.tv_nsec, (int)getpid());
}

/* 추가 헤더(extra: "Name: v\r\n" 들)를 붙인 응답 */
static void reply_h(int fd, int keep, int status, const char *extra, const char *body, size_t blen, int head_only) {
    char h[512];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d X\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n%sConnection: %s\r\n\r\n",
                     status, blen, extra, conn_hdr(keep));
    send_all(fd, h, (size_t)n);
    if (!head_only) send_all(fd, body, blen);
}

/* 요청 헤더 값 (소문자 이름 + ':' 로 검색). 없으면 빈 문자열 */
static void get_header(const char *buf, const char *hend, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    size_t nl = strlen(name);
    for (const char *p = buf; p < hend; p++) {
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, name, nl) == 0) {
            const char *v = p + nl;
            while (*v == ' ') v++;
            const char *e = memchr(v, '\r', (size_t)(hend - v));
            size_t l = e ? (size_t)(e - v) : (size_t)(hend - v);
            if (l >= cap) l = cap - 1;
            memcpy(out, v, l);
            out[l] = '\0';
            return;
        }
    }
}

/* /c-* : 캐시 정책 테스트용 엔드포인트. 반환 1: 처리함 */
static int handle_cache_endpoint(int fd, int keep, const char *path, const char *buf, const char *hend, int head_only, int *ok) {
    if (strncmp(path, "/c-", 3) != 0) return 0;
    const char *k = path + 3;
    char u[96], body[256];
    uniq(u, sizeof u);

    if (strncmp(k, "maxage", 6) == 0) {
        char h[64];
        snprintf(h, sizeof h, "Cache-Control: max-age=%ld\r\n", query_num(path, "n", 60));
        reply_h(fd, keep, 200, h, u, strlen(u), head_only);
    } else if (strncmp(k, "nostore", 7) == 0) {
        reply_h(fd, keep, 200, "Cache-Control: no-store\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "private", 7) == 0) {
        reply_h(fd, keep, 200, "Cache-Control: private, max-age=60\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "none", 4) == 0) {
        reply_h(fd, keep, 200, "", u, strlen(u), head_only);                       /* 신선도 정보 없음: 기본 TTL */
    } else if (strncmp(k, "cookie", 6) == 0) {
        reply_h(fd, keep, 200, "Cache-Control: max-age=60\r\nSet-Cookie: sid=1\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "trunc", 5) == 0) {          /* max-age 가 있지만 약속한 길이를 다 보내지 않고 끊음 */
        const char *h = "HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nContent-Length: 100\r\nConnection: close\r\n\r\n";
        send_all(fd, h, strlen(h));
        send_all(fd, "0123456789", 10);
        *ok = 0;
    } else if (strncmp(k, "smax", 4) == 0) {
        reply_h(fd, keep, 200, "Cache-Control: max-age=0, s-maxage=60\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "expires", 7) == 0) {
        reply_h(fd, keep, 200, "Expires: Thu, 01 Jan 2099 00:00:00 GMT\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "vary", 4) == 0) {
        char lang[64];
        get_header(buf, hend, "accept-language:", lang, sizeof lang);
        int n = snprintf(body, sizeof body, "%slang=%s\n", u, lang);
        reply_h(fd, keep, 200, "Cache-Control: max-age=60\r\nVary: Accept-Language\r\n", body, (size_t)n, head_only);
    } else if (strncmp(k, "404none", 7) == 0) {
        reply_h(fd, keep, 404, "", u, strlen(u), head_only);
    } else if (strncmp(k, "404", 3) == 0) {
        reply_h(fd, keep, 404, "Cache-Control: max-age=60\r\n", u, strlen(u), head_only);
    } else if (strncmp(k, "chunked", 7) == 0) {
        char h[200];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: max-age=60\r\n"
                                       "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n", conn_hdr(keep));
        send_all(fd, h, (size_t)hn);
        if (!head_only) {
            char c1[160];
            int cn = snprintf(c1, sizeof c1, "%zx\r\n%s\r\n", strlen(u), u);
            send_all(fd, c1, (size_t)cn);
            sleep_ms(30);
            send_all(fd, "6\r\n world\r\n0\r\n\r\n", 16);
        }
    } else if (strncmp(k, "big", 3) == 0) {
        long n = query_num(path, "n", 1000);
        char h[200];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: max-age=60\r\n"
                                       "Content-Length: %ld\r\nConnection: %s\r\n\r\n", n, conn_hdr(keep));
        send_all(fd, h, (size_t)hn);
        if (!head_only) {
            size_t ul = strlen(u);
            long left = n;
            if (left >= (long)ul) { send_all(fd, u, ul); left -= (long)ul; }
            char chunk[8192];
            memset(chunk, 'x', sizeof chunk);
            while (left > 0) {
                size_t t = left < (long)sizeof chunk ? (size_t)left : sizeof chunk;
                if (send_all(fd, chunk, t) < 0) { *ok = 0; break; }
                left -= (long)t;
            }
        }
    } else {
        return 0;
    }
    return 1;
}

/* 요청 하나를 처리한다. 반환 1: 연결 유지 가능, 0: 닫아야 함 */
static int handle_one(int fd, long conn_no, int req_no, int *silent_close) {
    char buf[65536];
    size_t len = 0;
    char *hend = NULL;
    while (!hend && len < sizeof buf - 1) {
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0) return 0;
        len += (size_t)n;
        buf[len] = '\0';
        hend = strstr(buf, "\r\n\r\n");
    }
    if (!hend) return 0;
    size_t head_len = (size_t)(hend - buf) + 4;

    char method[16], path[1024], version[16];
    if (sscanf(buf, "%15s %1023s %15s", method, path, version) != 3) return 0;
    int head_only = strcmp(method, "HEAD") == 0;

    int keep = strcmp(version, "HTTP/1.1") == 0 && !header_has(buf, hend, "connection:", "close");
    if (g_max_reqs && req_no >= g_max_reqs) *silent_close = 1;  /* keep-alive 라고 알리고 조용히 닫는다 */

    size_t cl = 0;
    for (char *p = buf; p < hend; p++)
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, "content-length:", 15) == 0) cl = (size_t)atol(p + 15);
    char *body = malloc(cl + 1);
    if (!body) return 0;
    size_t have = len - head_len;
    if (have > cl) have = cl;
    memcpy(body, buf + head_len, have);
    while (have < cl) {
        ssize_t n = read(fd, body + have, cl - have);
        if (n <= 0) { free(body); return 0; }
        have += (size_t)n;
    }

    int ok = keep;  /* 응답 후 연결을 이어갈 수 있는가 */
    if (strncmp(path, "/whoami", 7) == 0) {
        char b[64];
        int n = snprintf(b, sizeof b, "backend-%d\n", g_port);
        reply(fd, keep, 200, "text/plain", b, (size_t)n, head_only);
    } else if (strncmp(path, "/conn", 5) == 0) {
        char b[64];
        int n = snprintf(b, sizeof b, "conn=%ld req=%d\n", conn_no, req_no);
        reply(fd, keep, 200, "text/plain", b, (size_t)n, head_only);
    } else if (strncmp(path, "/headers", 8) == 0) {
        reply(fd, keep, 200, "text/plain", buf, head_len, head_only);
    } else if (strncmp(path, "/echo", 5) == 0) {
        reply(fd, keep, 200, "application/octet-stream", body, cl, head_only);
    } else if (strncmp(path, "/chunked", 8) == 0) {
        char h[160];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: %s\r\n\r\n", conn_hdr(keep));
        send_all(fd, h, (size_t)hn);
        if (!head_only) {
            send_all(fd, "5\r\nhello\r\n", 10);
            sleep_ms(50);  /* 청크 경계가 여러 read 로 나뉘는 상황을 만든다 */
            send_all(fd, "6\r\n world\r\n", 11);
            send_all(fd, "0\r\nX-Trailer: t\r\n\r\n", 20);
        }
    } else if (strncmp(path, "/close", 6) == 0) {
        const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
        send_all(fd, h, strlen(h));
        if (!head_only) send_all(fd, "close-delimited-body\n", 21);
        ok = 0;
    } else if (strncmp(path, "/big", 4) == 0) {
        long n = query_num(path, "n", 1024);
        char h[200];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %ld\r\nConnection: %s\r\n\r\n", n, conn_hdr(keep));
        send_all(fd, h, (size_t)hn);
        if (!head_only) {
            char chunk[8192];
            memset(chunk, 'x', sizeof chunk);
            for (long left = n; left > 0; ) {
                size_t t = left < (long)sizeof chunk ? (size_t)left : sizeof chunk;
                if (send_all(fd, chunk, t) < 0) { ok = 0; break; }
                left -= (long)t;
            }
        }
    } else if (strncmp(path, "/status", 7) == 0) {
        long code = query_num(path, "code", 200);
        char b[32];
        int n = snprintf(b, sizeof b, "status %ld\n", code);
        reply(fd, keep, (int)code, "text/plain", b, (size_t)n, head_only);
    } else if (strncmp(path, "/nocontent", 10) == 0) {
        char h[128];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 204 No Content\r\nConnection: %s\r\n\r\n", conn_hdr(keep));
        send_all(fd, h, (size_t)hn);
    } else if (strncmp(path, "/slow", 5) == 0) {
        sleep_ms(query_num(path, "ms", 1000));
        reply(fd, keep, 200, "text/plain", "slow-done\n", 10, head_only);
    } else if (strncmp(path, "/hang", 5) == 0) {
        sleep_ms(8000);
        ok = 0;
    } else if (strncmp(path, "/truncate", 9) == 0) {
        const char *h = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n";
        send_all(fd, h, strlen(h));
        send_all(fd, "0123456789", 10);
        ok = 0;
    } else if (strncmp(path, "/badresp", 8) == 0) {
        send_all(fd, "NOT HTTP AT ALL\r\n\r\n", 19);
        ok = 0;
    } else if (handle_cache_endpoint(fd, keep, path, buf, hend, head_only, &ok)) {
        /* /c-* 캐시 테스트 엔드포인트 */
    } else {
        reply(fd, keep, 404, "text/plain", "not found\n", 10, head_only);
    }
    free(body);
    return ok;
}

static void serve_conn(int fd, long conn_no) {
    int silent_close = 0;
    for (int req = 1; ; req++) {
        if (g_idle_ms > 0) {  /* 유휴 시간 초과 시 예고 없이 닫는다 */
            struct pollfd p = { fd, POLLIN, 0 };
            int rc = poll(&p, 1, (int)g_idle_ms);
            if (rc <= 0) return;
        }
        if (!handle_one(fd, conn_no, req, &silent_close)) return;
        if (silent_close) {
            if (g_close_delay > 0) sleep_ms(g_close_delay);  /* 응답 후 잠시 유지하다 닫는다 */
            return;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s PORT [-t IDLE_MS] [-m MAX_REQS]\n", argv[0]); return 2; }
    g_port = atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) g_idle_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) g_max_reqs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) g_close_delay = atol(argv[++i]);
        else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);  /* 자식 자동 회수 */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    a.sin_port = htons((unsigned short)g_port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 128) < 0) { perror("listen"); return 1; }

    long conn_no = 0;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); return 1; }
        conn_no++;  /* fork 시점의 값이 자식에게 복사되므로 자식은 자기 연결 번호를 알 수 있다 */
        pid_t pid = fork();
        if (pid == 0) { close(lfd); serve_conn(fd, conn_no); close(fd); _exit(0); }
        close(fd);
    }
}
