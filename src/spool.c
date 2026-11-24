#include "spool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void spool_init(struct spool *s, atomic_llong *disk)
{
    memset(s, 0, sizeof *s);
    buf_init(&s->mem, SPOOL_MEM);
    s->fd = -1;
    s->disk = disk;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int open_file(struct spool *s)
{
    char name[] = "/tmp/webserver-XXXXXX";
    s->fd = mkostemp(name, O_CLOEXEC);
    if (s->fd < 0)
        return -1;
    unlink(name); /* 이름을 바로 지운다. 서버가 죽어도 fd가 닫히면 커널이 디스크를 돌려받는다 */
    return write_all(s->fd, buf_head(&s->mem), buf_len(&s->mem));
}

int spool_append(struct spool *s, const void *p, size_t n, long long disk_max)
{
    if (s->fd < 0 && buf_append(&s->mem, p, n)) {
        s->size += (long long)n;
        return 0;
    }
    long long need = (long long)n + (s->fd < 0 ? s->size : 0);
    if (atomic_fetch_add(s->disk, need) + need > disk_max) {
        atomic_fetch_sub(s->disk, need);
        return 503;
    }
    s->disk_used += need;
    if ((s->fd < 0 && open_file(s) < 0) || write_all(s->fd, p, n) < 0)
        return 500;
    buf_free(&s->mem);
    s->size += (long long)n;
    return 0;
}

ssize_t spool_read(const struct spool *s, long long off, void *dst, size_t n)
{
    if (s->fd >= 0)
        return pread(s->fd, dst, n, (off_t)off);
    size_t have = buf_len(&s->mem) - (size_t)off;
    if (n > have)
        n = have;
    memcpy(dst, buf_head(&s->mem) + off, n);
    return (ssize_t)n;
}

int spool_to_file(struct spool *s)
{
    if (s->fd < 0 && open_file(s) < 0)
        return -1;
    return lseek(s->fd, 0, SEEK_SET) < 0 ? -1 : s->fd;
}

void spool_free(struct spool *s)
{
    buf_free(&s->mem);
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    if (s->disk && s->disk_used)
        atomic_fetch_sub(s->disk, s->disk_used);
    s->disk_used = 0;
}
