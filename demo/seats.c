/* 좌석 예매 데모 백엔드. 서버 본체에 이 처리기 하나만 끼워 build/seats를 만든다 (make demo).

     GET  /api/show    공연 정보 (프록시가 캐시해도 되는 유일한 응답)
     GET  /api/seats   지금 좌석판
     GET  /api/events  끝나지 않는 응답. 좌석판이 바뀔 때마다 서버가 먼저 보낸다
     POST /api/book    seat=12&who=1a2b   (who: 손님이 스스로 정한 번호표)

   좌석은 파일을 mmap한 메모리에 있다. 그래서 백엔드 여러 개가 같은 좌석판을 보고, 죽었다 다시 떠도 남아 있다 */
#include "app.h"
#include "conn.h"
#include "loop.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEATS 60
#define FILE_SIZE (SEATS * sizeof(atomic_ullong))
#define PAY_MS 250   /* 결제를 흉내 내는 시간. 루프가 제한 시간을 0.25~0.5초마다 보므로 더 잘게는 못 잰다 */
#define BEAT_MS 2000 /* 조용할 때 보내는 빈 신호. 없으면 프록시가 30초 뒤 끊고(timeout_stall), 손님이 떠난 것도 모른다 */
#define NO_STORE "Cache-Control: no-store\r\n"

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
    unsigned long long who;
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

static void json(struct conn *c, int status, const char *body, const char *extra)
{
    struct exchange *ex = c->ex;
    if (reply_text(&ex->reply, &c->out, status, "application/json", body, strlen(body), extra))
        exchange_finish(c);
    else
        exchange_error(c, 500, NULL);
}

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
    if (ex->body.size > 0 && ex->body.size < (long long)sizeof body)
        (void)!spool_read(&ex->body, 0, body, sizeof body - 1);
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
    ex->deadline = c->loop->now + PAY_MS; /* 기다리는 동안 루프는 다른 연결을 처리한다 */
}

static void seats_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    const char *path = ex->path;
    char out[256], board[SEATS + 1];
    bool book = !strcmp(path, "/api/book");
    bool get = ex->req.method == M_GET || ex->req.method == M_HEAD;
    if (book ? ex->req.method != M_POST : !get) {
        exchange_error(c, 405, book ? "Allow: POST\r\n" : "Allow: GET, HEAD\r\n");
    } else if (book) {
        start_book(c);
    } else if (!strcmp(path, "/api/events")) {
        start_events(c);
    } else if (!strcmp(path, "/api/seats")) {
        read_board(0, board);
        snprintf(out, sizeof out, "{\"board\":\"%s\",\"by\":%d}", board, my_port(c));
        json(c, 200, out, NO_STORE);
    } else if (!strcmp(path, "/api/show")) {
        snprintf(out, sizeof out, "{\"title\":\"커널의 밤\",\"place\":\"정글 소극장\",\"time\":\"오늘 저녁 8시\",\"by\":%d}", my_port(c));
        json(c, 200, out, "Cache-Control: max-age=0, s-maxage=5\r\n"); /* 브라우저는 매번 묻고, 프록시만 5초 저장 */
    } else if (!strcmp(path, "/api/health")) {
        json(c, 200, "{}", NO_STORE);
    } else {
        exchange_error(c, 404, NULL);
    }
}

static void seats_timeout(struct conn *c)
{
    struct seat_st *st = c->ex->u.app;
    if (!st->push) {
        char out[64];
        snprintf(out, sizeof out, "{\"seat\":%d,\"again\":%s,\"by\":%d}", st->seat, st->again ? "true" : "false", my_port(c));
        json(c, 200, out, NO_STORE);
    } else if (c->rd_eof) {
        exchange_abort(c); /* 손님이 떠났다. 끝나지 않는 응답이라 여기서 알아채야 자리가 빈다 */
    } else if (c->loop->draining) {
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
