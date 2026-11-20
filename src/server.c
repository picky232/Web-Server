/*
 * server.c — 서버의 뼈대. 학습은 이 파일의 server_run() 에서 시작하면 된다.
 *
 *   main 스레드:  listen 소켓 → poll → accept → tp_submit(fd)   (절대 요청을 처리하지 않음)
 *   worker 스레드: cq_pop(fd) → handle_connection(fd)
 *                    └ (keep-alive 동안 반복)
 *                       http_read_request → dispatch → static_serve | proxy_forward
 *                    └ close(fd)
 *
 * 종료(SIGINT/SIGTERM): 시그널 핸들러는 g_stop 플래그만 세운다.
 *   accept 루프가 이를 보고 빠져나와 listen fd 를 닫고, tp_shutdown 이 worker 를
 *   join 한 뒤 대기열에 남은 fd 에 503 을 보내고 닫는다.
 */
#include "server.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "http_conn.h"
#include "http_response.h"
#include "log.h"
#include "net_io.h"
#include "proxy.h"
#include "router.h"
#include "static_files.h"
#include "thread_pool.h"

/* 시그널 핸들러와 모든 스레드가 공유하는 종료 플래그.
 * lock-free atomic 은 시그널 핸들러 안에서 써도 안전하다. */
static atomic_bool g_stop = false;

typedef struct {
    const server_config_t *cfg;
    char root_real[PATH_MAX];  /* realpath() 된 document root */
    http_limits_t limits;
    proxy_t proxy;
    thread_pool_t pool;
} server_t;

static void on_stop_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, true); /* 핸들러에서는 플래그만: async-signal-safe */
}

/* 큐가 가득 찼거나 종료 중이라 처리할 수 없는 연결: 503 을 짧게 시도하고 닫는다.
 * accept 루프를 오래 막지 않도록 송신 대기는 최대 50ms. */
static void reject_connection(int fd, void *ctx)
{
    (void)ctx;
    http_send_error(fd, 503, false, false, "Retry-After: 1\r\n", now_ms() + 50);
    shutdown(fd, SHUT_WR);
    close(fd);
}

/*
 * keep-alive 연결에서 다음 요청을 기다린다. true 면 읽을 데이터(또는 EOF)가 있음.
 * 200ms 마다 깨어나서 다음을 확인한다:
 *   - 서버 종료 중인가                → 닫는다
 *   - 대기열에 다른 연결이 기다리는가 → 노는 이 연결을 닫고 worker 를 양보한다
 */
static bool wait_for_next_request(server_t *srv, int fd)
{
    int64_t deadline = now_ms() + srv->cfg->keepalive_timeout_ms;
    /* 첫 조각은 100ms: 클라이언트가 응답을 읽고 바로 다음 요청을 보내는 데 걸리는
     * 시간만큼은 양보하지 않고 기다려 준다 (너무 짧으면 보내는 중인 요청을 끊게 됨). */
    int64_t slice_ms = 100;
    for (;;) {
        int64_t now = now_ms();
        int64_t slice_end = now + slice_ms < deadline ? now + slice_ms : deadline;
        int w = wait_fd(fd, POLLIN, slice_end);
        if (w == 1)
            return true; /* 데이터(또는 EOF) 도착: 읽기 단계에서 판단 */
        if (w < 0)
            return false;
        /* 읽을 것이 없을 때만 양보/종료를 판단한다. 이미 도착한 요청을 버리지 않기 위해
         * "데이터 확인 → 그다음 대기열 확인" 순서가 중요하다. */
        if (atomic_load(&g_stop) || tp_pending(&srv->pool) > 0 || now_ms() >= deadline)
            return false;
        slice_ms = 200;
    }
}

/* 라우팅 + 메서드 검사 후 정적 파일 또는 프록시로 보낸다 */
static response_info_t dispatch(server_t *srv, http_conn_t *conn, const http_request_t *req,
                                bool keep_alive)
{
    route_kind_t route = router_route(srv->cfg, req->target);
    const char *allow = NULL;
    if (!router_method_allowed(route, req->method, &allow)) {
        char extra[64];
        snprintf(extra, sizeof extra, "Allow: %s\r\n", allow);
        response_info_t info = {.status = 405, .body_bytes = 0, .keep_open = keep_alive};
        if (http_send_error(conn->fd, 405, keep_alive, req->method == HTTP_METHOD_HEAD,
                            extra, now_ms() + srv->cfg->read_timeout_ms) != 0)
            info.keep_open = false;
        return info;
    }
    if (route == ROUTE_PROXY)
        return proxy_forward(&srv->proxy, conn, req, keep_alive, srv->cfg->read_timeout_ms);
    return static_serve(conn->fd, srv->root_real, req, keep_alive, srv->cfg->read_timeout_ms);
}

/*
 * worker 스레드가 연결 소켓 하나를 처리하는 함수 (thread pool 의 handler).
 * 이 함수가 fd 의 주인이다: 어떤 경로로 끝나든 정확히 한 번 닫는다.
 */
static void handle_connection(int fd, void *arg)
{
    server_t *srv = arg;
    http_conn_t conn; /* 연결별 수신 버퍼 (worker 스택 위, 다른 스레드와 공유 안 함) */
    http_conn_init(&conn, fd);
    /* 응답 head 와 body 를 따로 send() 하므로, Nagle 알고리즘이 켜져 있으면
     * 상대의 delayed ACK(~40ms)를 기다리며 body 가 묶인다. 작은 응답이 많은
     * 웹 서버에서는 꺼 두는 것이 보통이다. */
    set_tcp_nodelay(fd);

    for (int served = 0;; served++) {
        /* 두 번째 요청부터는 keep-alive 대기. 이미 버퍼에 다음 요청이 있으면 바로 처리 */
        if (served > 0 && conn.len == 0 && !wait_for_next_request(srv, fd))
            break;

        int64_t started = now_ms();
        http_request_t req;
        int err_status = 0;
        read_result_t rr = http_read_request(&conn, &req, &srv->limits, &err_status);

        if (rr == READ_CLOSED || rr == READ_ABORTED) {
            http_request_free(&req);
            break; /* 응답할 대상이 없음: 그냥 닫는다 */
        }
        if (rr == READ_BAD) {
            /* 파싱 오류/시간 초과/크기 초과: 스트림 위치를 믿을 수 없으므로 닫는다 */
            http_send_error(fd, err_status, false, false, NULL,
                            now_ms() + srv->cfg->read_timeout_ms);
            log_access("%s \"%s %.200s\" %d - %lldms (rejected)", conn.client_ip,
                       req.method_str ? req.method_str : "-",
                       req.target ? req.target : "-", err_status,
                       (long long)(now_ms() - started));
            http_request_free(&req);
            close_gracefully(fd, 500);
            return;
        }

        /* 연결을 유지할지 결정: 클라이언트 희망 + 종료 중 아님 + 요청 수 한도 + 대기열 없음 */
        bool keep_alive = req.keep_alive && !atomic_load(&g_stop) &&
                          served + 1 < srv->cfg->max_requests_per_conn &&
                          tp_pending(&srv->pool) == 0;

        response_info_t info = dispatch(srv, &conn, &req, keep_alive);

        log_access("%s \"%s %.200s HTTP/1.%d\" %d %llu %lldms", conn.client_ip,
                   req.method_str, req.target, req.version_minor, info.status,
                   info.body_bytes, (long long)(now_ms() - started));

        http_request_free(&req);
        http_conn_consume(&conn); /* 다음(파이프라인) 요청 바이트를 버퍼 앞으로 */
        if (!info.keep_open)
            break;
    }
    close(fd);
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음: poll() 이 EINTR 로 즉시 깨어나게 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    /* 끊긴 소켓에 write 하면 기본 동작이 "프로세스 종료"인 SIGPIPE 가 온다.
     * 무시하면 send() 가 EPIPE 를 돌려주고, 우리는 그 연결만 정리한다.
     * (send 에도 MSG_NOSIGNAL 을 주므로 이중 안전장치) */
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    return sigaction(SIGPIPE, &ign, NULL);
}

/* worker 를 만들 때 SIGINT/SIGTERM 을 막아 두면, 새 스레드가 그 마스크를 물려받아
 * 시그널은 항상 main 스레드(accept 루프)로만 배달된다. */
static int start_pool_with_signals_blocked(server_t *srv)
{
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    int rc = tp_init(&srv->pool, (size_t)srv->cfg->threads,
                     (size_t)srv->cfg->queue_capacity, handle_connection, srv);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

/* accept 루프: 새 연결을 받아 큐에 넣기만 한다 (요청을 읽지 않으므로 느린
 * 클라이언트가 이 루프를 막을 수 없다). */
static void accept_loop(server_t *srv, int listen_fd)
{
    while (!atomic_load(&g_stop)) {
        struct pollfd p = {.fd = listen_fd, .events = POLLIN, .revents = 0};
        int r = poll(&p, 1, 500); /* 시그널이 poll 과 경쟁해도 최대 0.5초 안에 g_stop 을 본다 */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll(listen): %s", strerror(errno));
            break;
        }
        if (r == 0)
            continue;

        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 고갈: 잠깐 쉬어서 바쁜 루프를 피한다 */
                log_error("accept(): %s (fd limit reached)", strerror(errno));
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
                nanosleep(&ts, NULL);
                continue;
            }
            log_error("accept(): %s", strerror(errno));
            continue;
        }

        cq_status_t qs = tp_submit(&srv->pool, cfd);
        if (qs != CQ_OK) {
            log_error("queue %s: rejecting connection with 503",
                      qs == CQ_FULL ? "full" : "closed");
            reject_connection(cfd, srv);
        }
    }
}

int server_run(const server_config_t *cfg)
{
    server_t *srv = calloc(1, sizeof *srv);
    if (srv == NULL) {
        log_error("out of memory");
        return 1;
    }
    srv->cfg = cfg;
    srv->limits.read_timeout_ms = cfg->read_timeout_ms;
    srv->limits.max_body_bytes = cfg->max_body_bytes;
    log_set_quiet(cfg->quiet);
    int rc = 1;
    int listen_fd = -1;
    char err[256];

    /* document root 를 절대 경로로 확정 (이후 모든 경로 검사의 기준) */
    struct stat sb;
    if (realpath(cfg->doc_root, srv->root_real) == NULL ||
        stat(srv->root_real, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        log_error("document root '%s' is not an accessible directory: %s", cfg->doc_root,
                  strerror(errno));
        goto out;
    }
    if (cfg->backend_count > 0 && proxy_init(&srv->proxy, cfg, err, sizeof err) != 0) {
        log_error("%s", err);
        goto out;
    }
    if (install_signal_handlers() != 0) {
        log_error("sigaction: %s", strerror(errno));
        goto out;
    }

    listen_fd = create_listen_socket(cfg->bind_addr, cfg->port, SOMAXCONN, err, sizeof err);
    if (listen_fd < 0) {
        log_error("%s", err);
        goto out;
    }
    /* --port 0 이면 OS 가 고른 실제 포트를 알아낸다 (테스트에서 사용) */
    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    int actual_port = cfg->port;
    if (getsockname(listen_fd, (struct sockaddr *)&bound, &blen) == 0)
        actual_port = ntohs(bound.sin_port);

    if (start_pool_with_signals_blocked(srv) != 0) {
        log_error("failed to start thread pool");
        goto out;
    }

    log_info("listening on http://%s:%d (root=%s, threads=%d, queue=%d)", cfg->bind_addr,
             actual_port, srv->root_real, cfg->threads, cfg->queue_capacity);
    for (int i = 0; i < cfg->backend_count; i++)
        log_info("proxy: %s/* -> backend %s", cfg->proxy_prefix,
                 srv->proxy.backends[i].host_header);

    accept_loop(srv, listen_fd);

    /* ---- graceful shutdown ---- */
    log_info("shutting down: closing listener, waiting for workers...");
    close(listen_fd); /* 새 연결은 커널 단계에서 거부된다 */
    listen_fd = -1;
    tp_shutdown(&srv->pool, reject_connection, srv);
    log_info("shutdown complete");
    rc = 0;

out:
    if (listen_fd >= 0)
        close(listen_fd);
    free(srv);
    return rc;
}
