#ifndef LOG_H
#define LOG_H

struct conn;
struct loop;

void log_access(struct conn *c); /* 요청 하나가 끝났다: 통계와 접근 로그 */
void log_flush(struct loop *l);  /* 루프 바퀴 끝에 모아서 쓴다 */

/* 운영자에게 알리는 한 줄(표준 오류). 받는 쪽이 막혀 있으면 기다리지 않고 버린다 */
void log_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
