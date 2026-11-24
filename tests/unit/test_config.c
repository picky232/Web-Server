#include "config.h"
#include "mime.h"
#include "route.h"
#include "static.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static char err[512];

static bool line(struct config *c, const char *s)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    return config_line(c, buf, err, sizeof err);
}

static const char *find(struct config *c, const char *host, const char *path)
{
    const struct route *r = route_find(c, host, path);
    return r ? r->pool->name : "없음";
}

void test_config(void)
{
    struct config *c = malloc(sizeof *c);
    config_defaults(c);
    CHECK(line(c, ""));
    CHECK(line(c, "   # 주석만"));
    CHECK(line(c, "listen 127.0.0.1:9000  # 뒤 주석") && net_port(&c->listen) == 9000);
    CHECK(line(c, "listen [::1]:9001") && net_port(&c->listen) == 9001);
    CHECK(line(c, "max_body 2m") && c->max_body == 2 << 20);
    CHECK(line(c, "timeout_head 1500ms") && c->head_ms == 1500);
    CHECK(line(c, "timeout_idle 3s") && c->idle_ms == 3000);
    CHECK(line(c, "loops auto") && c->loops == 0);
    CHECK(line(c, "loops 4") && c->loops == 4);
    CHECK(line(c, "access_log off") && c->access_log[0] == '\0');
    CHECK(line(c, "pool a 127.0.0.1:1 127.0.0.1:2") && c->pools[0].n == 2 && c->pools[0].policy == LEAST_CONN);
    CHECK(line(c, "pool b round_robin max=7 health=/ok 127.0.0.1:3"));
    CHECK(c->pools[1].policy == ROUND_ROBIN && c->pools[1].max_inflight == 7 && !strcmp(c->pools[1].health, "/ok"));
    CHECK(line(c, "pool host 127.0.0.1:4") && line(c, "pool sub 127.0.0.1:5"));
    const char *bad[] = {"listen", "listen 8080", "listen a b", "nope 1", "max_body 1x", "max_body -1", "timeout_head 10",
                         "timeout_head 0s", "loops 0", "loops x", "pool a 127.0.0.1:9", "pool", "pool z", "pool z 127.0.0.1",
                         "pool z max=0 127.0.0.1:1", "route * /x proxy none", "route * x proxy a", "route * /x nope a",
                         "route * /x static /no/such/dir", "route * /x"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_MSG(!line(c, bad[i]), bad[i]);

    CHECK(line(c, "route * /api/ proxy a"));
    CHECK(line(c, "route * /api/v2 proxy b"));
    CHECK(line(c, "route Admin.Localhost / proxy host"));
    CHECK(line(c, "route admin.localhost /api proxy sub"));
    CHECK(!strcmp(find(c, "x", "/api"), "a"));
    CHECK(!strcmp(find(c, "x", "/api/"), "a"));
    CHECK(!strcmp(find(c, "x", "/api/x"), "a"));
    CHECK(!strcmp(find(c, "x", "/apix"), "없음")); /* 조각 경계 */
    CHECK(!strcmp(find(c, "x", "/api/v2/y"), "b")); /* 가장 긴 접두사 */
    CHECK(!strcmp(find(c, "x", "/api/v22"), "a"));
    CHECK(!strcmp(find(c, "x", "/"), "없음"));
    CHECK(!strcmp(find(c, "admin.localhost", "/zzz"), "host"));
    CHECK(!strcmp(find(c, "admin.localhost", "/api/v2"), "sub")); /* 호스트가 접두사 길이보다 먼저 */
    config_free(c);

    c = malloc(sizeof *c); /* 호스트 규칙에 맞는 경로가 없으면 * 규칙으로 내려간다 */
    config_defaults(c);
    CHECK(line(c, "pool a 127.0.0.1:1") && line(c, "pool b 127.0.0.1:2"));
    CHECK(line(c, "route h /only proxy a") && line(c, "route * / proxy b"));
    CHECK(!strcmp(find(c, "h", "/only/x"), "a") && !strcmp(find(c, "h", "/other"), "b"));
    config_free(c);

    static const struct {
        const char *ip;
        bool priv;
    } ips[] = {{"127.0.0.1", true}, {"10.1.2.3", true}, {"172.16.0.1", true}, {"172.32.0.1", false}, {"192.168.0.1", true},
               {"169.254.169.254", true}, {"100.64.0.1", true}, {"0.0.0.0", true}, {"224.0.0.1", true}, {"8.8.8.8", false},
               {"::1", true}, {"::", true}, {"fe80::1", true}, {"fd00::1", true}, {"::ffff:10.0.0.1", true}, {"::ffff:8.8.8.8", false},
               {"2001:4860:4860::8888", false}, {"ff02::1", true}, {"64:ff9b::a00:1", true}};
    for (size_t i = 0; i < sizeof ips / sizeof ips[0]; i++) {
        struct addr a;
        CHECK(net_parse_hostport(ips[i].ip, "80", true, &a));
        CHECK_MSG(net_is_private(&a) == ips[i].priv, ips[i].ip);
    }

    static const char *mimes[][2] = {
        {"/a.html", "text/html; charset=utf-8"}, {"/a.HTML", "text/html; charset=utf-8"}, {"/a.css", "text/css; charset=utf-8"},
        {"/a.js", "text/javascript; charset=utf-8"}, {"/a.png", "image/png"}, {"/a.jpg", "image/jpeg"}, {"/a.JPEG", "image/jpeg"},
        {"/a.tar.gz", "application/octet-stream"}, {"/a", "application/octet-stream"}, {"/dir.html/a", "application/octet-stream"},
        {"/.html", "application/octet-stream"}, {"/a.", "application/octet-stream"}};
    for (size_t i = 0; i < sizeof mimes / sizeof mimes[0]; i++)
        CHECK_MSG(!strcmp(mime_type(mimes[i][0]), mimes[i][1]), mimes[i][0]);

    CHECK(status_from_errno(ENOENT) == 404 && status_from_errno(ENOTDIR) == 404 && status_from_errno(ENAMETOOLONG) == 404);
    CHECK(status_from_errno(EACCES) == 403 && status_from_errno(ELOOP) == 403);
    CHECK(status_from_errno(EMFILE) == 503 && status_from_errno(ENFILE) == 503);
    CHECK(status_from_errno(EIO) == 500);

    CHECK(etag_matches((struct sl){"\"a\"", 3}, "\"a\""));
    CHECK(etag_matches((struct sl){"\"x\", W/\"a\"", 10}, "\"a\""));
    CHECK(etag_matches((struct sl){"*", 1}, "\"a\""));
    CHECK(!etag_matches((struct sl){"\"ab\"", 4}, "\"a\""));
    CHECK(!etag_matches((struct sl){"", 0}, "\"a\""));

    char dir[] = "/tmp/webserver-test-XXXXXX", cmd[512];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(cmd, sizeof cmd, "cd %s && mkdir -p d && echo x > d/f && ln -s d/f link && ln -s d dirlink && ln -s /etc up && echo s > .env && mkfifo fifo", dir);
    CHECK(system(cmd) == 0);
    int root = open(dir, O_RDONLY | O_DIRECTORY);
    struct stat st;
    int fd = open_under(root, "/d/f", &st);
    CHECK(fd >= 0 && S_ISREG(st.st_mode));
    close(fd);
    fd = open_under(root, "/d", &st);
    CHECK(fd >= 0 && S_ISDIR(st.st_mode));
    close(fd);
    const char *no[] = {"/link", "/dirlink/f", "/up/passwd", "/.env", "/none", "/d/f/x", "/d/.hidden"};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        CHECK_MSG(open_under(root, no[i], &st) < 0, no[i]);
    fd = open_under(root, "/fifo", &st); /* 멈추지 않아야 한다 */
    CHECK(fd >= 0 && !S_ISREG(st.st_mode));
    close(fd);
    close(root);
    snprintf(cmd, sizeof cmd, "rm -r %s", dir);
    CHECK(system(cmd) == 0);
}
