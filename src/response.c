#include "response.h"
#include "net.h"

#include <stdio.h>
#include <time.h>

const char *status_text(int status)
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
    default: return "Unknown";
    }
}

int send_head(int fd, int status, const char *ctype, long long length, int keep_alive,
              const char *extra)
{
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    char ct[160] = "", cl[48] = "";
    if (ctype)
        snprintf(ct, sizeof ct, "Content-Type: %s\r\n", ctype);
    if (length >= 0)
        snprintf(cl, sizeof cl, "Content-Length: %lld\r\n", length);

    char head[4096];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: jiho-webserver\r\n"
                     "%s%s"
                     "Connection: %s\r\n"
                     "%s\r\n",
                     status, status_text(status), date, ct, cl,
                     keep_alive ? "keep-alive" : "close", extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof head)
        return -1;
    return net_send_all(fd, head, (size_t)n);
}

int send_body(int fd, int status, const char *ctype, const void *body, size_t len,
              int keep_alive, const char *extra, int head_only)
{
    if (send_head(fd, status, ctype, (long long)len, keep_alive, extra) < 0)
        return -1;
    return head_only ? 0 : net_send_all(fd, body, len);
}

int send_error(int fd, int status, int keep_alive, const char *extra, int head_only)
{
    char body[256];
    int n = snprintf(body, sizeof body, "<!doctype html><title>%d %s</title><h1>%d %s</h1>\n",
                     status, status_text(status), status, status_text(status));
    return send_body(fd, status, "text/html; charset=utf-8", body, (size_t)n, keep_alive,
                     extra, head_only);
}
