#ifndef REPLY_H
#define REPLY_H

#include "buf.h"

enum framing { FRAME_NONE, FRAME_LENGTH, FRAME_CHUNKED, FRAME_CLOSE };

/* 응답은 정적·프록시·CGI 모두 이 문을 거쳐서만 나간다.
   약속한 길이를 여기 한 곳에서 세므로, 더 보내거나 덜 보내고도 연결을 유지하는 일이 없다 */
struct reply {
    int minor;      /* 클라이언트의 HTTP 버전. 1.0에는 chunked를 보내면 안 된다 */
    bool head_only; /* HEAD 요청: 머리만 보낸다 */
    bool keep;      /* 응답 뒤 연결을 유지할 것인가 */
    bool started, finished;
    bool broken;    /* 메모리가 모자라 일부를 못 썼다 → 끝까지 보낸 척하면 안 된다 */
    int status;
    enum framing framing;
    long long left; /* FRAME_LENGTH에서 아직 안 보낸 바이트 */
    long long sent; /* 보낸 본문 바이트 (로그용) */
};

#define REPLY_HAS_DATE 1u   /* extra에 이미 Date가 있다 */
#define REPLY_HAS_SERVER 2u

const char *status_text(int status);

/* 머리를 out에 쓴다. length < 0이면 길이를 모른다는 뜻.
   extra는 "이름: 값\r\n" 줄들. false = out에 자리가 없다 */
bool reply_start(struct reply *r, struct buf *out, int status, long long length,
                 const char *extra, size_t extra_n, unsigned flags);
size_t reply_room(const struct reply *r, const struct buf *out); /* 지금 더 쓸 수 있는 본문 바이트 */
void reply_write(struct reply *r, struct buf *out, const char *p, size_t n); /* n <= reply_room */
bool reply_finish(struct reply *r, struct buf *out); /* false = 약속한 길이를 못 채움 → 연결을 끊어야 한다 */

/* 작은 응답을 한 번에 */
bool reply_text(struct reply *r, struct buf *out, int status, const char *type,
                const char *body, size_t n, const char *extra);

#endif
