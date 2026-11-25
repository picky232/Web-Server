#include "mime.h"

#include <string.h>
#include <strings.h>

const char *mime_type(const char *path)
{
    static const struct {
        const char *ext, *type;
    } table[] = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},   {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"}, {"json", "application/json"},
        {"txt", "text/plain; charset=utf-8"}, {"md", "text/markdown; charset=utf-8"},
        {"xml", "application/xml"},           {"png", "image/png"},
        {"jpg", "image/jpeg"},                {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},                 {"svg", "image/svg+xml"},
        {"webp", "image/webp"},               {"ico", "image/x-icon"},
        {"pdf", "application/pdf"},           {"woff2", "font/woff2"},
        {"mp4", "video/mp4"},                 {"wasm", "application/wasm"},
    };
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) /* ".html"처럼 점으로 시작하는 이름은 확장자가 아니다 */
        for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
            if (!strcasecmp(dot + 1, table[i].ext))
                return table[i].type;
    return "application/octet-stream";
}
