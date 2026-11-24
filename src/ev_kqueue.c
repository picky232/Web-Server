/* macOS: kqueue. 읽기와 쓰기를 따로 등록한다 */
#include "ev.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/event.h>
#include <sys/time.h>

#define EV_MAX 256

int ev_open(void)
{
    int fd = kqueue();
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static int change(int evfd, struct io *io, short filter, bool on)
{
    struct kevent k;
    EV_SET(&k, io->fd, filter, on ? EV_ADD : EV_DELETE, 0, 0, io);
    int r = kevent(evfd, &k, 1, NULL, 0, NULL);
    return r < 0 && !(errno == ENOENT && !on) ? -1 : 0;
}

int ev_set(int evfd, struct io *io, unsigned want)
{
    unsigned diff = io->want ^ want;
    if ((diff & IO_R) && change(evfd, io, EVFILT_READ, want & IO_R) < 0)
        return -1;
    if ((diff & IO_W) && change(evfd, io, EVFILT_WRITE, want & IO_W) < 0)
        return -1;
    io->want = want;
    return 0;
}

int ev_wait(int evfd, struct ev_event *out, int max, int timeout_ms)
{
    struct kevent ev[EV_MAX];
    struct timespec ts = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int n = kevent(evfd, NULL, 0, ev, max < EV_MAX ? max : EV_MAX, timeout_ms < 0 ? NULL : &ts);
    for (int i = 0; i < n; i++)
        out[i] = (struct ev_event){ev[i].udata, ev[i].filter == EVFILT_READ ? IO_R : IO_W};
    return n;
}
