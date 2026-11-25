/* 좌석 예매 데모 백엔드. 서버 본체에 이 처리기 하나만 끼워 build/seats를 만든다 (make demo).

     GET  /api/show     공연 정보 (프록시가 캐시해도 되는 유일한 응답). POST하면 사본이 지워진다
     GET  /api/seats    지금 좌석판
     GET  /api/events   끝나지 않는 응답. 좌석판이 바뀔 때마다 서버가 먼저 보낸다
     POST /api/book     seat=12&who=1a2b   (who: 손님이 스스로 정한 번호표)
     POST /api/reset    좌석 비우기
     GET·POST /api/work, /rr/work, /lc/work   누가 답했는지만 말하는 빈 일. 앞단이 경로마다 다른 묶음으로 보낸다
     POST /fault/9001   mode=slow   10초 동안 고장 난 척 (slow hang busy drop die ok)

   좌석은 파일을 mmap한 메모리에 있다. 그래서 백엔드 여러 개가 같은 좌석판을 보고, 죽었다 다시 떠도 남아 있다 */
#include "app.h"
#include "demo.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILE_SIZE (SEATS * sizeof(atomic_ullong))
#define PAY_MS 250   /* 결제를 흉내 내는 시간. 루프가 제한 시간을 0.25~0.5초마다 보므로 더 잘게는 못 잰다 */
#define BEAT_MS 2000 /* 조용할 때 보내는 빈 신호. 없으면 프록시가 30초 뒤 끊고(timeout_stall), 손님이 떠난 것도 모른다 */
#define FAULT_MS 10000 /* 고장은 저절로 풀린다. 켜 둔 것을 잊어도 화면이 망가진 채 남지 않게 */
#define SLOW_MS 1500   /* 앞단의 timeout_response(3초)보다 짧아야 "느림"이지 "멈춤"이 아니다 */

/* 잠금으로 흉내 낸 atomic은 그 잠금이 프로세스마다 따로라서, 다른 프로세스와 함께 쓸 수 없다 */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64비트 atomic에 잠금이 필요한 기계에서는 쓸 수 없다");

/* 칸의 값: 0 = 빈자리, 그 외 = 산 사람의 번호표.
   shortcut: 디스크에 다 쓰기 전에 전원이 꺼지면 예매를 잃는다,
   진짜 판매라면 응답 전에 디스크까지 내려쓴다(msync, macOS는 이어서 F_FULLFSYNC). 느리므로 루프 밖에서 */
static atomic_ullong *g_seat;

struct seat_st {
    bool push;
    bool again; /* 이미 이 손님의 자리였다 */
    int seat;
    unsigned long long who; /* 번호표 */
    char board[SEATS + 1]; /* 이 손님에게 마지막으로 보낸 좌석판 */
    long long next_beat;
};

bool app_open(const char *path, char *err, size_t cap)
{
    if (g_seat) {
        snprintf(err, cap, "app 경로는 하나만 적을 수 있습니다");
        return false;
    }
    void *p = MAP_FAILED;
    struct stat st;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0 && fstat(fd, &st) == 0) {
        /* 새 파일이거나 우리가 만든 크기일 때만. 경로를 잘못 적어 남의 파일을 망가뜨리지 않게 */
        if (!S_ISREG(st.st_mode) || (st.st_size != 0 && st.st_size != (off_t)FILE_SIZE))
            errno = EINVAL;
        else if (ftruncate(fd, (off_t)FILE_SIZE) == 0)
            p = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (p == MAP_FAILED)
        snprintf(err, cap, "좌석 파일을 쓸 수 없습니다: %s: %s", path, strerror(errno));
    else
        g_seat = p;
    if (fd >= 0)
        close(fd); /* 닫아도 mmap은 남는다 */
    return g_seat != NULL;
}

static int my_port(const struct conn *c)
{
    return net_port(&c->loop->srv->cfg->listen);
}

/* 0 = 빈자리, 1 = 팔림, 2 = 내 자리. 남의 번호표는 내보내지 않는다 */
static void read_board(unsigned long long who, char out[SEATS + 1])
{
    for (int i = 0; i < SEATS; i++) {
        unsigned long long owner = atomic_load(&g_seat[i]);
        out[i] = owner == 0 ? '0' : owner == who ? '2' : '1';
    }
    out[SEATS] = '\0';
}

/* ---- 고장 내기 ---- */

enum fault { FT_OK, FT_SLOW, FT_HANG, FT_BUSY, FT_DROP, FT_DIE, FT_N };
static const char *const g_fault_name[FT_N] = {"ok", "slow", "hang", "busy", "drop", "die"};
static atomic_int g_fault;
static atomic_llong g_until; /* 이 시각까지 */

static enum fault fault_now(const struct conn *c)
{
    return c->loop->now < atomic_load(&g_until) ? (enum fault)atomic_load(&g_fault) : FT_OK;
}

/* 앞단은 어느 백엔드로 보낼지 스스로 고르므로, 포트를 짚어 말할 수 있게 경로마다 백엔드 하나짜리 묶음을 둔다 */
static void control(struct conn *c)
{
    char mine[24], body[24], out[64];
    snprintf(mine, sizeof mine, "/fault/%d", my_port(c));
    bool post = c->ex->req.method == M_POST;
    if (strcmp(c->ex->path, mine)) {
        exchange_error(c, 404, NULL);
        return;
    }
    if (!post && c->ex->req.method != M_GET) {
        exchange_error(c, 405, "Allow: GET, POST\r\n");
        return;
    }
    if (post && !from_page(c)) {
        json(c, 403, "{\"error\":\"데모 화면에서 온 요청이 아닙니다\"}", NO_STORE);
        return;
    }
    if (post) {
        int f = FT_N;
        if (body_text(c, body, sizeof body) && !strncmp(body, "mode=", 5))
            for (f = 0; f < FT_N && strcmp(body + 5, g_fault_name[f]); f++) {
            }
        if (f == FT_N) {
            json(c, 400, "{\"error\":\"mode=ok, slow, hang, busy, drop, die 가운데 하나입니다\"}", NO_STORE);
            return;
        }
        if (f == FT_DIE)
            raise(SIGKILL); /* 처리 중이던 것까지 그대로 끝난다. demo/run.sh가 몇 초 뒤 다시 켠다 */
        atomic_store(&g_fault, f);
        atomic_store(&g_until, f == FT_OK ? 0 : c->loop->now + FAULT_MS);
    }
    long long left = atomic_load(&g_until) - c->loop->now;
    snprintf(out, sizeof out, "{\"mode\":\"%s\",\"left_ms\":%lld}", g_fault_name[fault_now(c)], left > 0 ? left : 0);
    json(c, 200, out, NO_STORE);
}

/* ---- 좌석 ---- */

/* 16진수 번호표. 0이나 잘못된 글자면 false */
static bool parse_who(const char *s, unsigned long long *who)
{
    char end;
    return strspn(s, "0123456789abcdef") == strlen(s) && sscanf(s, "%16llx%c", who, &end) == 1 && *who != 0;
}

/* 이 손님의 좌석판이 바뀌었으면 보낸다. 조용하면 가끔 빈 신호만 */
static void push(struct conn *c)
{
    struct seat_st *st = c->ex->u.app;
    char now[SEATS + 1], line[128];
    int n = 0;
    read_board(st->who, now);
    if (strcmp(now, st->board))
        n = snprintf(line, sizeof line, "data: {\"board\":\"%s\",\"by\":%d}\n\n", now, my_port(c));
    else if (c->loop->now >= st->next_beat)
        n = snprintf(line, sizeof line, ":\n\n");
    if (n == 0 || reply_room(&c->ex->reply, &c->out) < (size_t)n)
        return; /* 받아 가지 못하는 손님에게 쌓아 두지 않는다. 다음 차례에 그때의 좌석판을 보낸다 */
    reply_write(&c->ex->reply, &c->out, line, (size_t)n);
    strcpy(st->board, now);
    st->next_beat = c->loop->now + BEAT_MS;
}

static void start_events(struct conn *c)
{
    static const char head[] = "Content-Type: text/event-stream\r\n" NO_STORE;
    struct exchange *ex = c->ex;
    struct seat_st *st = calloc(1, sizeof *st);
    char q[64] = "";
    ex->u.app = st;
    if (!st || !reply_start(&ex->reply, &c->out, 200, -1, head, sizeof head - 1, 0)) {
        exchange_error(c, 500, NULL);
        return;
    }
    if (ex->reply.head_only) {
        exchange_finish(c);
        return;
    }
    static const char retry[] = "retry: 500\n\n"; /* 끊기면 브라우저가 0.5초 뒤 다시 잇는다 (안 적으면 몇 초) */
    reply_write(&ex->reply, &c->out, retry, sizeof retry - 1);
    st->push = true;
    if (sl_copy(ex->target.query, q, sizeof q) && !strncmp(q, "?who=", 5) && !parse_who(q + 5, &st->who))
        st->who = 0; /* 번호표를 모르면 "내 자리" 표시만 없다 */
    push(c);
    ex->deadline = c->loop->now; /* 지난 시각으로 두면 루프가 제한 시간을 볼 때마다(0.25~0.5초) timeout이 불린다 */
}

/* 예매: 빈자리일 때만 내 번호표로 바꾼다. 비교와 교환이 CPU 명령 하나라서 동시에 와도 한 명만 성공한다.
   같은 번호표로 다시 오면 성공으로 답하므로, 응답을 못 받은 손님이 다시 물어도 두 번 팔리지 않는다 */
static void start_book(struct conn *c)
{
    struct exchange *ex = c->ex;
    char body[64] = "", who_text[18] = "", end;
    int seat = 0;
    unsigned long long who = 0;
    (void)body_text(c, body, sizeof body); /* 너무 길면 비어 있고, 아래 길이 비교에서 걸린다 */
    /* 쓸 수 있는 글자만 있는지 먼저 본다. sscanf는 앞의 공백·부호를 봐주고 NUL 뒤를 보지 못한다 */
    if (strspn(body, "seathwo=&0123456789abcdef") != (size_t)ex->body.size ||
        sscanf(body, "seat=%2d&who=%17s%c", &seat, who_text, &end) != 2 || seat < 1 || seat > SEATS ||
        !parse_who(who_text, &who)) {
        json(c, 400, "{\"error\":\"seat=1~60&who=16진수 형식이어야 합니다\"}", NO_STORE);
        return;
    }
    struct seat_st *st = calloc(1, sizeof *st);
    ex->u.app = st;
    if (!st) {
        exchange_error(c, 500, NULL);
        return;
    }
    unsigned long long owner = 0;
    if (!atomic_compare_exchange_strong(&g_seat[seat - 1], &owner, who) && owner != who) {
        char out[64];
        snprintf(out, sizeof out, "{\"seat\":%d,\"by\":%d}", seat, my_port(c));
        json(c, 409, out, NO_STORE);
        return;
    }
    st->again = owner == who;
    st->seat = seat;
    st->who = who;
    ex->deadline = c->loop->now + PAY_MS; /* 기다리는 동안 루프는 다른 연결을 처리한다 */
}

static void start_show(struct conn *c)
{
    char out[256], at[16];
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(at, sizeof at, "%H:%M:%S", &tm);
    snprintf(out, sizeof out,
             "{\"title\":\"커널의 밤\",\"place\":\"정글 소극장\",\"time\":\"오늘 저녁 8시\",\"made\":\"%s.%03d\",\"by\":%d}",
             at, (int)(ts.tv_nsec / 1000000), my_port(c));
    json(c, 200, out, "Cache-Control: max-age=0, s-maxage=5\r\n"); /* 브라우저는 매번 묻고, 프록시만 5초 저장 */
}

static void serve(struct conn *c)
{
    struct exchange *ex = c->ex;
    const char *path = ex->path;
    char out[128], board[SEATS + 1];
    bool book = !strcmp(path, "/api/book"), reset = !strcmp(path, "/api/reset"), show = !strcmp(path, "/api/show");
    bool work = !strcmp(path, "/api/work") || !strcmp(path, "/rr/work") || !strcmp(path, "/lc/work");
    bool post = ex->req.method == M_POST, get = ex->req.method == M_GET || ex->req.method == M_HEAD;
    if (!(get && !book && !reset) && !(post && (book || reset || show || work))) {
        exchange_error(c, 405, book || reset ? "Allow: POST\r\n" : show || work ? "Allow: GET, HEAD, POST\r\n" : "Allow: GET, HEAD\r\n");
    } else if (post && (reset || show) && !from_page(c)) {
        json(c, 403, "{\"error\":\"데모 화면에서 온 요청이 아닙니다\"}", NO_STORE);
    } else if (book) {
        start_book(c);
    } else if (reset) {
        for (int i = 0; i < SEATS; i++)
            atomic_store(&g_seat[i], 0);
        json(c, 200, "{}", NO_STORE);
    } else if (work || (show && post)) {
        snprintf(out, sizeof out, "{\"by\":%d}", my_port(c));
        json(c, 200, out, NO_STORE);
    } else if (show) {
        start_show(c);
    } else if (!strcmp(path, "/api/events")) {
        start_events(c);
    } else if (!strcmp(path, "/api/seats")) {
        read_board(0, board);
        snprintf(out, sizeof out, "{\"board\":\"%s\",\"by\":%d}", board, my_port(c));
        json(c, 200, out, NO_STORE);
    } else if (!strcmp(path, "/api/health")) {
        json(c, 200, "{}", NO_STORE);
    } else {
        exchange_error(c, 404, NULL);
    }
}

static void seats_start(struct conn *c)
{
    const char *path = c->ex->path;
    enum fault f = fault_now(c);
    if (!strncmp(path, "/fault/", 7)) {
        control(c);
        return;
    }
    /* 멈춤은 헬스 체크까지 답하지 않는다. 나머지 고장은 "살아 있다고 답하지만 일은 제대로 못 하는" 상태 */
    if (f != FT_HANG && (!strcmp(path, "/api/health") || !strcmp(path, "/api/events")))
        f = FT_OK;
    if (f == FT_HANG || f == FT_SLOW)
        c->ex->deadline = c->loop->now; /* 아직 시작하지 않는다. seats_timeout이 때가 되면 시작한다 */
    else if (f == FT_BUSY)
        exchange_error(c, 503, "Retry-After: 1\r\n");
    else if (f == FT_DROP && strcmp(path, "/api/book"))
        exchange_abort(c);
    else
        serve(c); /* 예매의 drop은 자리를 판 뒤에 끊는다: 손님은 팔렸는지 모르는 채로 남는다 */
}

/* 0.25~0.5초마다 (ex->deadline이 지난 시각이므로) */
static void seats_timeout(struct conn *c)
{
    struct exchange *ex = c->ex;
    struct seat_st *st = ex->u.app;
    enum fault f = fault_now(c);
    bool draining = c->loop->draining;
    if ((!st || st->push) && c->rd_eof) {
        exchange_abort(c); /* 손님이 떠났다. 끝나지 않는 응답이거나 고장 때문에 미룬 요청이라, 여기서 알아채야 자리가 빈다 */
    } else if (f == FT_HANG && !draining) {
        return;
    } else if (!st) {
        if (f == FT_SLOW && !draining && c->loop->now - ex->start_ms < SLOW_MS)
            return;
        ex->deadline = 0;
        serve(c);
    } else if (!st->push && f == FT_DROP) {
        exchange_abort(c);
    } else if (!st->push) {
        char out[64];
        bool mine = atomic_load(&g_seat[st->seat - 1]) == st->who; /* 결제하는 사이 누가 좌석을 비웠을 수 있다 */
        snprintf(out, sizeof out, "{\"seat\":%d,\"again\":%s,\"by\":%d}", st->seat, st->again ? "true" : "false", my_port(c));
        json(c, mine ? 200 : 409, out, NO_STORE);
    } else if (draining) {
        exchange_finish(c); /* 종료 중: 여기서 끝내면 브라우저가 다시 연결한다 */
    } else {
        push(c);
    }
}

static void seats_stop(struct conn *c)
{
    free(c->ex->u.app);
    c->ex->u.app = NULL;
}

static const struct handler handler = {.name = "seats", .start = seats_start, .timeout = seats_timeout, .stop = seats_stop};
const struct handler *const app_handler = &handler;
