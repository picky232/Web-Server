#ifndef CACHING_H
#define CACHING_H

#include "exchange.h"

bool caching_on(const struct conn *c);  /* 이 요청의 규칙에 cache가 붙어 있나 */
bool caching_lookup(struct conn *c);    /* 저장해 둔 것으로 답했으면 true */
void caching_begin(struct conn *c, const struct http_res *res, const struct buf *extra, unsigned flags);
void caching_add(struct proxy_st *px, struct sl data);
void caching_commit(struct conn *c);
void caching_drop(struct proxy_st *px);
void caching_invalidate(struct conn *c, int status);

#endif
