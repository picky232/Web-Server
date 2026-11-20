#include "server.h"
#include "cache.h"
#include "event.h"
#include "http_parser.h"
#include "mime.h"
#include "proxy_msg.h"
#include "router.h"
#include "static_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* 요청 한도 */
#define MAX_HEAD  16384            /* 요청 라인 + 헤더 */
#define MAX_URI   8192
#define MAX_BODY  (1024 * 1024)
#define RBUF_INIT 4096
#define RBUF_MAX  (MAX_HEAD + MAX_BODY)
#define FCHUNK    (64 * 1024)      /* 파일 전송 청크 */
#define WRITE_BUDGET (256 * 1024)  /* 한 이벤트에서 보낼 최대 바이트: 큰 파일이 루프를 독점하지 않게 */

/* 프록시 */
#define UHEAD_MAX   16384          /* 백엔드 응답 헤더 최대 */
#define UREAD_CHUNK 16384
#define DOWN_MS     10000          /* 연결 실패한 백엔드를 제외하는 시간 */

static volatile sig_atomic_t g_stop = 0;
static char        g_docroot[PATH_MAX];  /* realpath 로 정규화된 문서 루트 */
static app_config *g_conf;
static cache      *g_cache;   /* cache_size 가 0 이면 NULL */

void server_request_stop(void) { g_stop = 1; }

/* 이벤트 udata 의 첫 필드는 항상 kind 여야 한다 (디스패치에서 먼저 읽는다). */
enum { CK_LISTENER = 1, CK_CLIENT, CK_UPSTREAM, CK_POOLED };
enum { US_CONNECTING, US_SENDING, US_HEAD, US_BODY };

typedef struct conn  conn;
typedef struct uconn uconn;

/* 백엔드로 나가는 연결. 수명은 소유자인 클라이언트 연결(conn.up)이 관리한다. */
struct uconn {
    int       kind;
    int       fd;
    int       mask;
    conn     *client;
    upstream *up;
    int       idx;            /* 현재 시도 중인 서버 */
    unsigned  tried;          /* 시도한 서버 비트마스크 */
    int       last_timeout;   /* 마지막 실패가 타임아웃이었는지 (504 vs 502) */
    int       state;
    int       reused;         /* 풀에서 꺼낸 (이미 한 번 쓰인) 연결인가 */
    int       no_pool;        /* 이 요청은 풀을 쓰지 않는다 (stale 재시도 후 새 연결 강제) */

    char     *req;            /* 백엔드로 보낼 요청 전체 */
    size_t    req_len, req_off;

    char     *hbuf;           /* 응답 헤더 누적 */
    size_t    hlen;
    pm_resp   resp;

    pm_body_mode mode;
    size_t    remain;         /* BM_LENGTH: 남은 본문 */
    chunk_dec chunk;          /* BM_CHUNKED */

    /* 캐시: 캐시 라우트의 GET 요청일 때만 사용 */
    char     *ckey;           /* 캐시 키 (소유) */
    size_t    ckey_len;
    int       allow_store;    /* 요청이 저장을 허용하는가 */
    const char *xcache;       /* 응답에 붙일 X-Cache 값 ("MISS"/"BYPASS"), 없으면 NULL */
    cache_resp_info cinfo;
    char     *chead;          /* 저장할 응답 헤더 (소유) */
    size_t    chead_len;
    char     *cbuf;           /* 중계하면서 모으는 본문 (소유) */
    size_t    cbuf_len, cbuf_cap;
    int       cap_on;         /* 이 응답을 캐시에 저장하는 중인가 */
};

/* 리스너와 클라이언트 연결 */
struct conn {
    int    kind;
    int    fd;
    int    mask;              /* 현재 등록된 관심 이벤트 */

    char  *rbuf;              /* 누적 읽기 버퍼 (요청 시작부터) */
    size_t rlen, rcap;
    hp_parser parser;
    int    peer_fin;          /* 상대가 FIN 을 보냄 (half-close 포함) */
    char   peer[INET_ADDRSTRLEN];

    char  *wbuf;              /* 보낼 데이터 (힙) */
    size_t wlen, woff, wcap;
    int    close_after;       /* 응답 후 연결 종료 */

    int    ffd;               /* 전송 중인 파일 (없으면 -1) */
    off_t  foff;
    off_t  fremain;

    uconn *up;                /* 프록시 중인 백엔드 연결 */
    int    resp_started;      /* 클라이언트에게 응답 헤더를 이미 넘겼는가 */

    long long deadline;       /* 이 시각(ms, 단조 시계)을 넘기면 타임아웃 처리. 0 이면 없음 */

    struct conn *prev, *next;
};

static conn *g_conns = NULL;  /* 살아있는 클라이언트 연결: 종료 정리와 타임아웃 검사용 */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void list_add(conn *c) {
    c->prev = NULL;
    c->next = g_conns;
    if (g_conns) g_conns->prev = c;
    g_conns = c;
}

static void list_del(conn *c) {
    if (c->prev) c->prev->next = c->next; else g_conns = c->next;
    if (c->next) c->next->prev = c->prev;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int one = 1;
    /* 재시작 시 TIME_WAIT 상태의 포트에도 bind 할 수 있게 한다. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0)                      { perror("listen"); close(fd); return -1; }
    if (set_nonblock(fd) < 0)                           { perror("fcntl"); close(fd); return -1; }
    return fd;
}

/* ----------------------------- 연결 해제 ----------------------------- */

static void uconn_set_mask(ev_loop *loop, uconn *u, int mask) {
    if (ev_update(loop, u->fd, u->mask, mask, u) == 0) u->mask = mask;
}

static void uconn_free(ev_loop *loop, uconn *u) {
    if (u->fd >= 0) {
        if (u->mask) ev_update(loop, u->fd, u->mask, 0, u);
        close(u->fd);
    }
    if (u->client) u->client->up = NULL;
    free(u->req);
    free(u->hbuf);
    free(u->ckey);     /* 중단/실패한 응답은 캐시에 저장되지 않고 여기서 버려진다 */
    free(u->chead);
    free(u->cbuf);
    free(u);
}

/* ------------------------------ 백엔드 연결 풀 ------------------------------
 * 응답을 끝까지 정확히 읽은 keep-alive 연결을 서버별로 보관(LIFO)했다가 다음 요청에 재사용한다.
 * 보관 중에도 읽기 이벤트를 걸어 두어, 백엔드가 먼저 닫거나 예상 밖 데이터를 보내면 즉시 버린다. */

typedef struct pconn {
    int       kind;           /* CK_POOLED: 항상 첫 필드 */
    int       fd;
    int       mask;
    int       dead;           /* 풀에서 빠졌지만 같은 이벤트 배치에 stale 이벤트가 남아 있을 수 있어 해제를 미룸 */
    up_server *srv;
    long long expire;
    struct pconn *prev, *next;   /* 서버별 목록 */
    struct pconn *dnext;         /* 해제 대기 목록 */
} pconn;

static pconn *g_dead = NULL;
/* 통계: 종료 시 출력 */
static unsigned long g_st_new, g_st_reused, g_st_put, g_st_stale, g_st_evicted;

static void pool_unlink(ev_loop *loop, pconn *p) {
    if (p->prev) p->prev->next = p->next; else p->srv->idle_head = p->next;
    if (p->next) p->next->prev = p->prev;
    p->srv->idle_n--;
    if (p->mask) { ev_update(loop, p->fd, p->mask, 0, p); p->mask = 0; }
    p->dead = 1;
    p->dnext = g_dead;   /* 같은 배치의 남은 이벤트가 이 포인터를 볼 수 있으므로 배치가 끝난 뒤 해제 */
    g_dead = p;
}

static void pool_reap(void) {
    while (g_dead) {
        pconn *p = g_dead;
        g_dead = p->dnext;
        free(p);
    }
}

/* 풀에서 버린다 (연결을 닫는다) */
static void pool_discard(ev_loop *loop, pconn *p) {
    int fd = p->fd;
    pool_unlink(loop, p);
    close(fd);
    g_st_evicted++;
}

/* 가장 최근에 반납된 연결을 꺼낸다. 반환: fd (소유권이 호출자에게 넘어감), 없으면 -1 */
static int pool_take(ev_loop *loop, up_server *s) {
    pconn *p = s->idle_head;
    if (!p) return -1;
    int fd = p->fd;
    pool_unlink(loop, p);
    return fd;
}

/* 응답이 끝난 백엔드 연결을 풀에 반납한다. 성공 0 (fd 소유권이 풀로 이동), 실패 -1 (호출자가 닫는다) */
static int pool_put(ev_loop *loop, uconn *u) {
    up_server *s = &u->up->servers[u->idx];
    if (u->fd < 0 || s->idle_n >= g_conf->pool_max) return -1;
    pconn *p = calloc(1, sizeof *p);
    if (!p) return -1;
    if (u->mask) { ev_update(loop, u->fd, u->mask, 0, u); u->mask = 0; }
    if (ev_update(loop, u->fd, 0, EV_READ, p) < 0) { free(p); return -1; }
    p->kind = CK_POOLED;
    p->fd = u->fd;
    p->mask = EV_READ;
    p->srv = s;
    p->expire = now_ms() + g_conf->pool_idle_ms;
    p->next = s->idle_head;
    if (p->next) p->next->prev = p;
    s->idle_head = p;
    s->idle_n++;
    u->fd = -1;
    g_st_put++;
    return 0;
}

static void pool_sweep(ev_loop *loop, long long now) {
    for (int i = 0; i < g_conf->nups; i++)
        for (int j = 0; j < g_conf->ups[i].nservers; j++)
            for (pconn *p = g_conf->ups[i].servers[j].idle_head, *nx; p; p = nx) {
                nx = p->next;
                if (now > p->expire) pool_discard(loop, p);
            }
}

static void pool_clear(ev_loop *loop) {
    for (int i = 0; i < g_conf->nups; i++)
        for (int j = 0; j < g_conf->ups[i].nservers; j++)
            while (g_conf->ups[i].servers[j].idle_head) {
                pconn *p = g_conf->ups[i].servers[j].idle_head;
                int fd = p->fd;
                pool_unlink(loop, p);
                close(fd);
            }
}

static void conn_free(ev_loop *loop, conn *c) {
    if (c->up) uconn_free(loop, c->up);
    if (c->mask) ev_update(loop, c->fd, c->mask, 0, c);
    close(c->fd);
    if (c->ffd >= 0) close(c->ffd);
    list_del(c);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

static void conn_set_mask(ev_loop *loop, conn *c, int mask) {
    if (ev_update(loop, c->fd, c->mask, mask, c) == 0) c->mask = mask;
}

/* --------------------------------- 응답 --------------------------------- */

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* 헤더(+선택적 메모리 본문)를 만들어 쓰기 상태로 전환한다.
 *  - extra: "Name: value\r\n" 형태의 추가 헤더들 (NULL 가능)
 *  - content_length: 헤더에 적을 길이 (HEAD 여도 GET 과 같은 값)
 *  - head_only: HEAD 요청이면 본문을 보내지 않는다
 * 반환 -1: 메모리 부족으로 연결이 해제되었으니 c 를 쓰지 말 것. */
static int respond_ex(ev_loop *loop, conn *c, int status, const char *ctype, const char *extra,
                      size_t content_length, const char *body, size_t blen,
                      int keep_alive, int head_only) {
    if (!extra) extra = "";
    int hn = snprintf(NULL, 0,
                      "HTTP/1.1 %d %s\r\nServer: webserver\r\nContent-Type: %s\r\n"
                      "Content-Length: %zu\r\nConnection: %s\r\n%s\r\n",
                      status, status_text(status), ctype, content_length,
                      keep_alive ? "keep-alive" : "close", extra);
    size_t send_body = head_only ? 0 : blen;
    char *out = malloc((size_t)hn + 1 + send_body);
    if (!out) { conn_free(loop, c); return -1; }
    snprintf(out, (size_t)hn + 1,
             "HTTP/1.1 %d %s\r\nServer: webserver\r\nContent-Type: %s\r\n"
             "Content-Length: %zu\r\nConnection: %s\r\n%s\r\n",
             status, status_text(status), ctype, content_length,
             keep_alive ? "keep-alive" : "close", extra);
    if (send_body) memcpy(out + hn, body, send_body);

    free(c->wbuf);
    c->wbuf = out;
    c->wlen = (size_t)hn + send_body;
    c->wcap = (size_t)hn + 1 + send_body;
    c->woff = 0;
    c->close_after = !keep_alive;
    c->deadline = now_ms() + g_conf->idle_timeout_ms;
    conn_set_mask(loop, c, EV_WRITE);
    return 0;
}

/* 짧은 텍스트 응답 ("404 Not Found\n" 형태). 연결 유지 여부는 호출자가 결정. */
static void respond_status(ev_loop *loop, conn *c, int status, const char *extra,
                           int keep_alive, int head_only) {
    char body[96];
    int n = snprintf(body, sizeof body, "%d %s\n", status, status_text(status));
    respond_ex(loop, c, status, "text/plain", extra, (size_t)n, body, (size_t)n, keep_alive, head_only);
}

/* 프로토콜 오류: 요청 경계를 신뢰할 수 없으므로 항상 연결을 닫는다. */
static void respond_error(ev_loop *loop, conn *c, int status) {
    respond_status(loop, c, status, NULL, 0, 0);
}

/* 한 응답을 끝까지 보낸 뒤: 연결을 닫거나, 다음 요청(파이프라인 포함)을 준비한다. */
static void conn_process(ev_loop *loop, conn *c);

static void response_finished(ev_loop *loop, conn *c) {
    free(c->wbuf);
    c->wbuf = NULL;
    c->wlen = c->woff = c->wcap = 0;
    c->resp_started = 0;

    if (c->close_after) { conn_free(loop, c); return; }

    /* 처리한 요청을 버퍼 앞에서 제거하고 다음 요청을 준비 */
    size_t used = c->parser.total_len;
    if (used > c->rlen) used = c->rlen;
    memmove(c->rbuf, c->rbuf + used, c->rlen - used);
    c->rlen -= used;
    hp_init(&c->parser, MAX_HEAD, MAX_URI, MAX_BODY);

    if (c->rlen > 0) {                                /* 이미 읽어 둔 다음 요청 */
        c->deadline = now_ms() + g_conf->header_timeout_ms;
        conn_process(loop, c);
    } else if (c->peer_fin) {
        conn_free(loop, c);
    } else {
        c->deadline = now_ms() + g_conf->idle_timeout_ms;  /* keep-alive 대기 */
        conn_set_mask(loop, c, EV_READ);
    }
}

/* ------------------------------ 정적 파일 ------------------------------ */

static void serve_static(ev_loop *loop, conn *c) {
    const hp_parser *p = &c->parser;
    int ka = p->keep_alive, head = (p->method == HP_HEAD);
    const char *uri = c->rbuf + p->uri_off;

    static_target t;
    switch (static_resolve(g_docroot, uri, p->uri_len, &t)) {
    case SR_OK:        break;
    case SR_BAD:       respond_status(loop, c, 400, NULL, ka, head); return;
    case SR_FORBIDDEN: respond_status(loop, c, 403, NULL, ka, head); return;
    case SR_NOT_FOUND: respond_status(loop, c, 404, NULL, ka, head); return;
    case SR_ERROR:     respond_status(loop, c, 500, NULL, ka, head); return;
    case SR_REDIRECT: {
        /* "/sub" -> "/sub/" (쿼리는 버린다). 경로에는 CR/LF 가 없음이 파서에서 보장된다. */
        char *loc = malloc(t.path_len + 32);
        if (!loc) { respond_status(loop, c, 500, NULL, ka, head); return; }
        snprintf(loc, t.path_len + 32, "Location: %.*s/\r\n", (int)t.path_len, uri + t.path_off);
        respond_status(loop, c, 301, loc, ka, head);
        free(loc);
        return;
    }
    }

    /* O_NOFOLLOW: realpath 검사 이후 마지막 요소가 링크로 바뀌는 경쟁 상황을 줄인다. */
    int fd = open(t.file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        respond_status(loop, c, errno == EACCES ? 403 : errno == ENOENT ? 404 : 500, NULL, ka, head);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        respond_status(loop, c, 403, NULL, ka, head);
        return;
    }

    if (respond_ex(loop, c, 200, mime_for_path(t.file), NULL, (size_t)st.st_size, NULL, 0, ka, head) < 0) {
        close(fd);  /* 연결은 이미 해제됨 */
        return;
    }
    if (head || st.st_size == 0) { close(fd); return; }
    c->ffd = fd;
    c->foff = 0;
    c->fremain = st.st_size;
}

/* ------------------------------ 리버스 프록시 ------------------------------ */

/* 클라이언트에게 head + body 를 보낸다 (복사). 반환 -1: 메모리 부족으로 연결이 해제됨. */
static int client_send(ev_loop *loop, conn *c, const char *head, size_t hn, const char *body, size_t bn) {
    size_t tot = hn + bn;
    char *out = malloc(tot ? tot : 1);
    if (!out) { conn_free(loop, c); return -1; }
    if (hn) memcpy(out, head, hn);
    if (bn) memcpy(out + hn, body, bn);
    free(c->wbuf);
    c->wbuf = out;
    c->wlen = c->wcap = tot;
    c->woff = 0;
    c->deadline = now_ms() + g_conf->idle_timeout_ms;
    conn_set_mask(loop, c, EV_WRITE);
    return 0;
}

/* 세 조각(a + b + c)을 이어 붙여 클라이언트에게 보낸다. 캐시 적중 응답용: 저장된 헤더 + 새 헤더 + 본문.
 * 항목이 곧 퇴출될 수 있으므로 복사해서 보낸다. 반환 -1: 메모리 부족으로 연결이 해제됨. */
static int client_send3(ev_loop *loop, conn *c, const char *a, size_t an, const char *b, size_t bn,
                        const char *d, size_t dn) {
    size_t tot = an + bn + dn;
    char *out = malloc(tot ? tot : 1);
    if (!out) { conn_free(loop, c); return -1; }
    if (an) memcpy(out, a, an);
    if (bn) memcpy(out + an, b, bn);
    if (dn) memcpy(out + an + bn, d, dn);
    free(c->wbuf);
    c->wbuf = out;
    c->wlen = c->wcap = tot;
    c->woff = 0;
    c->deadline = now_ms() + g_conf->idle_timeout_ms;
    conn_set_mask(loop, c, EV_WRITE);
    return 0;
}

/* 캐시 적중: 백엔드에 가지 않고 저장된 응답을 바로 보낸다 */
static void serve_cache_hit(ev_loop *loop, conn *c, const cache_hit *h) {
    int ka = c->parser.keep_alive;
    char trailer[160];
    int tn = snprintf(trailer, sizeof trailer, "Age: %ld\r\nX-Cache: HIT\r\nVia: 1.1 webserver\r\nConnection: %s\r\n\r\n",
                      h->age_s, ka ? "keep-alive" : "close");
    c->close_after = !ka;
    client_send3(loop, c, h->head, h->head_len, trailer, (size_t)tn, h->body, h->body_len);
}

/* 백엔드 실패. 응답을 아직 보내지 않았으면 status 로 응답하고, 이미 보냈으면 연결을 끊는다. */
static void proxy_fail(ev_loop *loop, conn *c, int status) {
    if (c->up) uconn_free(loop, c->up);
    if (c->resp_started) { conn_free(loop, c); return; }
    respond_status(loop, c, status, NULL, 0, 0);
}

static int uconn_connect(ev_loop *loop, conn *c, uconn *u) {
    long long now = now_ms();
    for (;;) {
        int idx = upstream_pick(u->up, now, u->tried);
        if (idx < 0) return -1;
        u->tried |= 1u << idx;
        u->idx = idx;

        /* 풀에 이 서버의 유휴 연결이 있으면 재사용한다 (connect 생략). 쓰기 이벤트로 요청 전송을 시작한다. */
        if (!u->no_pool) {
            int pfd = pool_take(loop, &u->up->servers[idx]);
            if (pfd >= 0) {
                if (ev_update(loop, pfd, 0, EV_WRITE, u) == 0) {
                    u->fd = pfd;
                    u->mask = EV_WRITE;
                    u->state = US_SENDING;
                    u->reused = 1;
                    c->deadline = now + g_conf->upstream_timeout_ms;
                    g_st_reused++;
                    return 0;
                }
                close(pfd);
            }
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (set_nonblock(fd) < 0) { close(fd); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        const struct sockaddr_in *a = &u->up->servers[idx].addr;
        int rc = connect(fd, (const struct sockaddr *)a, sizeof *a);
        if (rc < 0 && errno != EINPROGRESS) {      /* 즉시 실패 (ECONNREFUSED 등) */
            close(fd);
            upstream_mark_down(u->up, idx, now + DOWN_MS);
            continue;
        }
        if (ev_update(loop, fd, 0, EV_WRITE, u) < 0) { close(fd); continue; }
        u->fd = fd;
        u->mask = EV_WRITE;
        u->state = US_CONNECTING;
        u->reused = 0;
        g_st_new++;
        c->deadline = now + g_conf->connect_timeout_ms;
        return 0;
    }
}

/* 현재 서버로의 연결이 실패(또는 타임아웃)했다. 다른 서버로 재시도하고, 없으면 502/504. */
static void uconn_retry(ev_loop *loop, conn *c, uconn *u, int timed_out) {
    upstream_mark_down(u->up, u->idx, now_ms() + DOWN_MS);
    if (u->fd >= 0) {
        if (u->mask) ev_update(loop, u->fd, u->mask, 0, u);
        close(u->fd);
        u->fd = -1;
        u->mask = 0;
    }
    u->last_timeout = timed_out;
    if (uconn_connect(loop, c, u) < 0) proxy_fail(loop, c, u->last_timeout ? 504 : 502);
}

/* 풀에서 꺼낸 연결이 이미 닫혀 있었다 (백엔드가 유휴 연결을 닫는 순간과 재사용이 겹친 경쟁 상황).
 * 응답을 한 바이트도 받기 전이고 멱등한 메서드(GET/HEAD)면 새 연결로 한 번 재시도한다.
 * POST 는 백엔드가 이미 처리했을 수 있어 재시도하지 않는다. 반환 1: 재시도를 시작함(호출자는 u 를 쓰지 말 것). */
static int upstream_stale_retry(ev_loop *loop, conn *c, uconn *u) {
    if (!u->reused || u->hlen != 0 || c->parser.method == HP_POST) return 0;
    g_st_stale++;
    if (u->fd >= 0) {
        if (u->mask) ev_update(loop, u->fd, u->mask, 0, u);
        close(u->fd);
        u->fd = -1;
        u->mask = 0;
    }
    u->reused = 0;
    u->no_pool = 1;                       /* 새 연결로 강제 */
    u->tried &= ~(1u << u->idx);          /* 같은 서버를 다시 써도 된다: 서버 장애가 아니다 */
    u->req_off = 0;
    if (uconn_connect(loop, c, u) < 0) proxy_fail(loop, c, 502);
    return 1;
}

static void upstream_send(ev_loop *loop, conn *c, uconn *u) {
    while (u->req_off < u->req_len) {
        ssize_t n = write(u->fd, u->req + u->req_off, u->req_len - u->req_off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            if (upstream_stale_retry(loop, c, u)) return;
            proxy_fail(loop, c, 502);
            return;
        }
        u->req_off += (size_t)n;
    }
    u->state = US_HEAD;
    uconn_set_mask(loop, u, EV_READ);
    c->deadline = now_ms() + g_conf->upstream_timeout_ms;
}

/* 응답을 정확히 끝까지 받았다. 연결을 다시 쓸 수 있다고 확신할 때만 풀에 반납하고, 아니면 닫는다.
 * extra: 응답이 끝난 뒤에도 읽어 둔 바이트가 남아 있다 (스트림 상태를 알 수 없음). */
static void upstream_finish(ev_loop *loop, uconn *u, int extra) {
    int reusable = g_conf->pool_max > 0 && !extra &&
                   u->mode != BM_CLOSE &&           /* 끝을 연결 종료로 알린 응답 */
                   u->resp.minor == 1 &&            /* HTTP/1.0 백엔드는 keep-alive 를 보장하지 않는다 */
                   !u->resp.conn_close;             /* 백엔드가 닫겠다고 알림 */
    if (reusable) pool_put(loop, u);                /* 실패하면 아래 uconn_free 가 닫는다 */
    uconn_free(loop, u);
}

/* 백엔드에서 읽은 본문 data[0..n) 을 응답 형식(framing)에 맞게 잘라 클라이언트에게 넘긴다.
 * head/hn 이 있으면 응답 헤더를 함께 보낸다. */
static void upstream_deliver(ev_loop *loop, conn *c, uconn *u,
                             const char *head, size_t hn, const char *data, size_t n) {
    size_t take = 0;
    int done = 0;
    switch (u->mode) {
    case BM_NONE:
        done = 1;
        break;
    case BM_LENGTH:
        take = n < u->remain ? n : u->remain;
        u->remain -= take;
        done = (u->remain == 0);
        break;
    case BM_CHUNKED: {
        size_t used = 0;
        int rc = chunk_feed(&u->chunk, data, n, &used);
        if (rc < 0) {                       /* 본문 형식 오류 */
            if (head) proxy_fail(loop, c, 502);
            else conn_free(loop, c);        /* 이미 헤더를 보냈다면 끊는 수밖에 없다 */
            return;
        }
        take = used;
        done = (rc == 1);
        break;
    }
    case BM_CLOSE:
        take = n;                           /* 끝은 백엔드의 연결 종료로 알 수 있다 */
        break;
    }

    /* 캐시 저장용으로 본문을 모은다. chunked 는 chunk 형식 그대로 모아서 꺼낼 때도 같은 형식으로 보낸다. */
    if (u->cap_on && take > 0) {
        if (u->cbuf_len + take > g_conf->cache_max_object) {          /* 길이를 모르는 chunked 가 한도를 넘음 */
            u->cap_on = 0;
        } else {
            if (u->cbuf_len + take > u->cbuf_cap) {
                size_t ncap = u->cbuf_cap ? u->cbuf_cap * 2 : 4096;
                while (ncap < u->cbuf_len + take) ncap *= 2;
                char *nb = realloc(u->cbuf, ncap);
                if (nb) { u->cbuf = nb; u->cbuf_cap = ncap; }
                else u->cap_on = 0;
            }
            if (u->cap_on) { memcpy(u->cbuf + u->cbuf_len, data, take); u->cbuf_len += take; }
        }
    }
    if (done && u->cap_on) {   /* 응답을 끝까지 정확히 받았을 때만 저장한다 (잘린/중단된 응답은 여기에 오지 않는다) */
        char vk[1024];
        if (cache_vary_key(&c->parser, c->rbuf, u->cinfo.vary, vk, sizeof vk) >= 0)
            cache_store(g_cache, u->ckey, u->ckey_len, &u->cinfo, vk, u->chead, u->chead_len,
                        u->cbuf, u->cbuf_len, now_ms());
        u->cap_on = 0;
    }

    int extra = n > take;   /* 응답 뒤에 남은 바이트가 있으면 이 연결은 재사용하지 않는다 */
    if (hn + take > 0) {
        if (client_send(loop, c, head, hn, data, take) < 0) return;
        if (done) upstream_finish(loop, u, extra);      /* 응답 완료: 풀에 반납하거나 닫는다 */
        else uconn_set_mask(loop, u, 0);                /* 클라이언트가 다 받을 때까지 읽기 중지 (역압) */
    } else if (done) {
        upstream_finish(loop, u, extra);
        response_finished(loop, c);
    }
}

static void upstream_read_head(ev_loop *loop, conn *c, uconn *u) {
    if (u->hlen >= UHEAD_MAX) { proxy_fail(loop, c, 502); return; }
    ssize_t n = read(u->fd, u->hbuf + u->hlen, UHEAD_MAX - u->hlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        if (upstream_stale_retry(loop, c, u)) return;
        proxy_fail(loop, c, 502);
        return;
    }
    if (n == 0) {                                       /* 응답 헤더를 받기 전에 백엔드가 끊음 */
        if (upstream_stale_retry(loop, c, u)) return;   /* 재사용한 연결이었다면 새 연결로 재시도 */
        proxy_fail(loop, c, 502);
        return;
    }
    u->hlen += (size_t)n;
    c->deadline = now_ms() + g_conf->upstream_timeout_ms;

    int rc = pm_resp_parse(u->hbuf, u->hlen, &u->resp);
    if (rc < 0) { proxy_fail(loop, c, 502); return; }
    if (rc == 0) {
        if (u->hlen >= UHEAD_MAX) proxy_fail(loop, c, 502);  /* 헤더가 너무 큼 */
        return;
    }
    if (u->resp.status / 100 == 1) { proxy_fail(loop, c, 502); return; }  /* 1xx 중간 응답은 지원하지 않음 */

    u->mode = pm_resp_body_mode(&u->resp, c->parser.method == HP_HEAD);
    u->remain = u->resp.content_length;
    chunk_init(&u->chunk);

    /* 클라이언트 연결 유지 여부: 요청이 keep-alive 이고, 응답 끝을 알 수 있어야 한다 */
    int ka = c->parser.keep_alive && u->mode != BM_CLOSE;

    /* 캐시 저장 여부: 응답의 끝이 확실하고(연결 종료로 끝나는 응답 제외), 정책이 허용하고, 크기 한도 안이어야 한다.
     * 실제 저장은 본문을 끝까지 받은 뒤(upstream_deliver)에만 일어난다. */
    char xbuf[40];
    const char *extra = NULL;
    if (u->xcache) {
        snprintf(xbuf, sizeof xbuf, "X-Cache: %s\r\n", u->xcache);
        extra = xbuf;
    }
    if (g_cache && u->ckey && u->allow_store && u->mode != BM_CLOSE &&
        cache_response_policy(&u->resp, u->hbuf, g_conf->cache_ttl_ms / 1000, &u->cinfo) &&
        !(u->mode == BM_LENGTH && u->resp.content_length > g_conf->cache_max_object)) {
        u->chead = pm_build_stored_head(&u->resp, u->hbuf, &u->chead_len);
        u->cap_on = (u->chead != NULL);
    }

    size_t hn;
    char *head = pm_build_client_head(&u->resp, u->hbuf, ka, extra, &hn);
    if (!head) { proxy_fail(loop, c, 502); return; }

    c->close_after = !ka;
    c->resp_started = 1;
    u->state = US_BODY;
    upstream_deliver(loop, c, u, head, hn, u->hbuf + u->resp.head_len, u->hlen - u->resp.head_len);
    free(head);
}

static void upstream_read_body(ev_loop *loop, conn *c, uconn *u) {
    char tmp[UREAD_CHUNK];
    ssize_t n = read(u->fd, tmp, sizeof tmp);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        conn_free(loop, c);                 /* 이미 헤더를 보냈으므로 클라이언트 연결을 끊는다 */
        return;
    }
    if (n == 0) {
        if (u->mode == BM_CLOSE) {          /* 연결 종료가 곧 본문의 끝 */
            uconn_free(loop, u);
            response_finished(loop, c);
        } else {
            conn_free(loop, c);             /* 약속한 길이/청크를 다 보내지 않고 끊김: 잘린 응답 */
        }
        return;
    }
    c->deadline = now_ms() + g_conf->upstream_timeout_ms;
    upstream_deliver(loop, c, u, NULL, 0, tmp, (size_t)n);
}

static void on_upstream(ev_loop *loop, uconn *u, int evmask) {
    conn *c = u->client;
    switch (u->state) {
    case US_CONNECTING: {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) err = errno;
        if (err) { uconn_retry(loop, c, u, 0); return; }
        u->state = US_SENDING;
        upstream_send(loop, c, u);
        return;
    }
    case US_SENDING: if (evmask & EV_WRITE) upstream_send(loop, c, u); return;
    case US_HEAD:    if (evmask & EV_READ)  upstream_read_head(loop, c, u); return;
    case US_BODY:    if (evmask & EV_READ)  upstream_read_body(loop, c, u); return;
    }
}

/* ckey: 캐시 키 (소유권이 넘어옴, NULL 이면 캐시 대상 아님). xcache: 응답에 붙일 X-Cache 값 */
static void proxy_start(ev_loop *loop, conn *c, const route *rt,
                        char *ckey, size_t ckey_len, int allow_store, const char *xcache) {
    size_t rl = 0;
    char *req = pm_build_request(&c->parser, c->rbuf, c->peer, rt->strip ? rt->plen : 0,
                                 g_conf->pool_max > 0, &rl);   /* 풀이 켜져 있으면 백엔드에 keep-alive 를 요청 */
    uconn *u = calloc(1, sizeof *u);
    char *hb = malloc(UHEAD_MAX);
    if (!req || !u || !hb) {
        free(req); free(u); free(hb); free(ckey);
        respond_status(loop, c, 500, NULL, 0, 0);
        return;
    }
    u->kind = CK_UPSTREAM;
    u->fd = -1;
    u->client = c;
    u->up = &g_conf->ups[rt->up];
    u->req = req;
    u->req_len = rl;
    u->hbuf = hb;
    u->ckey = ckey;
    u->ckey_len = ckey_len;
    u->allow_store = allow_store;
    u->xcache = xcache;

    c->up = u;
    c->resp_started = 0;
    conn_set_mask(loop, c, 0);   /* 프록시 중에는 클라이언트 이벤트를 받지 않는다 (응답 쓰기 때만 다시 켠다) */
    if (uconn_connect(loop, c, u) < 0) proxy_fail(loop, c, 502);
}

/* 매칭된 route 로 요청을 보낸다. 캐시 라우트면 먼저 캐시를 확인한다. */
static void route_dispatch(ev_loop *loop, conn *c, const route *rt) {
    const hp_parser *p = &c->parser;
    if (!rt->cache || !g_cache) { proxy_start(loop, c, rt, NULL, 0, 0, NULL); return; }

    char key[2048];
    int klen = cache_make_key(p, c->rbuf, key, sizeof key);
    if (klen < 0) { proxy_start(loop, c, rt, NULL, 0, 0, NULL); return; }   /* 키가 너무 긺: 캐시하지 않음 */

    if (p->method == HP_POST) {
        /* 안전하지 않은 메서드: 같은 URI 의 캐시를 무효화한다 (RFC 7234 4.4). 요청은 그대로 백엔드로 보낸다. */
        cache_invalidate(g_cache, key, (size_t)klen);
        proxy_start(loop, c, rt, NULL, 0, 0, NULL);
        return;
    }
    if (p->method != HP_GET) { proxy_start(loop, c, rt, NULL, 0, 0, NULL); return; }   /* HEAD 는 캐시를 쓰지 않음 */

    int allow_lookup, allow_store;
    cache_request_policy(p, c->rbuf, &allow_lookup, &allow_store);
    const char *x = "BYPASS";
    if (allow_lookup) {
        cache_hit h;
        if (cache_lookup(g_cache, key, (size_t)klen, p, c->rbuf, now_ms(), &h)) {
            serve_cache_hit(loop, c, &h);
            return;
        }
        x = "MISS";
    }
    char *ck = allow_store ? malloc((size_t)klen + 1) : NULL;
    if (ck) memcpy(ck, key, (size_t)klen + 1);
    proxy_start(loop, c, rt, ck, (size_t)klen, allow_store, x);
}

/* --------------------------------- 요청 처리 --------------------------------- */

static void handle_request(ev_loop *loop, conn *c) {
    const hp_parser *p = &c->parser;

    if (g_conf->nroutes > 0) {
        size_t off, plen;
        if (router_path_span(c->rbuf + p->uri_off, p->uri_len, &off, &plen) == 0) {
            const route *rt = router_match(g_conf, c->rbuf + p->uri_off + off, plen);
            if (rt) { route_dispatch(loop, c, rt); return; }
        }
    }

    if (p->method == HP_POST) {  /* 정적 리소스는 GET/HEAD 만 허용 */
        respond_status(loop, c, 405, "Allow: GET, HEAD\r\n", p->keep_alive, 0);
        return;
    }
    serve_static(loop, c);
}

/* 누적 버퍼에 대해 파서를 진행하고, 요청이 완성/오류면 응답을 시작한다. */
static void conn_process(ev_loop *loop, conn *c) {
    hp_status s = hp_feed(&c->parser, c->rbuf, c->rlen);
    switch (s) {
    case HP_NEED_MORE:
        if (c->peer_fin) conn_free(loop, c);          /* 요청이 끝나기 전에 상대가 끊음 */
        else conn_set_mask(loop, c, EV_READ);
        return;
    case HP_ERROR:
        respond_error(loop, c, c->parser.err_status);
        return;
    case HP_COMPLETE:
        handle_request(loop, c);
        return;
    }
}

static int rbuf_grow(conn *c) {
    if (c->rlen < c->rcap) return 0;
    if (c->rcap >= RBUF_MAX) return -1;
    size_t ncap = c->rcap ? c->rcap * 2 : RBUF_INIT;
    if (ncap > RBUF_MAX) ncap = RBUF_MAX;
    char *nb = realloc(c->rbuf, ncap);
    if (!nb) return -1;
    c->rbuf = nb;
    c->rcap = ncap;
    return 0;
}

static void on_readable(ev_loop *loop, conn *c) {
    size_t before = c->rlen;
    for (;;) {
        if (c->rlen == RBUF_MAX) break;               /* 상한: 파서가 판단하도록 넘긴다 */
        if (rbuf_grow(c) < 0) { respond_error(loop, c, 500); return; }
        ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n == 0) { c->peer_fin = 1; break; }       /* FIN: 이미 받은 데이터를 먼저 처리해야 한다 (half-close) */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            conn_free(loop, c);                       /* ECONNRESET 등 */
            return;
        }
        c->rlen += (size_t)n;
    }
    /* 요청의 첫 바이트가 도착한 시점부터 헤더 수신 제한시간(Slowloris 방어)을 건다. 이후 바이트로 연장하지 않는다. */
    if (before == 0 && c->rlen > 0) c->deadline = now_ms() + g_conf->header_timeout_ms;
    conn_process(loop, c);
}

/* 다음 파일 청크를 wbuf 로 읽는다. 실패(파일이 도중에 줄어듦 등) 시 -1. */
static int fill_from_file(conn *c) {
    size_t want = c->fremain < (off_t)FCHUNK ? (size_t)c->fremain : (size_t)FCHUNK;
    if (c->wcap < want) {
        char *nb = realloc(c->wbuf, FCHUNK);
        if (!nb) return -1;
        c->wbuf = nb;
        c->wcap = FCHUNK;
    }
    ssize_t n;
    do { n = pread(c->ffd, c->wbuf, want, c->foff); } while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;  /* 이미 Content-Length 를 약속했으므로 복구 불가: 연결을 끊어 클라이언트가 알게 한다 */
    c->foff += n;
    c->fremain -= n;
    c->wlen = (size_t)n;
    c->woff = 0;
    if (c->fremain == 0) { close(c->ffd); c->ffd = -1; }
    return 0;
}

static void on_writable(ev_loop *loop, conn *c) {
    size_t budget = WRITE_BUDGET;
    c->deadline = now_ms() + g_conf->idle_timeout_ms;   /* 쓰기가 진행되는 한 연장 */
    for (;;) {
        while (c->woff < c->wlen) {
            ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;  /* 다음 쓰기 이벤트까지 대기 */
                if (errno == EINTR) continue;
                conn_free(loop, c);                                   /* EPIPE 등: 클라이언트가 끊음 */
                return;
            }
            c->woff += (size_t)n;
            budget = (size_t)n >= budget ? 0 : budget - (size_t)n;
        }
        if (c->fremain <= 0) break;
        if (budget == 0) return;   /* 레벨 트리거: 다른 연결에 차례를 넘기고 곧 다시 호출된다 */
        if (fill_from_file(c) < 0) { conn_free(loop, c); return; }
    }

    if (c->up) {
        /* 프록시 응답의 일부를 모두 보냈다: 클라이언트 쓰기를 멈추고 백엔드에서 다음 조각을 읽는다 */
        c->wlen = c->woff = 0;
        conn_set_mask(loop, c, 0);
        uconn_set_mask(loop, c->up, EV_READ);
        c->deadline = now_ms() + g_conf->upstream_timeout_ms;
        return;
    }
    response_finished(loop, c);
}

static void on_accept(ev_loop *loop, conn *lc) {
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int fd = accept(lc->fd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");  /* EMFILE 등: 이번 라운드는 포기 */
            return;
        }
        if (set_nonblock(fd) < 0) { close(fd); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        conn *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->kind = CK_CLIENT;
        c->fd = fd;
        c->ffd = -1;
        if (!inet_ntop(AF_INET, &sa.sin_addr, c->peer, sizeof c->peer)) strcpy(c->peer, "unknown");
        hp_init(&c->parser, MAX_HEAD, MAX_URI, MAX_BODY);
        c->deadline = now_ms() + g_conf->idle_timeout_ms;
        if (ev_update(loop, fd, 0, EV_READ, c) < 0) { close(fd); free(c); continue; }
        c->mask = EV_READ;
        list_add(c);
    }
}

/* ------------------------------- 타임아웃 ------------------------------- */

static void conn_timeout(ev_loop *loop, conn *c) {
    uconn *u = c->up;
    if (u && !c->resp_started) {
        if (u->state == US_CONNECTING) uconn_retry(loop, c, u, 1);   /* connect 지연: 다른 서버로 */
        else proxy_fail(loop, c, 504);                               /* 백엔드가 응답하지 않음 */
        return;
    }
    conn_free(loop, c);   /* 유휴/헤더 지연/쓰기 정체/응답 도중 정체: 조용히 끊는다 */
}

static void sweep_timeouts(ev_loop *loop) {
    long long now = now_ms();
    for (conn *c = g_conns, *next; c; c = next) {
        next = c->next;
        if (c->deadline && now > c->deadline) conn_timeout(loop, c);
    }
}

/* ----------------------------------- 실행 ----------------------------------- */

int server_run(app_config *cfg) {
    g_conf = cfg;

    if (cfg->cache_max_bytes > 0) {
        g_cache = cache_create(cfg->cache_max_bytes, cfg->cache_max_object);
        if (!g_cache) { fprintf(stderr, "cannot allocate cache\n"); return -1; }
    }

    /* 문서 루트는 시작 시 한 번 정규화한다 (static_resolve 의 접두사 비교 기준). */
    if (!realpath(cfg->docroot, g_docroot)) {
        fprintf(stderr, "invalid docroot '%s': %s\n", cfg->docroot, strerror(errno));
        return -1;
    }
    struct stat dst;
    if (stat(g_docroot, &dst) < 0 || !S_ISDIR(dst.st_mode)) {
        fprintf(stderr, "docroot is not a directory: %s\n", g_docroot);
        return -1;
    }

    int lfd = listen_on(cfg->port);
    if (lfd < 0) return -1;

    ev_loop *loop = ev_create();
    if (!loop) { perror("ev_create"); close(lfd); return -1; }

    conn *lc = calloc(1, sizeof *lc);
    if (!lc) { ev_destroy(loop); close(lfd); return -1; }
    lc->kind = CK_LISTENER;
    lc->fd = lfd;
    lc->ffd = -1;
    if (ev_update(loop, lfd, 0, EV_READ, lc) < 0) {
        perror("ev_update"); free(lc); ev_destroy(loop); close(lfd); return -1;
    }
    lc->mask = EV_READ;

    fprintf(stderr, "listening on :%d (docroot=%s)\n", cfg->port, g_docroot);
    for (int i = 0; i < cfg->nroutes; i++) {
        const route *r = &cfg->routes[i];
        const upstream *u = &cfg->ups[r->up];
        fprintf(stderr, "route %s -> %s (%d server%s)%s\n", r->prefix, u->name, u->nservers,
                u->nservers == 1 ? "" : "s", r->strip ? " strip" : "");
    }

    ev_event evs[128];
    while (!g_stop) {
        int n = ev_wait(loop, evs, 128, 250);   /* 타임아웃 검사 주기를 겸한다 */
        if (n < 0) { perror("ev_wait"); break; }
        for (int i = 0; i < n; i++) {
            int kind = *(int *)evs[i].udata;
            if (kind == CK_LISTENER) { on_accept(loop, evs[i].udata); continue; }
            if (kind == CK_POOLED) {            /* 유휴 연결에 이벤트: 백엔드가 닫았거나 예상 밖 데이터 -> 버린다 */
                pconn *p = evs[i].udata;
                if (!p->dead) pool_discard(loop, p);
                continue;
            }
            if (kind == CK_UPSTREAM) {
                uconn *u = evs[i].udata;
                if (u->mask & evs[i].mask) on_upstream(loop, u, evs[i].mask);
                continue;
            }
            conn *c = evs[i].udata;
            if (c->mask & evs[i].mask & EV_READ)  { on_readable(loop, c); continue; }
            if (c->mask & evs[i].mask & EV_WRITE) { on_writable(loop, c); continue; }
        }
        pool_reap();                       /* 이번 배치에서 풀을 떠난 객체를 이제 안전하게 해제 */
        sweep_timeouts(loop);
        pool_sweep(loop, now_ms());        /* 오래 유휴인 풀 연결 정리 */
        pool_reap();
    }

    /* 종료: 남은 모든 연결(백엔드 연결, 풀 포함)과 리스너를 정리한다. */
    while (g_conns) conn_free(loop, g_conns);
    pool_clear(loop);
    pool_reap();
    fprintf(stderr, "pool: new=%lu reused=%lu returned=%lu stale_retries=%lu evicted=%lu\n",
            g_st_new, g_st_reused, g_st_put, g_st_stale, g_st_evicted);
    if (g_cache) {
        cache_stats cs;
        cache_get_stats(g_cache, &cs);
        fprintf(stderr, "cache: hits=%lu misses=%lu stores=%lu evictions=%lu expired=%lu invalidations=%lu entries=%zu bytes=%zu\n",
                cs.hits, cs.misses, cs.stores, cs.evictions, cs.expired, cs.invalidations, cs.entries, cs.bytes);
        cache_destroy(g_cache);
        g_cache = NULL;
    }
    close(lfd);
    free(lc);
    ev_destroy(loop);
    fprintf(stderr, "shutdown\n");
    return 0;
}
