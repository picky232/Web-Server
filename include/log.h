#ifndef LOG_H
#define LOG_H

struct conn;
struct loop;

void log_access(struct conn *c); /* 요청 하나가 끝났다: 통계와 접근 로그 */
void log_flush(struct loop *l);  /* 루프 바퀴 끝에 모아서 쓴다 */

#endif
