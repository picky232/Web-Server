#ifndef SPOOL_H
#define SPOOL_H

#include "buf.h"

#include <stdatomic.h>
#include <sys/types.h>

#define SPOOL_MEM 65536

/* 요청 본문을 다 받을 때까지 모아 두는 곳. 64KB까지는 메모리, 넘으면 이름 없는 임시 파일.
   다 받은 뒤에 백엔드를 고르므로 느린 업로드가 백엔드를 붙잡지 않는다 */
struct spool {
    struct buf mem;
    int fd; /* -1 = 아직 메모리 */
    long long size;
    atomic_llong *disk; /* 서버 전체의 임시 파일 바이트 */
    long long disk_used;
};

void spool_init(struct spool *s, atomic_llong *disk);
int spool_append(struct spool *s, const void *p, size_t n, long long disk_max); /* 0, 500, 503 */
ssize_t spool_read(const struct spool *s, long long off, void *dst, size_t n);
int spool_to_file(struct spool *s); /* 파일로 바꿔 처음 위치로 되감은 fd. 실패 -1 (CGI 표준 입력용) */
void spool_free(struct spool *s);

#endif
