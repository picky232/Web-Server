#include "str.h"

#include <string.h>
#include <strings.h>
#include <time.h>

bool sl_eq(struct sl a, const char *b)
{
    return strlen(b) == a.n && !memcmp(a.p, b, a.n);
}

bool sl_eqi(struct sl a, const char *b)
{
    return strlen(b) == a.n && !strncasecmp(a.p, b, a.n);
}

struct sl sl_trim(struct sl s)
{
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) {
        s.p++;
        s.n--;
    }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t'))
        s.n--;
    return s;
}

bool sl_list_has(struct sl list, const char *token)
{
    while (list.n) {
        const char *comma = memchr(list.p, ',', list.n);
        size_t n = comma ? (size_t)(comma - list.p) : list.n;
        if (sl_eqi(sl_trim((struct sl){list.p, n}), token))
            return true;
        size_t skip = comma ? n + 1 : n;
        list.p += skip;
        list.n -= skip;
    }
    return false;
}

bool sl_to_ll(struct sl s, long long *out)
{
    if (s.n == 0 || s.n > 18) /* 18자리까지는 long long을 넘지 않는다 */
        return false;
    long long v = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9')
            return false;
        v = v * 10 + (s.p[i] - '0');
    }
    *out = v;
    return true;
}

bool sl_copy(struct sl s, char *out, size_t cap)
{
    if (s.n >= cap)
        return false;
    memcpy(out, s.p, s.n);
    out[s.n] = '\0';
    return true;
}

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
