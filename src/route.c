#include "route.h"

#include <string.h>

static const struct route *longest(const struct config *c, const char *host, const char *path)
{
    const struct route *best = NULL;
    for (int i = 0; i < c->nroutes; i++) {
        const struct route *r = &c->routes[i];
        if (strcmp(r->host, host) || strncmp(path, r->prefix, r->prefix_len))
            continue;
        char next = path[r->prefix_len];
        if ((next == '\0' || next == '/') && (!best || r->prefix_len > best->prefix_len))
            best = r;
    }
    return best;
}

const struct route *route_find(const struct config *c, const char *host, const char *path)
{
    const struct route *r = longest(c, host, path);
    return r ? r : longest(c, "*", path);
}
