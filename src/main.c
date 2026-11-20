/*
 * main.c — 프로그램 진입점: 설정을 읽고 server_run() 을 호출한다.
 */
#include <stdio.h>

#include "config.h"
#include "server.h"

int main(int argc, char **argv)
{
    server_config_t cfg;
    config_set_defaults(&cfg);

    char err[256] = {0};
    switch (config_parse_args(&cfg, argc, argv, err, sizeof err)) {
    case CONFIG_HELP:
        config_print_usage(stdout, argv[0]);
        return 0;
    case CONFIG_ERROR:
        fprintf(stderr, "error: %s\n\n", err);
        config_print_usage(stderr, argv[0]);
        return 2;
    case CONFIG_OK:
        break;
    }
    return server_run(&cfg);
}
