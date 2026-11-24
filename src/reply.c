#include "reply.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define OUT_SOFT 65536 /* 본문은 여기까지만 쌓는다. 넘으면 생산을 멈춘다 (클라이언트 속도에 맞추기) */
#define CHUNK_OVERHEAD 16

const char *status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 508: return "Loop Detected";
    default: return "Unknown";
    }
}

bool reply_start(struct reply *r, struct buf *out, int status, long long length,
                 const char *extra, size_t extra_n, unsigned flags)
{
    bool no_body = r->head_only || status == 204 || status == 304;
    r->status = status;
    if (no_body)
        r->framing = FRAME_NONE;
    else if (length >= 0)
        r->framing = FRAME_LENGTH;
    else if (r->minor >= 1)
        r->framing = FRAME_CHUNKED;
    else
        r->framing = FRAME_CLOSE;
    if (r->framing == FRAME_CLOSE)
        r->keep = false; /* 연결을 닫는 것이 곧 본문의 끝 */
    r->left = r->framing == FRAME_LENGTH ? length : 0;

    char date[64];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof date, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
    char frame[64] = "";
    if (length >= 0 && status != 204 && status != 304)
        snprintf(frame, sizeof frame, "Content-Length: %lld\r\n", length);
    else if (r->framing == FRAME_CHUNKED)
        snprintf(frame, sizeof frame, "Transfer-Encoding: chunked\r\n");

    char head[512];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\n%s%s%sConnection: %s\r\n", status,
                     status_text(status), flags & REPLY_HAS_DATE ? "" : date,
                     flags & REPLY_HAS_SERVER ? "" : "Server: jiho-webserver\r\n", frame,
                     r->keep ? "keep-alive" : "close");
    if ((size_t)n + extra_n + 2 > buf_room(out) || !buf_append(out, head, (size_t)n) ||
        !buf_append(out, extra, extra_n) || !buf_append(out, "\r\n", 2))
        return false;
    r->started = true;
    return true;
}

size_t reply_room(const struct reply *r, const struct buf *out)
{
    size_t overhead = r->framing == FRAME_CHUNKED ? CHUNK_OVERHEAD : 0;
    size_t used = buf_len(out) + overhead;
    return used < OUT_SOFT ? OUT_SOFT - used : 0;
}

void reply_write(struct reply *r, struct buf *out, const char *p, size_t n)
{
    if (r->framing == FRAME_NONE)
        return;
    if (r->framing == FRAME_LENGTH && (long long)n > r->left)
        n = (size_t)r->left; /* 약속보다 많이 보내면 다음 응답 자리에 섞인다 */
    if (n == 0)
        return; /* 크기 0인 조각은 "끝"이라는 뜻이므로 쓰면 안 된다 */
    bool ok = !r->broken;
    if (r->framing == FRAME_CHUNKED)
        ok = ok && buf_printf(out, "%zx\r\n", n);
    ok = ok && buf_append(out, p, n);
    if (r->framing == FRAME_CHUNKED)
        ok = ok && buf_append(out, "\r\n", 2);
    if (!ok) {
        r->broken = true;
        return;
    }
    if (r->framing == FRAME_LENGTH)
        r->left -= (long long)n;
    r->sent += (long long)n;
}

bool reply_finish(struct reply *r, struct buf *out)
{
    if (r->broken || (r->framing == FRAME_LENGTH && r->left > 0))
        return false;
    if (r->framing == FRAME_CHUNKED && !buf_append(out, "0\r\n\r\n", 5))
        return false;
    r->finished = true;
    return true;
}

bool reply_text(struct reply *r, struct buf *out, int status, const char *type,
                const char *body, size_t n, const char *extra)
{
    char head[1024];
    int hn = snprintf(head, sizeof head, "Content-Type: %s\r\n%s", type, extra ? extra : "");
    if (hn < 0 || (size_t)hn >= sizeof head ||
        !reply_start(r, out, status, (long long)n, head, (size_t)hn, 0))
        return false;
    reply_write(r, out, body, n);
    return reply_finish(r, out);
}
