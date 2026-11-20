#ifndef THREADPOOL_H
#define THREADPOOL_H

/* 작업 스레드 n개 + 크기 qsize의 연결 큐. 큐에 들어온 연결은 handler(fd, ip)가 처리한다 */
int pool_start(int nthreads, int qsize, void (*handler)(int fd, const char *ip));

/* 메인 스레드가 accept한 연결을 큐에 넣는다. 0 = 넣음, -1 = 큐가 가득 참 */
int pool_submit(int fd, const char *ip);

/* 작업 스레드를 기다리는 연결 수. keep-alive로 놀고 있는 스레드가 자리를 비켜 줄지 판단할 때 쓴다 */
int pool_waiting(void);

/* 새 연결을 그만 꺼내고, 처리 중인 연결이 끝날 때까지 기다린 뒤 큐에 남은 연결을 닫는다 */
void pool_stop(void);

#endif
