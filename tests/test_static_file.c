/* macOS 는 _POSIX_C_SOURCE 를 정의하면 mkdtemp 선언을 숨긴다 (POSIX.1-2008 에는 포함). */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "unity.h"
#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 픽스처:
 *   <base>/outside/secret.txt         <- docroot 바깥의 비밀 파일
 *   <base>/root/                       <- docroot
 *     index.html, a.txt, "sp ace.txt", .secret
 *     sub/index.html, sub/page.html, emptydir/
 *     link_in  -> a.txt                (docroot 안쪽: 허용)
 *     link_out -> ../outside/secret.txt (바깥 파일 링크: 거부)
 *     dir_out  -> ../outside            (바깥 디렉터리 링크: 거부)
 */
static char g_base[PATH_MAX];
static char g_root[PATH_MAX];  /* realpath 로 정규화된 docroot */
static static_target t;

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fputs(content, f);
    fclose(f);
}

static void build_fixture(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_base, sizeof g_base, "%s/ws_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_base));

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/root", g_base);        mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/outside", g_base);     mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/root/sub", g_base);    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/root/emptydir", g_base); mkdir(p, 0755);

    snprintf(p, sizeof p, "%s/outside/secret.txt", g_base);  write_file(p, "TOP SECRET");
    snprintf(p, sizeof p, "%s/root/index.html", g_base);     write_file(p, "<h1>root</h1>");
    snprintf(p, sizeof p, "%s/root/a.txt", g_base);          write_file(p, "a");
    snprintf(p, sizeof p, "%s/root/sp ace.txt", g_base);     write_file(p, "s");
    snprintf(p, sizeof p, "%s/root/.secret", g_base);        write_file(p, "dot");
    snprintf(p, sizeof p, "%s/root/sub/index.html", g_base); write_file(p, "sub");
    snprintf(p, sizeof p, "%s/root/sub/page.html", g_base);  write_file(p, "page");

    snprintf(p, sizeof p, "%s/root/link_in", g_base);   TEST_ASSERT_EQUAL_INT(0, symlink("a.txt", p));
    snprintf(p, sizeof p, "%s/root/link_out", g_base);  TEST_ASSERT_EQUAL_INT(0, symlink("../outside/secret.txt", p));
    snprintf(p, sizeof p, "%s/root/dir_out", g_base);   TEST_ASSERT_EQUAL_INT(0, symlink("../outside", p));

    snprintf(p, sizeof p, "%s/root", g_base);
    TEST_ASSERT_NOT_NULL(realpath(p, g_root));
}

static void destroy_fixture(void) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_base);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed: %s\n", g_base);
}

void setUp(void)    {}
void tearDown(void) {}

static sr_result resolve(const char *uri) { return static_resolve(g_root, uri, strlen(uri), &t); }

static void assert_file_suffix(const char *suffix) {
    size_t fl = strlen(t.file), sl = strlen(suffix);
    TEST_ASSERT_TRUE_MESSAGE(fl >= sl && strcmp(t.file + fl - sl, suffix) == 0, t.file);
    TEST_ASSERT_EQUAL_INT(0, strncmp(t.file, g_root, strlen(g_root)));
}

/* ------------------------------ 정상 ------------------------------ */

void test_root_serves_index(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/"));
    assert_file_suffix("/index.html");
}

void test_plain_file(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/a.txt"));
    assert_file_suffix("/a.txt");
}

void test_query_and_fragment_ignored(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/a.txt?x=1&y=2"));
    assert_file_suffix("/a.txt");
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/a.txt#frag"));
    TEST_ASSERT_EQUAL_UINT(6, t.path_len);
}

void test_percent_encoded_space(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/sp%20ace.txt"));
    assert_file_suffix("/sp ace.txt");
}

void test_directory_with_slash_serves_index(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/sub/"));
    assert_file_suffix("/sub/index.html");
}

void test_directory_without_slash_redirects(void) {
    TEST_ASSERT_EQUAL_INT(SR_REDIRECT, resolve("/sub"));
    TEST_ASSERT_EQUAL_UINT(4, t.path_len);
    TEST_ASSERT_EQUAL_INT(SR_REDIRECT, resolve("/sub?x=1"));
    TEST_ASSERT_EQUAL_UINT(4, t.path_len);
}

void test_absolute_form_uri(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("http://example.com/a.txt"));
    assert_file_suffix("/a.txt");
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("http://example.com"));
    assert_file_suffix("/index.html");
}

void test_symlink_inside_docroot_allowed(void) {
    TEST_ASSERT_EQUAL_INT(SR_OK, resolve("/link_in"));
    assert_file_suffix("/a.txt");  /* 링크가 아니라 실제 파일로 해석된다 */
}

/* ----------------------------- 없음/금지 ---------------------------- */

void test_not_found(void) {
    TEST_ASSERT_EQUAL_INT(SR_NOT_FOUND, resolve("/missing.html"));
    TEST_ASSERT_EQUAL_INT(SR_NOT_FOUND, resolve("/a.txt/child"));  /* 파일 아래 경로 (ENOTDIR) */
}

void test_directory_without_index_is_forbidden(void) {
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/emptydir/"));  /* 디렉터리 목록은 노출하지 않는다 */
}

void test_dotfiles_forbidden(void) {
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/.secret"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/sub/.hidden"));
}

/* ------------------------- 경로 탈출(traversal) ------------------------- */

void test_dotdot_forbidden(void) {
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/../outside/secret.txt"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/sub/../a.txt"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/.."));
}

void test_encoded_dotdot_forbidden(void) {
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/%2e%2e/outside/secret.txt"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/%2E%2E%2Foutside/secret.txt"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/sub/%2e%2e/a.txt"));
}

void test_symlink_escape_forbidden(void) {
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/link_out"));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/dir_out/secret.txt"));
}

void test_sibling_prefix_directory_not_confused_with_docroot(void) {
    /* docroot ".../root" 와 접두사가 같은 ".../root_evil" 은 docroot 밖이어야 한다 */
    char evil[PATH_MAX], link[PATH_MAX];
    snprintf(evil, sizeof evil, "%s/root_evil", g_base);
    mkdir(evil, 0755);
    char f[PATH_MAX + 16];
    snprintf(f, sizeof f, "%s/x.txt", evil);
    write_file(f, "evil");
    snprintf(link, sizeof link, "%s/evil_link", g_root);
    TEST_ASSERT_EQUAL_INT(0, symlink("../root_evil", link));
    TEST_ASSERT_EQUAL_INT(SR_FORBIDDEN, resolve("/evil_link/x.txt"));
}

/* ------------------------------ 잘못된 입력 ------------------------------ */

void test_bad_percent_encoding(void) {
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("/%zz"));
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("/%2"));
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("/%"));
}

void test_encoded_nul_rejected(void) {
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("/a.txt%00.png"));
}

void test_not_origin_or_absolute_form(void) {
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("a.txt"));
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve("*"));
}

void test_overlong_path(void) {
    char uri[6000];
    uri[0] = '/';
    memset(uri + 1, 'a', sizeof uri - 2);
    uri[sizeof uri - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SR_BAD, resolve(uri));
}

int main(void) {
    build_fixture();
    UNITY_BEGIN();
    RUN_TEST(test_root_serves_index);
    RUN_TEST(test_plain_file);
    RUN_TEST(test_query_and_fragment_ignored);
    RUN_TEST(test_percent_encoded_space);
    RUN_TEST(test_directory_with_slash_serves_index);
    RUN_TEST(test_directory_without_slash_redirects);
    RUN_TEST(test_absolute_form_uri);
    RUN_TEST(test_symlink_inside_docroot_allowed);
    RUN_TEST(test_not_found);
    RUN_TEST(test_directory_without_index_is_forbidden);
    RUN_TEST(test_dotfiles_forbidden);
    RUN_TEST(test_dotdot_forbidden);
    RUN_TEST(test_encoded_dotdot_forbidden);
    RUN_TEST(test_symlink_escape_forbidden);
    RUN_TEST(test_sibling_prefix_directory_not_confused_with_docroot);
    RUN_TEST(test_bad_percent_encoding);
    RUN_TEST(test_encoded_nul_rejected);
    RUN_TEST(test_not_origin_or_absolute_form);
    RUN_TEST(test_overlong_path);
    int rc = UNITY_END();
    destroy_fixture();
    return rc;
}
