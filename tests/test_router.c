#include "unity.h"
#include "router.h"

#include <string.h>

static app_config c;
static upstream u;

void setUp(void) {
    config_defaults(&c);
    memset(&u, 0, sizeof u);
    u.nservers = 3;
}
void tearDown(void) {}

static void add_route(const char *prefix, int up) {
    route *r = &c.routes[c.nroutes++];
    snprintf(r->prefix, sizeof r->prefix, "%s", prefix);
    r->plen = strlen(prefix);
    r->up = up;
}

/* ---------------------------- 경로 추출 ---------------------------- */

void test_path_span_origin_form(void) {
    size_t off, plen;
    const char *u1 = "/a/b?x=1#f";
    TEST_ASSERT_EQUAL_INT(0, router_path_span(u1, strlen(u1), &off, &plen));
    TEST_ASSERT_EQUAL_UINT(0, off);
    TEST_ASSERT_EQUAL_UINT(4, plen);
    TEST_ASSERT_EQUAL_INT(0, router_path_span("/", 1, &off, &plen));
    TEST_ASSERT_EQUAL_UINT(1, plen);
}

void test_path_span_absolute_form(void) {
    size_t off, plen;
    const char *u1 = "http://example.com/api/x?q=1";
    TEST_ASSERT_EQUAL_INT(0, router_path_span(u1, strlen(u1), &off, &plen));
    TEST_ASSERT_EQUAL_STRING_LEN("/api/x", u1 + off, plen);
    const char *u2 = "http://example.com";
    TEST_ASSERT_EQUAL_INT(0, router_path_span(u2, strlen(u2), &off, &plen));
    TEST_ASSERT_EQUAL_UINT(0, plen);  /* "/" 로 취급 */
}

void test_path_span_invalid(void) {
    size_t off, plen;
    TEST_ASSERT_EQUAL_INT(-1, router_path_span("noslash", 7, &off, &plen));
    TEST_ASSERT_EQUAL_INT(-1, router_path_span("", 0, &off, &plen));
}

void test_origin_off(void) {
    TEST_ASSERT_EQUAL_UINT(0, router_origin_off("/x", 2));
    const char *a = "http://h/x?y";
    TEST_ASSERT_EQUAL_UINT(8, router_origin_off(a, strlen(a)));
    const char *b = "http://h";
    TEST_ASSERT_EQUAL_UINT(strlen(b), router_origin_off(b, strlen(b)));
}

/* ------------------------------ 라우팅 ------------------------------ */

void test_longest_prefix_wins(void) {
    add_route("/", 0);
    add_route("/api/", 1);
    add_route("/api/v2/", 2);
    TEST_ASSERT_EQUAL_INT(0, router_match(&c, "/index.html", 11)->up);
    TEST_ASSERT_EQUAL_INT(1, router_match(&c, "/api/users", 10)->up);
    TEST_ASSERT_EQUAL_INT(2, router_match(&c, "/api/v2/users", 13)->up);
}

void test_no_match_returns_null(void) {
    add_route("/api/", 1);
    TEST_ASSERT_NULL(router_match(&c, "/other", 6));
    TEST_ASSERT_NULL(router_match(&c, "/ap", 3));      /* 요청이 접두사보다 짧음 */
    TEST_ASSERT_NULL(router_match(&c, "/api", 4));     /* "/api/" 는 "/api" 와 일치하지 않음 */
}

void test_empty_path_matches_root(void) {
    add_route("/", 5);
    TEST_ASSERT_EQUAL_INT(5, router_match(&c, "", 0)->up);
}

void test_no_routes(void) {
    TEST_ASSERT_NULL(router_match(&c, "/x", 2));
}

/* --------------------------- Round-Robin --------------------------- */

void test_round_robin_cycles(void) {
    TEST_ASSERT_EQUAL_INT(0, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(2, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, upstream_pick(&u, 0, 0));
}

void test_down_server_skipped_until_expiry(void) {
    upstream_mark_down(&u, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(2, upstream_pick(&u, 0, 0));  /* 1 은 건너뜀 */
    for (int i = 0; i < 6; i++) TEST_ASSERT_NOT_EQUAL(1, upstream_pick(&u, 999, 0));
    int seen1 = 0;
    for (int i = 0; i < 6; i++) if (upstream_pick(&u, 1000, 0) == 1) seen1 = 1;  /* 만료 후 복귀 */
    TEST_ASSERT_TRUE(seen1);
}

void test_tried_servers_excluded(void) {
    TEST_ASSERT_EQUAL_INT(1, upstream_pick(&u, 0, 0x1));       /* 0 제외 */
    TEST_ASSERT_EQUAL_INT(-1, upstream_pick(&u, 0, 0x7));      /* 전부 시도함 */
}

void test_all_down_still_tried(void) {
    for (int i = 0; i < 3; i++) upstream_mark_down(&u, i, 1000000);
    int idx = upstream_pick(&u, 0, 0);
    TEST_ASSERT_TRUE(idx >= 0 && idx < 3);     /* 전부 장애여도 요청은 시도해 본다 */
}

void test_single_server(void) {
    u.nservers = 1;
    TEST_ASSERT_EQUAL_INT(0, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, upstream_pick(&u, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, upstream_pick(&u, 0, 0x1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_path_span_origin_form);
    RUN_TEST(test_path_span_absolute_form);
    RUN_TEST(test_path_span_invalid);
    RUN_TEST(test_origin_off);
    RUN_TEST(test_longest_prefix_wins);
    RUN_TEST(test_no_match_returns_null);
    RUN_TEST(test_empty_path_matches_root);
    RUN_TEST(test_no_routes);
    RUN_TEST(test_round_robin_cycles);
    RUN_TEST(test_down_server_skipped_until_expiry);
    RUN_TEST(test_tried_servers_excluded);
    RUN_TEST(test_all_down_still_tried);
    RUN_TEST(test_single_server);
    return UNITY_END();
}
