/* CGI: 요청마다 프로그램을 하나 실행하고, 그 표준 출력을 응답으로 돌려준다 */
#include "cgi.h"
#include "config.h"
#include "conn.h"
#include "loop.h"
#include "server.h"
#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CGI_IN_MAX 65536
#define CGI_EXIT_MS 1000 /* 출력을 닫은 뒤 끝날 때까지 주는 시간 */
#define CGI_MAX_ENV 160

static struct cgi_st *st_of(struct conn *c)
{
    return &c->ex->u.cgi;
}

/* ---- 환경 변수 ---- */

static bool env(struct buf *out, const char *name, struct sl v)
{
    return buf_append(out, name, strlen(name)) && buf_append(out, "=", 1) && buf_append(out, v.p, v.n) &&
           buf_append(out, "", 1);
}

static bool envs(struct buf *out, const char *name, const char *v)
{
    return env(out, name, (struct sl){v, strlen(v)});
}

static bool same_name(struct sl a, struct sl b)
{
    return a.n == b.n && !strncasecmp(a.p, b.p, a.n);
}

/* "이름=값\0" 을 이어 붙인다. 우리 서버의 환경 변수는 물려주지 않는다 (비밀 값이 샐 수 있다) */
bool cgi_env(const struct exchange *ex, const char *ip, int port, const char *script, const char *info, struct buf *out)
{
    const struct http_req *r = &ex->req;
    struct sl q = ex->target.query.n ? (struct sl){ex->target.query.p + 1, ex->target.query.n - 1} : (struct sl){"", 0};
    char num[32], portbuf[16];
    snprintf(num, sizeof num, "%lld", ex->body.size);
    snprintf(portbuf, sizeof portbuf, "%d", port);
    bool ok = envs(out, "GATEWAY_INTERFACE", "CGI/1.1") && envs(out, "SERVER_PROTOCOL", r->minor ? "HTTP/1.1" : "HTTP/1.0") &&
              envs(out, "SERVER_SOFTWARE", "jiho-webserver") && envs(out, "SERVER_NAME", ex->host[0] ? ex->host : "localhost") &&
              envs(out, "SERVER_PORT", portbuf) && env(out, "REQUEST_METHOD", r->method_text) && env(out, "QUERY_STRING", q) &&
              envs(out, "SCRIPT_NAME", script) && envs(out, "PATH_INFO", info) && envs(out, "REMOTE_ADDR", ip) &&
              envs(out, "PATH", "/usr/local/bin:/usr/bin:/bin");
    if (ok && ex->body.size)
        ok = envs(out, "CONTENT_LENGTH", num);
    for (int i = 0; ok && i < r->h.n; i++) {
        struct sl name = r->h.f[i].name;
        bool seen = false; /* 같은 이름이 여러 줄이면 처음 나온 자리에서 하나로 합친다 */
        for (int k = 0; k < i && !seen; k++)
            seen = same_name(r->h.f[k].name, name);
        if (seen)
            continue;
        if (sl_eqi(name, "Content-Type")) {
            ok = env(out, "CONTENT_TYPE", r->h.f[i].value);
            continue;
        }
        /* Proxy 헤더가 HTTP_PROXY가 되면 프로그램이 프록시 설정으로 착각한다 (httpoxy 취약점).
           비밀번호가 든 Authorization도 넘기지 않는다 */
        if (sl_eqi(name, "Content-Length") || sl_eqi(name, "Proxy") || sl_eqi(name, "Authorization") ||
            fields_hop_by_hop(&r->h, name) || name.n > 100)
            continue;
        char key[128] = "HTTP_";
        bool plain = true;
        for (size_t k = 0; k < name.n; k++) {
            char ch = name.p[k];
            plain = plain && ch != '_'; /* A_B와 A-B가 같은 이름이 되어 덮어쓰는 것을 막는다 */
            key[5 + k] = ch == '-' ? '_' : (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        }
        key[5 + name.n] = '\0';
        if (!plain)
            continue;
        if (sl_eqi(name, "Host") && ex->target.absolute) { /* 주소에 적힌 호스트가 Host 헤더보다 먼저다 */
            ok = env(out, key, ex->target.authority);
            continue;
        }
        ok = buf_printf(out, "%s=", key) && buf_append(out, r->h.f[i].value.p, r->h.f[i].value.n);
        for (int k = i + 1; ok && k < r->h.n; k++)
            if (same_name(r->h.f[k].name, name))
                ok = buf_append(out, sl_eqi(name, "Cookie") ? "; " : ", ", 2) &&
                     buf_append(out, r->h.f[k].value.p, r->h.f[k].value.n);
        ok = ok && buf_append(out, "", 1);
    }
    return ok;
}

/* ---- 자식 프로세스 ---- */

static int spawn(const char *path, struct buf *envbuf, int in, int out, pid_t *pid)
{
    char *envp[CGI_MAX_ENV + 1];
    int n = 0;
    for (char *p = buf_head(envbuf), *end = p + buf_len(envbuf); p < end && n < CGI_MAX_ENV; p += strlen(p) + 1)
        envp[n++] = p;
    envp[n] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_adddup2(&fa, in, 0);
    posix_spawn_file_actions_adddup2(&fa, out, 1);

    /* 무시해 둔 SIGPIPE는 exec 뒤에도 남는다. 자식에게는 기본값으로 돌려준다 */
    sigset_t def, none;
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setpgroup(&at, 0); /* 손자까지 한꺼번에 종료할 수 있게 그룹을 새로 만든다 */
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; /* 우리가 넘긴 것 말고는 어떤 fd도 물려받지 않는다 */
    posix_spawn_file_actions_addinherit_np(&fa, 2);
#endif
    posix_spawnattr_setflags(&at, flags);
    char *argv[] = {(char *)path, NULL};
    /* 스레드가 여럿인 프로세스에서 fork하면 자식이 남의 락을 물려받는다. posix_spawn은 그 틈이 없다 */
    int err = posix_spawn(pid, path, &fa, &at, argv, envp);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return err;
}

void cgi_reap(struct loop *l, bool force)
{
    struct child **pp = &l->children;
    l->reaping = false;
    while (*pp) {
        struct child *ch = *pp;
        if (!force && !ch->kill_at) { /* 아직 응답 중인 요청이 쓰고 있다. 놓은 뒤에만 회수한다 */
            pp = &ch->next;
            continue;
        }
        siginfo_t si;
        memset(&si, 0, sizeof si);
        /* 먼저 끝났는지만 보고(WNOWAIT), 그룹에 남은 손자를 정리한 뒤 회수한다.
           회수한 뒤에는 번호가 다른 프로세스에 재사용될 수 있어 kill하면 안 된다 */
        bool exited = waitid(P_PID, (id_t)ch->pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == ch->pid;
        if (exited || force || (ch->kill_at && l->now >= ch->kill_at))
            kill(-ch->pid, SIGKILL);
        if (force)
            waitpid(ch->pid, NULL, 0);
        if (!force && (!exited || waitpid(ch->pid, NULL, WNOHANG) != ch->pid)) {
            l->reaping = l->reaping || ch->kill_at;
            pp = &ch->next;
            continue;
        }
        *pp = ch->next;
        free(ch);
        atomic_fetch_sub(&l->srv->cgi_running, 1);
    }
}

void cgi_proc_free(struct cgi_proc *p)
{
    buf_free(&p->in);
    free(p);
}

void cgi_event(struct cgi_proc *p)
{
    conn_run(p->c);
}

/* ---- 핸들러 ---- */

/* 프로그램이 실행해도 되는 파일인가. 0 또는 상태 코드 */
static int check_script(const struct route *r, const char *name)
{
    if (!name[0] || name[0] == '.')
        return 403;
    struct stat st;
    if (fstatat(r->root_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return status_from_errno(errno);
    return S_ISREG(st.st_mode) && faccessat(r->root_fd, name, X_OK, 0) == 0 ? 0 : 403;
}

static void cgi_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct server *srv = c->loop->srv;
    struct cgi_st *s = st_of(c);
    memset(s, 0, sizeof *s);

    char rel[HTTP_URI_MAX + 16], info[HTTP_URI_MAX + 16] = "", name[300], path[1024];
    const char *tail = ex->path + ex->route->prefix_len;
    if (!*tail || !uri_decode(tail, rel, sizeof rel)) {
        exchange_error(c, 404, NULL);
        return;
    }
    /* "/hello.sh/a/b" → 프로그램 "hello.sh", PATH_INFO "/a/b" */
    char *script = rel + 1, *slash = strchr(script, '/');
    if (slash) {
        snprintf(info, sizeof info, "%s", slash);
        *slash = '\0';
    }
    int status = check_script(ex->route, script);
    if (!status && snprintf(name, sizeof name, "%s/%s", ex->route->prefix, script) >= (int)sizeof name)
        status = 404;
    if (!status && snprintf(path, sizeof path, "%s/%s", ex->route->dir, script) >= (int)sizeof path)
        status = 404;
    if (status) {
        exchange_error(c, status, NULL);
        return;
    }
    if (atomic_fetch_add(&srv->cgi_running, 1) >= srv->cfg->max_cgi) {
        atomic_fetch_sub(&srv->cgi_running, 1);
        exchange_error(c, 503, "Retry-After: 1\r\n");
        return;
    }

    struct buf envbuf;
    buf_init(&envbuf, 65536);
    int in = ex->body.size ? spool_to_file(&ex->body) : open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out[2] = {-1, -1};
    struct cgi_proc *p = calloc(1, sizeof *p);
    struct child *ch = calloc(1, sizeof *ch);
    bool ok = p && ch && in >= 0 && cgi_env(ex, c->ip, net_local_port(c->io.fd), name, info, &envbuf) &&
              net_pipe_for_child(out) == 0 && spawn(path, &envbuf, in, out[1], &ch->pid) == 0;
    if (in >= 0 && !ex->body.size)
        close(in);
    if (out[1] >= 0)
        close(out[1]);
    buf_free(&envbuf);
    if (!ok) {
        if (out[0] >= 0)
            close(out[0]);
        free(p);
        free(ch);
        atomic_fetch_sub(&srv->cgi_running, 1);
        exchange_error(c, 500, NULL); /* 실행 파일은 있는데 실행이 안 된다 (인터프리터 없음 등) */
        return;
    }
    ch->kill_at = 0;
    ch->next = c->loop->children;
    c->loop->children = ch;
    p->io = (struct io){.fd = out[0], .kind = IO_CGI};
    p->c = c;
    buf_init(&p->in, CGI_IN_MAX);
    s->p = p;
    s->child = ch;
    s->touched = c->loop->now;
    ex->deadline = c->loop->now + srv->cfg->cgi_ms;
    snprintf(ex->served_by, sizeof ex->served_by, "cgi:%s", script);
}

size_t cgi_head_end(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { /* CGI 프로그램은 줄 끝에 \n만 쓰기도 한다 */
        if (p[i] != '\n')
            continue;
        if (i + 1 < n && p[i + 1] == '\n')
            return i + 2;
        if (i + 2 < n && p[i + 1] == '\r' && p[i + 2] == '\n')
            return i + 3;
    }
    return 0;
}

bool cgi_parse_head(const char *p, size_t n, int *status, long long *length, struct buf *extra)
{
    const char *end = p + n;
    bool any = false, location = false;
    *status = 0;
    *length = -1;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *eol = nl > p && nl[-1] == '\r' ? nl - 1 : nl;
        if (eol == p)
            break;
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon)
            return false;
        struct sl name = {p, (size_t)(colon - p)};
        struct sl value = sl_trim((struct sl){colon + 1, (size_t)(eol - colon - 1)});
        p = nl + 1;
        if (!http_is_token(name))
            return false;
        for (size_t i = 0; i < value.n; i++)
            if ((unsigned char)value.p[i] < 0x20 && value.p[i] != '\t')
                return false;
        any = true;
        if (sl_eqi(name, "Status")) {
            long long v;
            if (value.n < 3 || !sl_to_ll((struct sl){value.p, 3}, &v) || v < 200 || v > 599)
                return false;
            *status = (int)v;
            continue;
        }
        if (sl_eqi(name, "Content-Length")) {
            if (!sl_to_ll(value, length))
                return false;
            continue;
        }
        /* 연결과 본문 길이에 관한 헤더는 서버가 정한다 */
        if (sl_eqi(name, "Connection") || sl_eqi(name, "Transfer-Encoding") || sl_eqi(name, "Keep-Alive"))
            continue;
        location = location || sl_eqi(name, "Location");
        if (!buf_append(extra, name.p, name.n) || !buf_append(extra, ": ", 2) ||
            !buf_append(extra, value.p, value.n) || !buf_append(extra, "\r\n", 2))
            return false;
    }
    if (*status == 0)
        *status = location ? 302 : 200;
    return any;
}

static bool cgi_pump(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct cgi_st *s = st_of(c);
    struct cgi_proc *p = s->p;
    const struct config *cfg = c->loop->srv->cfg;
    bool moved = false;
    if (!s->eof && (p->io.ready & IO_R) && buf_room(&p->in)) {
        size_t n = buf_room(&p->in);
        char *dst = buf_reserve(&p->in, n);
        ssize_t r = dst ? read(p->io.fd, dst, n) : -1;
        if (r > 0) {
            buf_commit(&p->in, (size_t)r);
            if ((size_t)r < n)
                p->io.ready &= ~IO_R;
            s->touched = c->loop->now;
            moved = true;
        } else if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            p->io.ready &= ~IO_R;
        } else {
            s->eof = true;
            moved = true;
        }
    }
    if (!s->head_done) {
        size_t hl = cgi_head_end(buf_head(&p->in), buf_len(&p->in));
        if (!hl && !s->eof && buf_len(&p->in) <= HTTP_HEAD_MAX)
            return moved;
        struct buf extra;
        int status;
        long long length;
        buf_init(&extra, HTTP_HEAD_MAX * 2);
        bool ok = hl && hl <= HTTP_HEAD_MAX && cgi_parse_head(buf_head(&p->in), hl, &status, &length, &extra) &&
                  reply_start(&ex->reply, &c->out, status, length, buf_head(&extra), buf_len(&extra), 0);
        buf_free(&extra);
        if (!ok) {
            exchange_error(c, 500, NULL); /* 머리 없이 끝났거나 형식이 틀렸다 */
            return true;
        }
        buf_consume(&p->in, hl);
        s->head_done = true;
        moved = true;
    }
    if (ex->reply.framing == FRAME_NONE || (ex->reply.framing == FRAME_LENGTH && ex->reply.left == 0)) {
        exchange_finish(c); /* 더 보낼 것이 없다. 프로그램이 계속 출력해도 기다리지 않는다 */
        return true;
    }
    size_t n = reply_room(&ex->reply, &c->out);
    if (n > buf_len(&p->in))
        n = buf_len(&p->in);
    if (n) {
        reply_write(&ex->reply, &c->out, buf_head(&p->in), n);
        buf_consume(&p->in, n);
        moved = true;
    }
    if (s->eof && !buf_len(&p->in)) {
        exchange_finish(c);
        return true;
    }
    if (reply_room(&ex->reply, &c->out) == 0) {
        s->touched = c->loop->now;
        ex->deadline = 0;
    } else {
        ex->deadline = s->touched + cfg->stall_ms;
    }
    return moved;
}

static void cgi_interest(struct conn *c)
{
    struct cgi_st *s = st_of(c);
    if (s->p)
        loop_watch(c->loop, &s->p->io, !s->eof && buf_room(&s->p->in) ? IO_R : 0);
}

static void cgi_timeout(struct conn *c)
{
    exchange_error(c, 504, NULL); /* 이미 머리를 보냈으면 연결을 끊는다 */
}

static void cgi_stop(struct conn *c)
{
    struct cgi_st *s = st_of(c);
    if (s->child) { /* 출력을 다 냈으면 스스로 끝날 시간을 주고, 아니면 바로 종료 */
        s->child->kill_at = s->eof ? c->loop->now + CGI_EXIT_MS : 1;
        c->loop->reaping = true;
    }
    s->child = NULL;
    if (s->p)
        loop_bury(c->loop, &s->p->io);
    s->p = NULL;
}

const struct handler cgi_handler = {
    .name = "cgi", .start = cgi_start, .pump = cgi_pump, .interest = cgi_interest,
    .timeout = cgi_timeout, .stop = cgi_stop,
};
