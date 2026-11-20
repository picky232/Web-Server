#include "unity.h"
#include "config.h"

#include <string.h>

static app_config c;
static char err[256];

void setUp(void)    { config_defaults(&c); err[0] = '\0'; }
void tearDown(void) {}

static void assert_fail(const char *text, const char *needle) {
    config_defaults(&c);
    TEST_ASSERT_EQUAL_INT(-1, config_parse(&c, text, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, needle), err);
}

void test_defaults(void) {
    TEST_ASSERT_EQUAL_INT(8080, c.port);
    TEST_ASSERT_EQUAL_STRING("./public", c.docroot);
    TEST_ASSERT_EQUAL_INT(0, c.nroutes);
}

void test_full_config(void) {
    const char *t =
        "# 전체 설정\n"
        "listen 9090\n"
        "docroot /srv/www   # 주석\n"
        "timeout_header 5s\n"
        "timeout_idle 20\n"
        "timeout_connect 500ms\n"
        "timeout_upstream 7s\n"
        "\n"
        "route /api/ api\n"            /* upstream 보다 먼저 선언해도 된다 */
        "route /static/ files strip\n"
        "upstream api {\n"
        "    server 127.0.0.1:8001\n"
        "    server 127.0.0.1:8002\n"
        "}\n"
        "upstream files {\n"
        "    server 127.0.0.1:8003\n"
        "}\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, config_parse(&c, t, err, sizeof err), err);
    TEST_ASSERT_EQUAL_INT(9090, c.port);
    TEST_ASSERT_EQUAL_STRING("/srv/www", c.docroot);
    TEST_ASSERT_EQUAL_INT(5000, c.header_timeout_ms);
    TEST_ASSERT_EQUAL_INT(20000, c.idle_timeout_ms);
    TEST_ASSERT_EQUAL_INT(500, c.connect_timeout_ms);
    TEST_ASSERT_EQUAL_INT(7000, c.upstream_timeout_ms);
    TEST_ASSERT_EQUAL_INT(2, c.nups);
    TEST_ASSERT_EQUAL_INT(2, c.ups[0].nservers);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1:8002", c.ups[0].servers[1].label);
    TEST_ASSERT_EQUAL_UINT16(8002, ntohs(c.ups[0].servers[1].addr.sin_port));
    TEST_ASSERT_EQUAL_INT(2, c.nroutes);
    TEST_ASSERT_EQUAL_INT(0, c.routes[0].up);
    TEST_ASSERT_EQUAL_INT(0, c.routes[0].strip);
    TEST_ASSERT_EQUAL_INT(1, c.routes[1].up);
    TEST_ASSERT_EQUAL_INT(1, c.routes[1].strip);
    TEST_ASSERT_EQUAL_UINT(8, c.routes[1].plen);
}

void test_pool_defaults_and_directives(void) {
    TEST_ASSERT_EQUAL_INT(16, c.pool_max);
    TEST_ASSERT_EQUAL_INT(5000, c.pool_idle_ms);
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "pool_size 4\ntimeout_pool 2s\n", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(4, c.pool_max);
    TEST_ASSERT_EQUAL_INT(2000, c.pool_idle_ms);
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "pool_size 0\n", err, sizeof err));  /* 0: 풀 비활성 */
    TEST_ASSERT_EQUAL_INT(0, c.pool_max);
}

void test_pool_invalid_values(void) {
    assert_fail("pool_size -1\n", "invalid pool_size");
    assert_fail("pool_size abc\n", "invalid pool_size");
    assert_fail("pool_size 5000\n", "invalid pool_size");
    assert_fail("timeout_pool 0\n", "invalid duration");
}

void test_empty_and_comment_only(void) {
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "# nothing\n\n   \n", err, sizeof err));
}

void test_last_line_without_newline(void) {
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "listen 1234", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(1234, c.port);
}

void test_errors_report_line_numbers(void) {
    assert_fail("listen 80\nbogus 1\n", "line 2");
}

void test_invalid_values(void) {
    assert_fail("listen 0\n", "invalid port");
    assert_fail("listen 70000\n", "invalid port");
    assert_fail("listen abc\n", "invalid port");
    assert_fail("timeout_idle -5\n", "invalid duration");
    assert_fail("timeout_idle 5m\n", "invalid duration");
    assert_fail("timeout_idle 0\n", "invalid duration");
}

void test_route_errors(void) {
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nroute api a\n", "must start with '/'");
    assert_fail("route /x nope\n", "unknown upstream");
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nroute /x a bogus\n", "unknown route option");
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nroute /x a strip strip\n", "unknown route option");
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nroute /x\n", "unknown or malformed");
}

void test_cache_directives(void) {
    TEST_ASSERT_EQUAL_UINT(0, c.cache_max_bytes);           /* 기본은 꺼짐 */
    TEST_ASSERT_EQUAL_UINT(1024 * 1024, c.cache_max_object);
    TEST_ASSERT_EQUAL_INT(0, c.cache_ttl_ms);
    const char *t =
        "cache_size 32m\ncache_max_object 64k\ncache_ttl 10s\n"
        "upstream a {\n server 127.0.0.1:1\n}\n"
        "route /a/ a cache strip\nroute /b/ a strip\nroute /c/ a cache\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, config_parse(&c, t, err, sizeof err), err);
    TEST_ASSERT_EQUAL_UINT(32u * 1024 * 1024, c.cache_max_bytes);
    TEST_ASSERT_EQUAL_UINT(64u * 1024, c.cache_max_object);
    TEST_ASSERT_EQUAL_INT(10000, c.cache_ttl_ms);
    TEST_ASSERT_EQUAL_INT(1, c.routes[0].cache);   /* 옵션 순서 무관 */
    TEST_ASSERT_EQUAL_INT(1, c.routes[0].strip);
    TEST_ASSERT_EQUAL_INT(0, c.routes[1].cache);
    TEST_ASSERT_EQUAL_INT(1, c.routes[1].strip);
    TEST_ASSERT_EQUAL_INT(1, c.routes[2].cache);
    TEST_ASSERT_EQUAL_INT(0, c.routes[2].strip);
}

void test_cache_size_units_and_zero_ttl(void) {
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "cache_size 4096\n", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT(4096, c.cache_max_bytes);
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "cache_size 2G\n", err, sizeof err));
    TEST_ASSERT_EQUAL_UINT((size_t)2 << 30, c.cache_max_bytes);
    TEST_ASSERT_EQUAL_INT(0, config_parse(&c, "cache_ttl 5s\ncache_ttl 0\n", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, c.cache_ttl_ms);
}

void test_cache_invalid_values(void) {
    assert_fail("cache_size abc\n", "invalid cache_size");
    assert_fail("cache_size -1\n", "invalid cache_size");
    assert_fail("cache_size 10x\n", "invalid cache_size");
    assert_fail("cache_size 99999999999g\n", "invalid cache_size");
    assert_fail("cache_max_object 0\n", "invalid cache_max_object");
    assert_fail("cache_ttl 5m\n", "invalid duration");
}

void test_cache_route_requires_cache_size(void) {
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nroute /x/ a cache\n", "cache_size is 0");
}

void test_upstream_block_errors(void) {
    assert_fail("upstream a {\n", "unterminated");
    assert_fail("upstream a {\n}\n", "no servers");
    assert_fail("upstream a {\n server 127.0.0.1\n}\n", "invalid server");
    assert_fail("upstream a {\n server 127.0.0.1:99999\n}\n", "invalid server");
    assert_fail("upstream a {\n listen 80\n}\n", "inside upstream");
    assert_fail("upstream a {\n server 127.0.0.1:1\n}\nupstream a {\n server 127.0.0.1:2\n}\n", "duplicate");
}

void test_load_missing_file(void) {
    TEST_ASSERT_EQUAL_INT(-1, config_load(&c, "/nonexistent/file.conf", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "cannot open"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_full_config);
    RUN_TEST(test_pool_defaults_and_directives);
    RUN_TEST(test_pool_invalid_values);
    RUN_TEST(test_empty_and_comment_only);
    RUN_TEST(test_last_line_without_newline);
    RUN_TEST(test_errors_report_line_numbers);
    RUN_TEST(test_invalid_values);
    RUN_TEST(test_route_errors);
    RUN_TEST(test_cache_directives);
    RUN_TEST(test_cache_size_units_and_zero_ttl);
    RUN_TEST(test_cache_invalid_values);
    RUN_TEST(test_cache_route_requires_cache_size);
    RUN_TEST(test_upstream_block_errors);
    RUN_TEST(test_load_missing_file);
    return UNITY_END();
}
