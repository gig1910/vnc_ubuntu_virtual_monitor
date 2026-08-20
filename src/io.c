#include "io.h"
#include "shutdown_signal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int io_deadline_active = 0;
static _Thread_local struct timespec io_deadline;

int
io_deadline_set_ms(int timeout_ms)
{
    if (timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;

    io_deadline = now;
    io_deadline.tv_sec += timeout_ms / 1000;
    io_deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    if (io_deadline.tv_nsec >= 1000000000L) {
        io_deadline.tv_sec++;
        io_deadline.tv_nsec -= 1000000000L;
    }

    io_deadline_active = 1;
    return 0;
}

void
io_deadline_clear(void)
{
    io_deadline_active = 0;
    io_deadline.tv_sec = 0;
    io_deadline.tv_nsec = 0;
}

static int
io_deadline_remaining_ms(void)
{
    if (!io_deadline_active)
        return -1;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;

    time_t seconds = io_deadline.tv_sec - now.tv_sec;
    long nanoseconds = io_deadline.tv_nsec - now.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }

    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0))
        return 0;

    long long remaining_ms =
        (long long)seconds * 1000LL +
        ((long long)nanoseconds + 999999LL) / 1000000LL;

    if (remaining_ms < 1)
        remaining_ms = 1;
    if (remaining_ms > INT_MAX)
        remaining_ms = INT_MAX;

    return (int)remaining_ms;
}

static int
socket_wait_timeout_ms(int fd, short events)
{
    int option = events & POLLOUT ? SO_SNDTIMEO : SO_RCVTIMEO;
    struct timeval timeout = {0};
    socklen_t timeout_len = sizeof(timeout);

    if (getsockopt(fd,
                   SOL_SOCKET,
                   option,
                   &timeout,
                   &timeout_len) < 0) {
        /* Non-socket descriptors keep the historical infinite wait. */
        return -1;
    }

    if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
        return -1;

    long long timeout_ms =
        (long long)timeout.tv_sec * 1000LL +
        ((long long)timeout.tv_usec + 999LL) / 1000LL;

    if (timeout_ms < 1)
        timeout_ms = 1;
    if (timeout_ms > INT_MAX)
        timeout_ms = INT_MAX;

    return (int)timeout_ms;
}

static int
combined_wait_timeout_ms(int fd, short events)
{
    int socket_timeout = socket_wait_timeout_ms(fd, events);
    int deadline_timeout = io_deadline_remaining_ms();

    if (deadline_timeout == 0)
        return 0;

    if (socket_timeout < 0)
        return deadline_timeout;
    if (deadline_timeout < 0)
        return socket_timeout;

    return socket_timeout < deadline_timeout
        ? socket_timeout
        : deadline_timeout;
}

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

        /*
         * Normal long-lived protocol I/O waits indefinitely and is interrupted
         * by the process shutdown pipe. Bounded protocol phases can install a
         * thread-local monotonic deadline; socket SO_RCVTIMEO/SO_SNDTIMEO are
         * also honored when present. The earliest limit wins.
         */
        int timeout_ms = combined_wait_timeout_ms(fd, events);
        int rc = poll(fds, nfds, timeout_ms);

        if (rc < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (rc == 0) {
            errno = ETIMEDOUT;
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
