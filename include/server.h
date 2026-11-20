/*
 * server.h — 리스닝 소켓, accept 루프, 연결 처리(worker), graceful shutdown.
 */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* 서버를 실행한다. SIGINT/SIGTERM 을 받으면 정리 후 0 을 반환.
 * 시작 단계에서 실패하면 1 을 반환. */
int server_run(const server_config_t *cfg);

#endif /* SERVER_H */
