#include "unity.h"
#include "mime.h"

void setUp(void) {}
void tearDown(void) {}

void test_common_types(void) {
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", mime_for_path("/index.html"));
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", mime_for_path("/a.htm"));
    TEST_ASSERT_EQUAL_STRING("text/css; charset=utf-8", mime_for_path("/s.css"));
    TEST_ASSERT_EQUAL_STRING("text/javascript; charset=utf-8", mime_for_path("/a.js"));
    TEST_ASSERT_EQUAL_STRING("application/json; charset=utf-8", mime_for_path("/d.json"));
    TEST_ASSERT_EQUAL_STRING("image/png", mime_for_path("/x.png"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", mime_for_path("/x.jpg"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", mime_for_path("/x.jpeg"));
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", mime_for_path("/x.svg"));
    TEST_ASSERT_EQUAL_STRING("application/wasm", mime_for_path("/m.wasm"));
}

void test_case_insensitive(void) {
    TEST_ASSERT_EQUAL_STRING("image/png", mime_for_path("/X.PNG"));
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", mime_for_path("/A.HtMl"));
}

void test_unknown_and_missing_extension(void) {
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/file.unknownext"));
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/README"));
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/a.tar.gz"));
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/trailingdot."));
}

void test_dot_in_directory_is_not_extension(void) {
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/dir.html/file"));
}

void test_dotfile_has_no_extension(void) {
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime_for_path("/.html"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_common_types);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_unknown_and_missing_extension);
    RUN_TEST(test_dot_in_directory_is_not_extension);
    RUN_TEST(test_dotfile_has_no_extension);
    return UNITY_END();
}
