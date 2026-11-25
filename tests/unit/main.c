#include "test.h"

int g_checks, g_failed;

int main(void)
{
    test_str();
    test_http();
    test_body();
    test_uri();
    test_reply();
    test_spool();
    test_config();
    test_balance();
    test_cache();
    printf("단위 테스트: %d개 검사, %d개 실패\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
