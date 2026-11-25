/* 데모의 부하 발생기. 손님 수백 명인 척 앞단(9000)에 요청을 보낸다.
   서버 본체에 이 처리기 하나만 끼워 build/crowd를 만든다 (make demo).

     POST /lab/run     what=book&n=200&c=100&rate=0&retry=1&seat=1
                       요청 n개를 동시에 c개까지. 응답은 진행 상황 한 줄(JSON)의 연속
     POST /lab/raw     받은 바이트를 그대로 앞단에 보내고, 앞단이 준 바이트를 그대로 돌려준다
     POST /lab/upload  받은 본문의 크기와 내용의 합
     POST /lab/reload  앞단의 감독 프로세스에 SIGHUP (설정 다시 읽기)

   보내는 곳은 설정에 적힌 앞단 하나뿐이다. 주소를 요청에서 받으면 남의 서버를 두드리는 도구가 된다.
   스레드는 없다. 서버가 백엔드에 쓰는 연결(upconn)을 빌려 써서, 루프 하나가 연결 1,000개를 함께 돌본다 */
#include "app.h"
#include "demo.h"
#include "loop.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_N 2000
#define MAX_C 1000
#define CONNECTING 64  /* 동시에 연결 중인 수. 앞단의 대기열(macOS 128)이 넘치면 커널이 1초 뒤에야 다시 연결을 시도한다 */
#define SHOT_MS 10000  /* 요청 하나의 제한 */
#define RUN_MS 30000   /* 실행 전체의 제한 */
#define LINE_MS 100    /* 진행 줄 간격 */
#define KEEP 512       /* 응답은 앞부분만 본다. 나머지는 읽어서 버린다 */
#define RAW_MAX 20000   /* 너무 큰 머리(16KB)도 보낼 수 있게 */
#define RAW_MS 7000    /* 408(5초)을 볼 수 있을 만큼, 앞단의 timeout_stall(8초)보다는 짧게 */
#define RAW_QUIET_MS 600 /* 받던 것이 이만큼 조용하면 끝. 예매의 답(0.25~0.5초)이 뒤에 올 수 있으므로 그보다 길게 */

enum kind { JOB_RUN, JOB_HOLD, JOB_RAW, JOB_UPLOAD, JOB_KINDS };
static const int g_limit[JOB_KINDS] = {1, 1, 8, 4}; /* 한 번에 몇 개까지. 자기 자신을 거듭 부르는 것도 여기서 막힌다 */
static atomic_int g_jobs[JOB_KINDS];
static struct addr g_front;

static const struct aim {
    const char *name, *method, *path;
    enum kind kind;
} g_aims[] = {
    {"static", "GET", "/style.css", JOB_RUN}, {"show", "GET", "/api/show", JOB_RUN},
    {"work", "GET", "/api/work", JOB_RUN},    {"post", "POST", "/api/work", JOB_RUN},
    {"rr", "GET", "/rr/work", JOB_RUN},       {"lc", "GET", "/lc/work", JOB_RUN},
    {"book", "POST", "/api/book", JOB_RUN},   {"same", "POST", "/api/book", JOB_RUN},
    {"idle", NULL, NULL, JOB_HOLD},           /* 연결만 열고 아무 말도 하지 않는다 */
    {"half", NULL, NULL, JOB_HOLD},           /* 요청 머리를 보내다 만다 */
};

/* 손님 한 명의 자리. 요청이 끝나면 다음 요청이 이 칸을 이어 쓴다 */
struct shot {
    struct upconn *up; /* NULL = 지금 연결이 없다 */
    int id;            /* 몇 번째 요청인가 */
    bool sent;
    long long born;     /* 이 손님이 처음 보낸 시각 (us). 다시 보내도 그대로 */
    long long t0;       /* 이번 연결을 시작한 시각 (us) */
    long long again_at; /* 0이 아니면 이 시각(ms)에 같은 요청을 다시 보낸다 */
    size_t got;
    char head[KEEP];
};

struct job {
    enum kind kind;
    /* 몰아 보내기 */
    const struct aim *what;
    int n, c, rate, seat;
    bool retry;
    long long began, next_line;
    int next_id, done, live, connecting, waiting, retried, hits;
    int code[600]; /* 상태 코드별 개수. 0 = 응답 없이 끊김 */
    int by[2];     /* 9001, 9002가 답한 수 */
    int owners[SEATS + 1]; /* 이 자리를 샀다고 들은 손님 수. 2 이상이면 두 번 팔린 것 */
    int nfresh, nlat;
    char fresh[MAX_N + 1]; /* 지난 줄 뒤로 끝난 요청들. 한 글자가 하나 */
    int lat[MAX_N];        /* 걸린 시간 (us) */
    /* 그대로 보내기 */
    struct upconn *up;
    bool sent;
    long long t0, last; /* 보낸 시각, 마지막으로 받은 시각 (us) */
    size_t got;
    /* 올리기 */
    long long off;
    unsigned sum;

    struct shot s[]; /* c개 */
};

bool app_open(const char *arg, char *err, size_t cap)
{
    if (net_parse_addr(arg, &g_front))
        return true;
    snprintf(err, cap, "앞단 주소를 읽을 수 없습니다: %s", arg);
    return false;
}

/* 자리가 있으면 일을 만든다. 없으면 응답까지 하고 NULL */
static struct job *new_job(struct conn *c, enum kind kind, int shots)
{
    if (atomic_fetch_add(&g_jobs[kind], 1) >= g_limit[kind]) {
        atomic_fetch_sub(&g_jobs[kind], 1);
        json(c, 409, "{\"error\":\"이미 돌고 있습니다\"}", NO_STORE);
        return NULL;
    }
    struct job *j = calloc(1, sizeof *j + (size_t)shots * sizeof j->s[0]);
    if (!j) {
        atomic_fetch_sub(&g_jobs[kind], 1);
        exchange_error(c, 500, NULL);
        return NULL;
    }
    j->kind = kind;
    j->c = shots;
    c->ex->u.app = j;
    return j;
}

/* 머리를 바로 보내 두면 일이 오래 걸려도 앞단의 timeout_response에 걸리지 않는다 */
static bool start_stream(struct conn *c, const char *type)
{
    struct exchange *ex = c->ex;
    char head[128];
    int n = snprintf(head, sizeof head, "Content-Type: %s\r\nX-Content-Type-Options: nosniff\r\n" NO_STORE, type);
    if (!reply_start(&ex->reply, &c->out, 200, -1, head, (size_t)n, 0)) {
        exchange_error(c, 500, NULL);
        return false;
    }
    ex->deadline = c->loop->now; /* 지난 시각: 조용해도 루프가 0.25~0.5초마다 timeout을 부른다 */
    return true;
}

/* ---- 몰아 보내기 ---- */

static int seat_of(const struct job *j, int id)
{
    return j->what->name[0] == 's' ? j->seat : id % SEATS + 1; /* same : book */
}

static bool is_book(const struct job *j)
{
    return j->what->path && !strcmp(j->what->path, "/api/book");
}

/* Host는 시험을 시킨 화면이 쓴 것과 같게. 캐시는 호스트가 다르면 다른 자료로 본다 */
static size_t request_text(const struct conn *c, int id, char *out, size_t cap)
{
    const struct job *j = c->ex->u.app;
    const struct aim *t = j->what;
    const char *host = c->ex->host;
    int port = c->ex->port;
    char body[64] = "";
    int n;
    if (!t->path)
        n = snprintf(out, cap, "%s", t->name[0] == 'h' ? "GET / HTTP/1.1\r\nHost: localhost\r\nX-Slow: " : "");
    else if (!strcmp(t->method, "GET"))
        n = snprintf(out, cap, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", t->path, host, port);
    else {
        if (is_book(j)) /* 번호표는 손님마다 다르고, 다시 보낼 때는 같다 */
            snprintf(body, sizeof body, "seat=%d&who=%llx", seat_of(j, id),
                     ((unsigned long long)j->t0 << 12) + (unsigned)id + 1);
        n = snprintf(out, cap,
                     "POST %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s",
                     t->path, host, port, strlen(body), body);
    }
    return (size_t)n;
}

static void close_shot(struct conn *c, struct job *j, struct shot *s)
{
    if (!s->up)
        return;
    loop_bury(c->loop, &s->up->io);
    s->up = NULL;
    j->live--;
    if (!s->sent)
        j->connecting--;
}

/* 요청 하나가 끝났다. 응답 앞부분에서 상태 코드와 누가 답했는지를 읽는다 */
static void end_shot(struct conn *c, struct job *j, struct shot *s)
{
    long long us = now_us() - s->born;
    s->head[s->got] = '\0';
    int code = 0;
    if (strncmp(s->head, "HTTP/1.1 ", 9) || sscanf(s->head + 9, "%3d", &code) != 1 || code < 100 || code > 599)
        code = 0;
    close_shot(c, j, s);
    if (j->retry && (code == 0 || code >= 500) && c->loop->now - j->began < RUN_MS - 3000) {
        s->again_at = c->loop->now + 1000 + s->id * 37 % 1000; /* 모두가 같은 순간에 다시 몰려오지 않게 흩는다 */
        j->waiting++;
        j->retried++;
        return;
    }
    const char *by = strstr(s->head, "\"by\":900");
    if (strstr(s->head, "\r\nX-Cache: HIT\r\n"))
        j->hits++; /* 사본에도 by가 적혀 있지만, 그 백엔드는 이번에 일하지 않았다 */
    else if (by && (by[8] == '1' || by[8] == '2'))
        j->by[by[8] - '1']++;
    if (code == 200 && is_book(j))
        j->owners[seat_of(j, s->id)]++;
    if (code)
        j->lat[j->nlat++] = us > 1000000000 ? 1000000000 : (int)us;
    j->fresh[j->nfresh++] = code == 0     ? 'e'
                            : code < 300  ? 'o'
                            : code == 408 ? 'h'
                            : code == 409 ? 'c'
                            : code == 502 ? 'b'
                            : code == 503 ? 'x'
                            : code == 504 ? 't'
                                          : '?';
    j->code[code]++;
    j->done++;
}

static void open_shot(struct conn *c, struct job *j, struct shot *s)
{
    s->t0 = now_us();
    s->sent = false;
    s->again_at = 0;
    s->got = 0;
    s->up = upconn_open(c->loop, &g_front, NULL);
    if (!s->up) {
        end_shot(c, j, s);
        return;
    }
    s->up->c = c;
    j->live++;
    j->connecting++;
}

static bool step_shot(struct conn *c, struct job *j, struct shot *s)
{
    struct io *io = &s->up->io;
    char b[8192];
    if (!s->sent) {
        if (!(io->ready & IO_W))
            return false;
        size_t n = request_text(c, s->id, b, sizeof b);
        if (net_error(io->fd) || write(io->fd, b, n) != (ssize_t)n) { /* 짧으므로 한 번에 들어간다 */
            end_shot(c, j, s);
        } else {
            s->sent = true;
            j->connecting--;
        }
        return true;
    }
    if (!(io->ready & IO_R))
        return false;
    ssize_t r = read(io->fd, b, sizeof b);
    if (r > 0) {
        size_t keep = KEEP - 1 - s->got < (size_t)r ? KEEP - 1 - s->got : (size_t)r;
        memcpy(s->head + s->got, b, keep);
        s->got += keep;
        return true;
    }
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
        io->ready &= ~IO_R;
        return false;
    }
    end_shot(c, j, s); /* Connection: close로 보냈으므로 닫힘이 곧 응답의 끝 */
    return true;
}

static int by_value(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* 진행 상황 한 줄. 자리가 없으면 false: 쌓아 두지 않고, 다음 줄이 이번 것까지 담는다 */
static bool write_line(struct conn *c, struct job *j, bool last)
{
    char b[MAX_N + 2048];
    j->fresh[j->nfresh] = '\0';
    int n = snprintf(b, sizeof b,
                     "{\"t\":%lld,\"n\":%d,\"done\":%d,\"live\":%d,\"waiting\":%d,\"retried\":%d,\"hits\":%d,"
                     "\"by\":[%d,%d],\"new\":\"%s\",\"codes\":{",
                     c->loop->now - j->began, j->n, j->done, j->live, j->waiting, j->retried, j->hits, j->by[0],
                     j->by[1], j->fresh);
    const char *comma = "";
    for (int i = 0; i < 600 && n < (int)sizeof b - 512; i++)
        if (j->code[i]) {
            n += snprintf(b + n, sizeof b - (size_t)n, "%s\"%d\":%d", comma, i, j->code[i]);
            comma = ",";
        }
    n += snprintf(b + n, sizeof b - (size_t)n, "}");
    if (last) {
        int sold = 0, dup = 0;
        for (int i = 1; i <= SEATS; i++) {
            sold += j->owners[i] > 0;
            dup += j->owners[i] > 1;
        }
        qsort(j->lat, (size_t)j->nlat, sizeof j->lat[0], by_value);
        int p50 = j->nlat ? j->lat[j->nlat / 2] : 0;
        int p99 = j->nlat ? j->lat[(j->nlat * 99 + 99) / 100 - 1] : 0;
        int max = j->nlat ? j->lat[j->nlat - 1] : 0;
        n += snprintf(b + n, sizeof b - (size_t)n,
                      ",\"end\":true,\"sold\":%d,\"dup\":%d,\"p50\":%d,\"p99\":%d,\"max\":%d", sold, dup, p50, p99, max);
    }
    n += snprintf(b + n, sizeof b - (size_t)n, "}\n");
    if (reply_room(&c->ex->reply, &c->out) < (size_t)n)
        return false;
    reply_write(&c->ex->reply, &c->out, b, (size_t)n);
    j->nfresh = 0;
    j->next_line = c->loop->now + LINE_MS;
    return true;
}

static bool run_pump(struct conn *c, struct job *j)
{
    long long now = c->loop->now;
    bool moved = false, over = now - j->began > RUN_MS || c->loop->draining;
    int allowed = j->n;
    if (j->rate) { /* 초당 rate개씩: 지금까지 몇 개를 보냈어야 하나 */
        long long due = 1 + (now - j->began) * j->rate / 1000;
        allowed = due < j->n ? (int)due : j->n;
    }
    for (int i = 0; i < j->c; i++) {
        struct shot *s = &j->s[i];
        if (s->up) {
            moved |= step_shot(c, j, s);
            continue;
        }
        if (over || j->connecting >= CONNECTING || (s->again_at ? now < s->again_at : j->next_id >= allowed))
            continue;
        if (s->again_at) {
            j->waiting--;
        } else {
            s->id = j->next_id++;
            s->born = now_us();
        }
        open_shot(c, j, s);
        moved = true;
    }
    if (j->done == j->n || over) {
        if (!write_line(c, j, true))
            return moved; /* 자리가 나면 다시 */
        exchange_finish(c);
        return true;
    }
    if (now >= j->next_line)
        write_line(c, j, false);
    return moved;
}

static void start_run(struct conn *c)
{
    char body[128], name[8] = "", end;
    int n = 0, shots = 0, rate = 0, retry = 0, seat = 0;
    const struct aim *t = NULL;
    if (body_text(c, body, sizeof body) &&
        sscanf(body, "what=%7[a-z]&n=%4d&c=%4d&rate=%3d&retry=%1d&seat=%2d%c", name, &n, &shots, &rate, &retry, &seat,
               &end) == 6)
        for (size_t i = 0; i < sizeof g_aims / sizeof g_aims[0]; i++)
            if (!strcmp(name, g_aims[i].name))
                t = &g_aims[i];
    if (!t || n < 1 || n > MAX_N || shots < 1 || shots > MAX_C || rate < 0 || rate > 200 || seat < 1 || seat > SEATS) {
        json(c, 400, "{\"error\":\"what, n=1~2000, c=1~1000, rate=0~200, retry, seat=1~60 순서로 적습니다\"}", NO_STORE);
        return;
    }
    struct job *j = new_job(c, t->kind, shots < n ? shots : n);
    if (!j || !start_stream(c, "application/x-ndjson"))
        return;
    j->what = t;
    j->n = n;
    j->rate = rate;
    j->retry = retry;
    j->seat = seat;
    j->began = c->loop->now;
    j->t0 = now_us();
}

/* ---- 그대로 보내기 ---- */

static void start_raw(struct conn *c)
{
    long long size = c->ex->body.size;
    if (size < 1 || size > RAW_MAX) {
        json(c, 400, "{\"error\":\"1~20000바이트만 보낼 수 있습니다\"}", NO_STORE);
        return;
    }
    struct job *j = new_job(c, JOB_RAW, 0);
    if (!j)
        return;
    j->up = upconn_open(c->loop, &g_front, NULL);
    if (!j->up) {
        json(c, 503, "{\"error\":\"앞단에 연결하지 못했습니다\"}", NO_STORE);
        return;
    }
    j->up->c = c;
    j->t0 = j->last = now_us();
    start_stream(c, "application/octet-stream");
}

/* 받은 바이트 뒤에 '\0'과 결과를 붙이고 끝낸다 */
static bool end_raw(struct conn *c, struct job *j, bool closed)
{
    char b[96] = "";
    int n = 1 + snprintf(b + 1, sizeof b - 1, "{\"closed\":%s,\"us\":%lld}", closed ? "true" : "false", j->last - j->t0);
    if (reply_room(&c->ex->reply, &c->out) < (size_t)n)
        return false;
    reply_write(&c->ex->reply, &c->out, b, (size_t)n);
    exchange_finish(c);
    return true;
}

static bool raw_pump(struct conn *c, struct job *j)
{
    struct io *io = &j->up->io;
    char b[RAW_MAX];
    if (!j->sent) {
        if (!(io->ready & IO_W))
            return false;
        size_t n = (size_t)c->ex->body.size;
        if (spool_read(&c->ex->body, 0, b, n) != (ssize_t)n || net_error(io->fd) || write(io->fd, b, n) != (ssize_t)n)
            return end_raw(c, j, true);
        j->sent = true; /* 쓰기는 닫지 않는다. 닫으면 앞단이 "보내다 만 요청"을 바로 끊어 408을 볼 수 없다 */
        j->t0 = j->last = now_us();
        return true;
    }
    size_t room = reply_room(&c->ex->reply, &c->out);
    if (!(io->ready & IO_R) || room == 0)
        return false;
    ssize_t r = read(io->fd, b, room < sizeof b ? room : sizeof b);
    if (r > 0) {
        reply_write(&c->ex->reply, &c->out, b, (size_t)r);
        j->last = now_us();
        j->got += (size_t)r;
        return true;
    }
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
        io->ready &= ~IO_R;
        return false;
    }
    return end_raw(c, j, true);
}

/* ---- 올리기 ---- */

static bool upload_pump(struct conn *c, struct job *j)
{
    char b[32768]; /* 한 번에 이만큼만. 8MB를 한꺼번에 읽으면 그동안 다른 연결이 기다린다 */
    ssize_t n = spool_read(&c->ex->body, j->off, b, sizeof b);
    for (ssize_t i = 0; i < n; i++)
        j->sum = (j->sum ^ (unsigned char)b[i]) * 16777619u; /* FNV-1a: 화면의 JavaScript도 같은 식으로 센다 */
    if (n > 0)
        j->off += n;
    if (j->off == c->ex->body.size) {
        snprintf(b, 96, "{\"size\":%lld,\"sum\":\"%08x\"}", j->off, j->sum);
        json(c, 200, b, NO_STORE);
    } else if (n <= 0) {
        exchange_error(c, 500, NULL);
    }
    return true;
}

/* ---- 처리기 ---- */

static void reload_front(struct conn *c)
{
    const char *s = getenv("FRONT_PID"); /* demo/run.sh가 알려 준다 */
    int pid = s ? atoi(s) : 0;
    if (pid > 1 && kill((pid_t)pid, SIGHUP) == 0)
        json(c, 200, "{}", NO_STORE);
    else
        json(c, 503, "{\"error\":\"앞단의 번호를 모릅니다. demo/run.sh로 켜 주세요\"}", NO_STORE);
}

static void crowd_start(struct conn *c)
{
    const char *path = c->ex->path;
    if (!strcmp(path, "/lab/health"))
        json(c, 200, "{}", NO_STORE);
    else if (c->ex->req.method != M_POST)
        exchange_error(c, 405, "Allow: POST\r\n");
    else if (!from_page(c))
        json(c, 403, "{\"error\":\"데모 화면에서 온 요청이 아닙니다\"}", NO_STORE);
    else if (!strcmp(path, "/lab/run"))
        start_run(c);
    else if (!strcmp(path, "/lab/raw"))
        start_raw(c);
    else if (!strcmp(path, "/lab/reload"))
        reload_front(c);
    else if (strcmp(path, "/lab/upload"))
        exchange_error(c, 404, NULL);
    else {
        struct job *j = new_job(c, JOB_UPLOAD, 0);
        if (j)
            j->sum = 2166136261u;
    }
}

/* 끝내는 함수(exchange_finish 등)를 부르면 job이 해제된다. 그 뒤로는 만지지 말 것 */
static bool crowd_pump(struct conn *c)
{
    struct job *j = c->ex->u.app;
    if (!j)
        return false;
    if (c->rd_eof) { /* 손님이 떠났다. 보내던 연결도 모두 거둔다 */
        exchange_abort(c);
        return true;
    }
    switch (j->kind) {
    case JOB_RAW: return raw_pump(c, j);
    case JOB_UPLOAD: return upload_pump(c, j);
    default: return run_pump(c, j);
    }
}

static void crowd_interest(struct conn *c)
{
    struct job *j = c->ex->u.app;
    if (!j)
        return;
    for (int i = 0; i < j->c; i++)
        if (j->s[i].up)
            loop_watch(c->loop, &j->s[i].up->io, j->s[i].sent ? IO_R : IO_W);
    if (j->up)
        loop_watch(c->loop, &j->up->io, !j->sent ? IO_W : reply_room(&c->ex->reply, &c->out) ? IO_R : 0);
}

/* 0.25~0.5초마다. 여기서 돌아가면 루프가 이어서 pump를 부른다 */
static void crowd_timeout(struct conn *c)
{
    struct job *j = c->ex->u.app;
    if (!j || c->rd_eof)
        return;
    long long now = now_us();
    for (int i = 0; i < j->c; i++)
        if (j->s[i].up && now - j->s[i].t0 > SHOT_MS * 1000LL) {
            j->s[i].got = 0; /* 다 받지 못한 응답은 "끊김"으로 센다 */
            end_shot(c, j, &j->s[i]);
        }
    if (j->kind == JOB_RAW && (c->loop->draining || now - j->t0 > RAW_MS * 1000LL ||
                               (j->got && now - j->last > RAW_QUIET_MS * 1000LL)))
        end_raw(c, j, false);
}

static void crowd_stop(struct conn *c)
{
    struct job *j = c->ex->u.app;
    if (!j)
        return;
    for (int i = 0; i < j->c; i++)
        close_shot(c, j, &j->s[i]);
    if (j->up)
        loop_bury(c->loop, &j->up->io);
    atomic_fetch_sub(&g_jobs[j->kind], 1);
    free(j);
    c->ex->u.app = NULL;
}

static const struct handler handler = {.name = "crowd", .start = crowd_start, .pump = crowd_pump,
                                       .interest = crowd_interest, .timeout = crowd_timeout, .stop = crowd_stop};
const struct handler *const app_handler = &handler;
