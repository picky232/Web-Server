/*
 * http_response.c — 응답을 바이트로 만들어 send_all() 로 보낸다.
 *
 * 모든 응답에 Content-Length 를 붙인다. 그래야 클라이언트가 "본문이 어디서
 * 끝나는지" 알 수 있고, 같은 연결로 다음 요청을 보낼 수 있다(keep-alive).
 */
#include "http_response.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "net_io.h"

const char *http_reason_phrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_send_head(int fd, int status, const char *content_type,
                   unsigned long long content_length, bool keep_alive,
                   const char *extra_headers, int64_t deadline_ms)
{
    char date[64];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: " SERVER_NAME "\r\n"
                     "Date: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     status, http_reason_phrase(status), date, content_type,
                     content_length, keep_alive ? "keep-alive" : "close",
                     extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof head)
        return -1;
    return send_all(fd, head, (size_t)n, deadline_ms);
}

int http_send_simple(int fd, int status, const char *content_type,
                     const char *body, size_t body_len, bool keep_alive,
                     bool head_only, const char *extra_headers, int64_t deadline_ms)
{
    int r = http_send_head(fd, status, content_type, body_len, keep_alive,
                           extra_headers, deadline_ms);
    if (r != 0 || head_only || body_len == 0)
        return r;
    return send_all(fd, body, body_len, deadline_ms);
}

int http_send_error(int fd, int status, bool keep_alive, bool head_only,
                    const char *extra_headers, int64_t deadline_ms)
{
    char body[512];
    const char *reason = http_reason_phrase(status);
    int n = snprintf(body, sizeof body,
                     "<!doctype html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>" SERVER_NAME "</p></body></html>\n",
                     status, reason, status, reason);
    if (n < 0)
        return -1;
    return http_send_simple(fd, status, "text/html; charset=utf-8", body, (size_t)n,
                            keep_alive, head_only, extra_headers, deadline_ms);
}
