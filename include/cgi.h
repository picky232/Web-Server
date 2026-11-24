#ifndef CGI_H
#define CGI_H

#include "buf.h"
#include "ev.h"
#include "str.h"

#include <sys/types.h>

struct conn;
struct loop;

/* CGI 프로그램의 표준 출력을 읽는 파이프 */
struct cgi_proc {
    struct io io; /* 맨 앞 */
    struct conn *c;
    struct buf in;
};

/* 아직 회수하지 않은 자식 프로세스 */
struct child {
    pid_t pid;
    long long kill_at; /* 이 시각까지 안 끝나면 강제 종료 */
    struct child *next;
};

struct cgi_st {
    struct cgi_proc *p;
    struct child *child;
    bool head_done, eof;
    long long touched;
};

extern const struct handler cgi_handler;

void cgi_event(struct cgi_proc *p);
void cgi_reap(struct loop *l, bool force); /* 끝난 자식을 회수한다 (좀비 방지) */
void cgi_proc_free(struct cgi_proc *p);

/* 시험할 수 있게 떼어 둔 것: CGI 출력의 머리 해석. extra에 넘길 헤더를 모은다 */
size_t cgi_head_end(const char *p, size_t n); /* 빈 줄까지의 길이. 아직이면 0 */
bool cgi_parse_head(const char *p, size_t n, int *status, long long *length, struct buf *extra);
struct exchange;
bool cgi_env(const struct exchange *ex, const char *ip, int port, const char *script, const char *info, struct buf *out);

#endif
