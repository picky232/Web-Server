/*
 * config.c — 명령행 인자를 server_config_t 로 변환한다.
 *
 * getopt_long 대신 직접 argv 를 순회한다. 전역 상태(optind)가 없어서
 * 단위 테스트에서 여러 번 호출해도 안전하고, 동작을 그대로 읽을 수 있다.
 */
#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void config_set_defaults(server_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->bind_addr, sizeof cfg->bind_addr, "%s", "0.0.0.0");
    cfg->port = 8080;
    snprintf(cfg->doc_root, sizeof cfg->doc_root, "%s", "./www");
    cfg->threads = 4;
    cfg->queue_capacity = 64;
    snprintf(cfg->proxy_prefix, sizeof cfg->proxy_prefix, "%s", "/api");
    cfg->backend_count = 0;
    cfg->read_timeout_ms = 10000;
    cfg->keepalive_timeout_ms = 5000;
    cfg->proxy_timeout_ms = 5000;
    cfg->max_body_bytes = 1024ULL * 1024ULL; /* 1 MiB */
    cfg->max_requests_per_conn = 100;
    cfg->quiet = false;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* 10진 정수 문자열을 [min, max] 범위로 엄격하게 파싱한다 ("12abc", "" 거부). */
static bool parse_long_range(const char *s, long min, long max, long *out)
{
    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max)
        return false;
    *out = v;
    return true;
}

/* "host:port" → backend_spec_t. host 부분은 DNS 해석 전 문자열 그대로 둔다. */
static bool parse_backend(const char *s, backend_spec_t *b)
{
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon == s)
        return false;
    size_t host_len = (size_t)(colon - s);
    if (host_len >= sizeof b->host)
        return false;
    long port;
    if (!parse_long_range(colon + 1, 1, 65535, &port))
        return false;
    memcpy(b->host, s, host_len);
    b->host[host_len] = '\0';
    b->port = (int)port;
    return true;
}

static bool is_opt(const char *arg, const char *short_name, const char *long_name)
{
    return (short_name != NULL && strcmp(arg, short_name) == 0) ||
           (long_name != NULL && strcmp(arg, long_name) == 0);
}

config_result_t config_parse_args(server_config_t *cfg, int argc, char **argv,
                                  char *err, size_t errlen)
{
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];

        /* 값이 없는 옵션 */
        if (is_opt(opt, "-h", "--help"))
            return CONFIG_HELP;
        if (is_opt(opt, "-q", "--quiet")) {
            cfg->quiet = true;
            continue;
        }

        /* 나머지는 모두 "--옵션 값" 형태 */
        static const char *const with_value[] = {
            "-p", "--port", "-r", "--root", "-t", "--threads", "--queue",
            "--bind", "--proxy-prefix", "-b", "--backend", "--read-timeout-ms",
            "--keepalive-ms", "--proxy-timeout-ms", "--max-body",
            "--max-requests", NULL};
        bool known = false;
        for (int k = 0; with_value[k] != NULL; k++)
            if (strcmp(opt, with_value[k]) == 0)
                known = true;
        if (!known) {
            set_err(err, errlen, "unknown option '%s'", opt);
            return CONFIG_ERROR;
        }
        if (i + 1 >= argc) {
            set_err(err, errlen, "option '%s' requires a value", opt);
            return CONFIG_ERROR;
        }
        const char *val = argv[++i];
        long n = 0;

        if (is_opt(opt, "-p", "--port")) {
            if (!parse_long_range(val, 0, 65535, &n)) {
                set_err(err, errlen, "invalid port '%s' (0-65535)", val);
                return CONFIG_ERROR;
            }
            cfg->port = (int)n;
        } else if (is_opt(opt, "-r", "--root")) {
            if (strlen(val) >= sizeof cfg->doc_root) {
                set_err(err, errlen, "document root path too long");
                return CONFIG_ERROR;
            }
            snprintf(cfg->doc_root, sizeof cfg->doc_root, "%s", val);
        } else if (is_opt(opt, "-t", "--threads")) {
            if (!parse_long_range(val, 1, 256, &n)) {
                set_err(err, errlen, "invalid thread count '%s' (1-256)", val);
                return CONFIG_ERROR;
            }
            cfg->threads = (int)n;
        } else if (is_opt(opt, NULL, "--queue")) {
            if (!parse_long_range(val, 1, 65536, &n)) {
                set_err(err, errlen, "invalid queue capacity '%s' (1-65536)", val);
                return CONFIG_ERROR;
            }
            cfg->queue_capacity = (int)n;
        } else if (is_opt(opt, NULL, "--bind")) {
            if (strlen(val) >= sizeof cfg->bind_addr) {
                set_err(err, errlen, "bind address too long");
                return CONFIG_ERROR;
            }
            snprintf(cfg->bind_addr, sizeof cfg->bind_addr, "%s", val);
        } else if (is_opt(opt, NULL, "--proxy-prefix")) {
            size_t len = strlen(val);
            if (len < 2 || len >= sizeof cfg->proxy_prefix || val[0] != '/') {
                set_err(err, errlen, "proxy prefix must look like '/api'");
                return CONFIG_ERROR;
            }
            snprintf(cfg->proxy_prefix, sizeof cfg->proxy_prefix, "%s", val);
            /* "/api/" 처럼 끝 '/' 는 제거해서 "/api" 로 통일 */
            while (len > 1 && cfg->proxy_prefix[len - 1] == '/')
                cfg->proxy_prefix[--len] = '\0';
            if (len < 2) {
                set_err(err, errlen, "proxy prefix must not be '/'");
                return CONFIG_ERROR;
            }
        } else if (is_opt(opt, "-b", "--backend")) {
            if (cfg->backend_count >= CONFIG_MAX_BACKENDS) {
                set_err(err, errlen, "at most %d backends are supported",
                        CONFIG_MAX_BACKENDS);
                return CONFIG_ERROR;
            }
            if (!parse_backend(val, &cfg->backends[cfg->backend_count])) {
                set_err(err, errlen, "invalid backend '%s' (expected host:port)", val);
                return CONFIG_ERROR;
            }
            cfg->backend_count++;
        } else if (is_opt(opt, NULL, "--read-timeout-ms")) {
            if (!parse_long_range(val, 100, 600000, &n)) {
                set_err(err, errlen, "invalid read timeout '%s' (100-600000 ms)", val);
                return CONFIG_ERROR;
            }
            cfg->read_timeout_ms = (int)n;
        } else if (is_opt(opt, NULL, "--keepalive-ms")) {
            if (!parse_long_range(val, 100, 600000, &n)) {
                set_err(err, errlen, "invalid keep-alive timeout '%s' (100-600000 ms)", val);
                return CONFIG_ERROR;
            }
            cfg->keepalive_timeout_ms = (int)n;
        } else if (is_opt(opt, NULL, "--proxy-timeout-ms")) {
            if (!parse_long_range(val, 100, 600000, &n)) {
                set_err(err, errlen, "invalid proxy timeout '%s' (100-600000 ms)", val);
                return CONFIG_ERROR;
            }
            cfg->proxy_timeout_ms = (int)n;
        } else if (is_opt(opt, NULL, "--max-body")) {
            if (!parse_long_range(val, 0, 1024L * 1024L * 1024L, &n)) {
                set_err(err, errlen, "invalid max body '%s' (0-1073741824 bytes)", val);
                return CONFIG_ERROR;
            }
            cfg->max_body_bytes = (unsigned long long)n;
        } else if (is_opt(opt, NULL, "--max-requests")) {
            if (!parse_long_range(val, 1, 100000, &n)) {
                set_err(err, errlen, "invalid max requests '%s' (1-100000)", val);
                return CONFIG_ERROR;
            }
            cfg->max_requests_per_conn = (int)n;
        }
    }
    return CONFIG_OK;
}

void config_print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "\n"
            "A small HTTP/1.1 server: static files + reverse proxy + thread pool.\n"
            "\n"
            "Options:\n"
            "  -p, --port N             listening port (default 8080, 0 = pick a free port)\n"
            "      --bind ADDR          IPv4 address to bind (default 0.0.0.0)\n"
            "  -r, --root DIR           document root for static files (default ./www)\n"
            "  -t, --threads N          worker threads (default 4)\n"
            "      --queue N            pending-connection queue capacity (default 64)\n"
            "  -b, --backend HOST:PORT  reverse-proxy backend; repeat for round-robin (max %d)\n"
            "      --proxy-prefix PATH  path prefix routed to the backend (default /api)\n"
            "      --read-timeout-ms N  time allowed to receive one request (default 10000)\n"
            "      --keepalive-ms N     idle time before closing a keep-alive connection (default 5000)\n"
            "      --proxy-timeout-ms N backend connect/response timeout (default 5000)\n"
            "      --max-body N         max request body bytes (default 1048576)\n"
            "      --max-requests N     max requests per connection (default 100)\n"
            "  -q, --quiet              disable per-request access log\n"
            "  -h, --help               show this help\n"
            "\n"
            "Example:\n"
            "  %s --port 8080 --root ./www --threads 4 --backend 127.0.0.1:9000\n",
            prog, CONFIG_MAX_BACKENDS, prog);
}
