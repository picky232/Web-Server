#ifndef DEMO_H
#define DEMO_H

/* 데모의 두 앱(seats.c, crowd.c)이 같이 쓰는 것 */
#include "conn.h"

#include <string.h>

#define SEATS 60
#define NO_STORE "Cache-Control: no-store\r\n"

static inline void json(struct conn *c, int status, const char *body, const char *extra)
{
    struct exchange *ex = c->ex;
    if (reply_text(&ex->reply, &c->out, status, "application/json", body, strlen(body), extra))
        exchange_finish(c);
    else
        exchange_error(c, 500, NULL);
}

/* 데모 화면이 보낸 요청인가. 브라우저는 POST마다 어느 페이지가 보냈는지(Origin)를 적고, 페이지는 그 값을 바꾸지 못한다.
   이것을 안 보면 다른 사이트를 열어 둔 것만으로 좌석이 비워지거나 백엔드가 죽는다 */
static inline bool from_page(const struct conn *c)
{
    const struct field *o = fields_find(&c->ex->req.h, "Origin");
    return o && (sl_eq(o->value, "http://localhost:9000") || sl_eq(o->value, "http://127.0.0.1:9000"));
}

/* 짧은 요청 본문을 문자열로. 안 들어가면 false */
static inline bool body_text(const struct conn *c, char *out, size_t cap)
{
    const struct spool *b = &c->ex->body;
    memset(out, 0, cap);
    return b->size > 0 && b->size < (long long)cap && spool_read(b, 0, out, cap - 1) == (ssize_t)b->size;
}

#endif
