#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

extern int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

/* 실패했을 때 어떤 입력이었는지 같이 보여 준다 */
#define CHECK_MSG(cond, msg)                                               \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            printf("FAIL %s:%d  %s  [%s]\n", __FILE__, __LINE__, #cond, msg); \
        }                                                                  \
    } while (0)

void test_str(void);
void test_http(void);
void test_body(void);
void test_uri(void);
void test_reply(void);
void test_spool(void);
void test_config(void);
void test_balance(void);
void test_cgi(void);
void test_cache(void);

#endif
