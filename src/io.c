#include "io.h"
#include "shutdown_signal.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

static int
wait_fd_or_shutdown(
    int fd,
    short events)
{
    for (;;) {
        if (shutdown_signal_requested()) {
            errno = ECANCELED;
            return -1;
        }

        struct pollfd fds[2];
        nfds_t nfds = 1;

        fds[0].fd = fd;
        fds[0].events = events;
        fds[0].revents = 0;

        int stop_fd = shutdown_signal_fd();

        if (stop_fd >= 0) {
            fds[1].fd = stop_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        int rc = poll(fds, nfds, -1);

        if (rc < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (
            nfds == 2 &&
            (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))
        ) {
            errno = ECANCELED;
            return -1;
        }

        if (
            fds[0].revents &
            (events | POLLERR | POLLHUP | POLLNVAL)
        )
            return 0;
    }
}

int
io_read_exact_status(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len > 0) {
        if (wait_fd_or_shutdown(fd, POLLIN) < 0)
            return -1;

        ssize_t n = read(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return 0;

        p += n;
        len -= (size_t)n;
    }

    return 1;
}

int
io_read_exact(int fd, void *buf, size_t len)
{
    return
        io_read_exact_status(fd, buf, len) == 1
            ? 0
            : -1;
}

int
io_write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len > 0) {
        if (wait_fd_or_shutdown(fd, POLLOUT) < 0)
            return -1;

        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

uint16_t
io_get_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

uint32_t
io_get_u32_be(const uint8_t *p)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        (uint32_t)p[3];
}

void
io_put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void
io_put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
