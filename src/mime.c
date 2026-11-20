#include "mime.h"

#include <string.h>

static const struct { const char *ext; const char *type; } k_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "mjs",  "text/javascript; charset=utf-8" },
    { "json", "application/json; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "csv",  "text/csv; charset=utf-8" },
    { "xml",  "application/xml; charset=utf-8" },
    { "svg",  "image/svg+xml" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "webp", "image/webp" },
    { "ico",  "image/x-icon" },
    { "woff", "font/woff" },
    { "woff2","font/woff2" },
    { "pdf",  "application/pdf" },
    { "wasm", "application/wasm" },
    { "mp4",  "video/mp4" },
    { "mp3",  "audio/mpeg" },
};

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

const char *mime_for_path(const char *path) {
    /* 마지막 '/' 이후 구간에서만 확장자를 찾는다 ("/a.b/c" 의 ".b" 는 확장자가 아님) */
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');
    if (!dot || dot == base) return "application/octet-stream";  /* 확장자 없음, ".env" 같은 dotfile */
    dot++;
    for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++)
        if (ieq(dot, k_table[i].ext)) return k_table[i].type;
    return "application/octet-stream";
}
