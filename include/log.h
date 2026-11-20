/*
 * log.h — stderr 로 한 줄씩 남기는 아주 작은 로거.
 * 각 줄 앞에 시각과 스레드 표시([main] 또는 [w3])를 붙여, 어떤 worker 가
 * 어떤 요청을 처리했는지 눈으로 따라갈 수 있게 한다.
 */
#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

void log_set_quiet(bool quiet);
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 요청별 access log. --quiet 이면 출력하지 않는다. */
void log_access(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* LOG_H */
