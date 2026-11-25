#ifndef BUF_H
#define BUF_H

#include <stdbool.h>
#include <stddef.h>

/* 상한이 있는 바이트 버퍼. 처음 쓸 때 할당하므로 조용한 연결은 메모리를 쓰지 않는다 */
struct buf {
    char *data;
    size_t cap;  /* 지금 할당된 크기 */
    size_t max;  /* 절대 넘지 않는 크기 */
    size_t r, w; /* [r, w)가 아직 안 쓴 바이트 */
};

void buf_init(struct buf *b, size_t max);
void buf_free(struct buf *b);

static inline size_t buf_len(const struct buf *b) { return b->w - b->r; }
static inline size_t buf_room(const struct buf *b) { return b->max - buf_len(b); }
static inline char *buf_head(const struct buf *b) { return b->data + b->r; }

/* n바이트를 쓸 자리를 준다. 상한을 넘거나 메모리가 없으면 NULL. 쓴 뒤 buf_commit */
char *buf_reserve(struct buf *b, size_t n);
void buf_commit(struct buf *b, size_t n);
void buf_consume(struct buf *b, size_t n);
bool buf_append(struct buf *b, const void *p, size_t n); /* 전부 넣거나 아무것도 안 넣는다 */
bool buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
