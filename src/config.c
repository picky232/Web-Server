#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct config g_cfg = {
    .bind_addr = "0.0.0.0",
    .port = 8080,
    .root = "./www",
    .threads = 4,
    .queue = 64,
    .proxy_prefix = "/api",
    .read_timeout_ms = 10000,
    .keepalive_ms = 5000,
    .proxy_timeout_ms = 5000,
    .max_body = 1048576,
    .max_requests = 100,
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -p, --port N              리스닝 포트 (0 = 빈 포트 자동)       [8080]\n"
            "      --bind ADDR           바인드할 IPv4 주소                  [0.0.0.0]\n"
            "  -r, --root DIR            정적 파일 폴더                      [./www]\n"
            "  -t, --threads N           작업 스레드 수 (1-256)              [4]\n"
            "      --queue N             연결 대기 큐 크기 (가득 차면 503)   [64]\n"
            "  -b, --backend HOST:PORT   프록시 백엔드 (반복하면 Round-Robin, 최대 4)\n"
            "      --proxy-prefix PATH   백엔드로 보낼 경로 접두사           [/api]\n"
            "      --read-timeout-ms N   요청 하나를 다 받는 제한 시간 → 408 [10000]\n"
            "      --keepalive-ms N      keep-alive 유휴 제한 시간           [5000]\n"
            "      --proxy-timeout-ms N  백엔드 연결·응답 대기 제한 → 504    [5000]\n"
            "      --max-body N          요청 본문 상한 → 413                [1048576]\n"
            "      --max-requests N      연결 하나당 최대 요청 수            [100]\n"
            "  -q, --quiet               요청별 로그 끄기\n",
            prog);
}

/* 문자열 → 정수. 범위를 벗어나거나 숫자가 아니면 -1 */
static int parse_num(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int parse_backend(const char *s)
{
    if (g_cfg.nbackends == MAX_BACKENDS)
        return -1;
    const char *colon = strrchr(s, ':');
    long port;
    if (!colon || colon == s || (size_t)(colon - s) >= sizeof g_cfg.backends[0].host ||
        parse_num(colon + 1, 1, 65535, &port) < 0)
        return -1;
    struct backend *b = &g_cfg.backends[g_cfg.nbackends++];
    snprintf(b->host, sizeof b->host, "%.*s", (int)(colon - s), s);
    snprintf(b->port, sizeof b->port, "%ld", port);
    snprintf(b->hostport, sizeof b->hostport, "%s:%ld", b->host, port);
    return 0;
}

int config_parse(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
            usage(argv[0]);
            return 1;
        }
        if (!strcmp(opt, "-q") || !strcmp(opt, "--quiet")) {
            g_cfg.quiet = 1;
            continue;
        }
        /* 나머지 옵션은 전부 값을 하나 받는다 */
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: 값이 필요합니다\n", opt);
            return -1;
        }
        const char *val = argv[++i];
        long n = 0;
        int bad = 0;
        if (!strcmp(opt, "-p") || !strcmp(opt, "--port")) {
            bad = parse_num(val, 0, 65535, &n);
            g_cfg.port = (int)n;
        } else if (!strcmp(opt, "--bind")) {
            g_cfg.bind_addr = val;
        } else if (!strcmp(opt, "-r") || !strcmp(opt, "--root")) {
            g_cfg.root = val;
        } else if (!strcmp(opt, "-t") || !strcmp(opt, "--threads")) {
            bad = parse_num(val, 1, 256, &n);
            g_cfg.threads = (int)n;
        } else if (!strcmp(opt, "--queue")) {
            bad = parse_num(val, 1, 65536, &n);
            g_cfg.queue = (int)n;
        } else if (!strcmp(opt, "-b") || !strcmp(opt, "--backend")) {
            bad = parse_backend(val);
        } else if (!strcmp(opt, "--proxy-prefix")) {
            bad = (val[0] != '/');
            g_cfg.proxy_prefix = val;
        } else if (!strcmp(opt, "--read-timeout-ms")) {
            bad = parse_num(val, 1, 3600000, &n);
            g_cfg.read_timeout_ms = (int)n;
        } else if (!strcmp(opt, "--keepalive-ms")) {
            bad = parse_num(val, 1, 3600000, &n);
            g_cfg.keepalive_ms = (int)n;
        } else if (!strcmp(opt, "--proxy-timeout-ms")) {
            bad = parse_num(val, 1, 3600000, &n);
            g_cfg.proxy_timeout_ms = (int)n;
        } else if (!strcmp(opt, "--max-body")) {
            bad = parse_num(val, 0, 1L << 30, &n);
            g_cfg.max_body = n;
        } else if (!strcmp(opt, "--max-requests")) {
            bad = parse_num(val, 1, 1000000, &n);
            g_cfg.max_requests = (int)n;
        } else {
            fprintf(stderr, "모르는 옵션: %s (--help 참고)\n", opt);
            return -1;
        }
        if (bad) {
            fprintf(stderr, "%s: 잘못된 값 '%s'\n", opt, val);
            return -1;
        }
    }
    return 0;
}
