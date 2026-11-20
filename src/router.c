#include "router.h"
#include "config.h"

#include <pthread.h>
#include <string.h>

int router_is_proxy(const char *path)
{
    const char *prefix = g_cfg.proxy_prefix;
    size_t n = strlen(prefix);
    while (n > 1 && prefix[n - 1] == '/')
        n--; /* "--proxy-prefix /api/" 라고 줘도 "/api"로 본다 */
    return !strncmp(path, prefix, n) && (path[n] == '\0' || path[n] == '/');
}

int router_next_backend(void)
{
    /* 여러 작업 스레드가 동시에 올리는 공유 카운터라 mutex로 감싼다.
       안 감싸면 두 스레드가 같은 값을 읽어 같은 백엔드로 몰릴 수 있다 */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned next;
    if (g_cfg.nbackends == 0)
        return -1;
    pthread_mutex_lock(&lock);
    int i = (int)(next++ % (unsigned)g_cfg.nbackends);
    pthread_mutex_unlock(&lock);
    return i;
}
