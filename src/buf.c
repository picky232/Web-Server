#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(struct buf *b, size_t max)
{
    memset(b, 0, sizeof *b);
    b->max = max;
}

void buf_free(struct buf *b)
{
    free(b->data);
    buf_init(b, b->max);
}

char *buf_reserve(struct buf *b, size_t n)
{
    if (n > buf_room(b))
        return NULL;
    if (b->data && b->w + n <= b->cap)
        return b->data + b->w;
    if (b->r > 0) { /* 앞쪽의 다 쓴 자리를 당겨서 메운다 */
        memmove(b->data, b->data + b->r, buf_len(b));
        b->w -= b->r;
        b->r = 0;
        if (b->w + n <= b->cap)
            return b->data + b->w;
    }
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->w + n)
        cap *= 2;
    if (cap > b->max)
        cap = b->max;
    char *p = realloc(b->data, cap);
    if (!p)
        return NULL;
    b->data = p;
    b->cap = cap;
    return b->data + b->w;
}

void buf_commit(struct buf *b, size_t n)
{
    b->w += n;
}

void buf_consume(struct buf *b, size_t n)
{
    b->r += n;
    if (b->r == b->w)
        b->r = b->w = 0;
}

bool buf_append(struct buf *b, const void *p, size_t n)
{
    if (n == 0)
        return true;
    char *dst = buf_reserve(b, n);
    if (!dst)
        return false;
    memcpy(dst, p, n);
    buf_commit(b, n);
    return true;
}

bool buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if ((size_t)n < sizeof tmp)
        return buf_append(b, tmp, (size_t)n);
    char *dst = buf_reserve(b, (size_t)n + 1);
    if (!dst)
        return false;
    va_start(ap, fmt);
    vsnprintf(dst, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_commit(b, (size_t)n);
    return true;
}
