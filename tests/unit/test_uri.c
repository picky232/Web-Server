#include "test.h"
#include "uri.h"

static struct sl sl(const char *s) { return (struct sl){s, strlen(s)}; }

void test_uri(void)
{
    static const struct {
        const char *in, *want; /* want가 NULL이면 400 */
    } t[] = {
        {"/", "/"}, {"/a", "/a"}, {"/a/", "/a/"}, {"//about", "/about"}, {"/a//b///c", "/a/b/c"},
        {"/./a", "/a"}, {"/a/.", "/a/"}, {"/a/..", "/"}, {"/a/b/..", "/a/"}, {"/a/../b", "/b"},
        {"/a/./b/../c", "/a/c"}, {"/...", "/..."}, {"/a..b", "/a..b"}, {"/%41%7e", "/A~"},
        {"/%20x", "/%20x"}, {"/%e3%81", "/%E3%81"}, {"/a:b@c", "/a:b@c"}, {"/%25", "/%25"},
        {"/..", NULL}, {"/../a", NULL}, {"/a/../../b", NULL}, {"/%2e%2e/a", NULL}, {"/a%2fb", NULL},
        {"/a%5cb", NULL}, {"/%00", NULL}, {"/%0a", NULL}, {"/%4", NULL}, {"/%zz", NULL}, {"/%", NULL},
        {"/a\\b", NULL}, {"/a{b", NULL}, {"/<x>", NULL},
    };
    char out[256];
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int st = uri_normalize(sl(t[i].in), out, sizeof out);
        if (t[i].want)
            CHECK_MSG(st == 0 && !strcmp(out, t[i].want), t[i].in);
        else
            CHECK_MSG(st == 400, t[i].in);
    }
    char small[8];
    CHECK(uri_normalize(sl("/abcdefghijk"), small, sizeof small) == 414);

    struct target g;
    CHECK(uri_split(sl("/a?b=c"), M_GET, &g) == 0 && sl_eq(g.path, "/a") && sl_eq(g.query, "?b=c") && !g.absolute);
    CHECK(uri_split(sl("http://H:1/a?x"), M_GET, &g) == 0 && g.absolute && sl_eq(g.authority, "H:1") && sl_eq(g.path, "/a"));
    CHECK(uri_split(sl("HTTP://h"), M_GET, &g) == 0 && sl_eq(g.path, "/"));
    CHECK(uri_split(sl("http://h?q"), M_GET, &g) == 0 && sl_eq(g.path, "/") && sl_eq(g.query, "?q"));
    CHECK(uri_split(sl("*"), M_OPTIONS, &g) == 0 && g.asterisk);
    CHECK(uri_split(sl("*"), M_GET, &g) == 400);
    CHECK(uri_split(sl("ftp://h/"), M_GET, &g) == 400);
    CHECK(uri_split(sl("http:///a"), M_GET, &g) == 400);
    CHECK(uri_split(sl("a/b"), M_GET, &g) == 400);
    CHECK(uri_split(sl("/a#b"), M_GET, &g) == 400);

    CHECK(uri_split(sl("h:443"), M_CONNECT, &g) == 0 && sl_eq(g.authority, "h:443"));
    CHECK(uri_split(sl("h"), M_CONNECT, &g) == 400 && uri_split(sl("h:"), M_CONNECT, &g) == 400);
    CHECK(uri_split(sl("http://h"), M_OPTIONS, &g) == 0 && g.asterisk);

    char host[64];
    int port;
    CHECK(uri_host(sl("ADMIN.localhost:8080"), 80, host, sizeof host, &port) == 0 && !strcmp(host, "admin.localhost") && port == 8080);
    CHECK(uri_host(sl("x"), 80, host, sizeof host, &port) == 0 && port == 80);
    CHECK(uri_host(sl("[::1]:81"), 80, host, sizeof host, &port) == 0 && !strcmp(host, "[::1]") && port == 81);
    const char *bad[] = {"", "a b", "a/b", "u@h", "x:0", "x:99999", "x:y", "[::1", "a,b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(uri_host(sl(bad[i]), 80, host, sizeof host, &port) == 400, bad[i]);

    CHECK(uri_decode("/a%20b%25", out, sizeof out) && !strcmp(out, "/a b%"));
    CHECK(!uri_decode("/abcdefghijk", small, sizeof small));
}
