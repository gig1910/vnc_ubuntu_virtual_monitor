#define _GNU_SOURCE

#include "shutdown_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_REGISTERED_FDS 32

static int signal_pipe[2] = {-1, -1};
static int supervisor_pipe[2] = {-1, -1};
static volatile sig_atomic_t requested = 0;
static volatile sig_atomic_t supervisor_stop = 0;
static pthread_t supervisor_thread;
static int supervisor_started = 0;
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int registered_fds[MAX_REGISTERED_FDS];
static size_t registered_fd_count = 0;

static void
signal_handler(int sig)
{
    if (requested) {
        /* Emergency escape hatch; _exit() is async-signal-safe. */
        _exit(128 + sig);
    }

    requested = 1;

    const uint8_t byte = 1;

    if (signal_pipe[1] >= 0) {
        ssize_t rc = write(signal_pipe[1], &byte, sizeof(byte));
        (void)rc;
    }

    if (supervisor_pipe[1] >= 0) {
        ssize_t rc = write(supervisor_pipe[1], &byte, sizeof(byte));
        (void)rc;
    }
}

static int
set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    flags = fcntl(fd, F_GETFD, 0);

    if (flags < 0)
        return -1;

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;

    return 0;
}

static void
shutdown_registered_fds(void)
{
    int snapshot[MAX_REGISTERED_FDS];
    size_t count = 0;

    pthread_mutex_lock(&fd_mutex);

    count = registered_fd_count;
    if (count > MAX_REGISTERED_FDS)
        count = MAX_REGISTERED_FDS;

    memcpy(snapshot, registered_fds, count * sizeof(snapshot[0]));

    pthread_mutex_unlock(&fd_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snapshot[i] >= 0)
            (void)shutdown(snapshot[i], SHUT_RDWR);
    }
}

static void *
shutdown_supervisor(void *arg)
{
    (void)arg;

    uint8_t buf[64];

    for (;;) {
        ssize_t n = read(supervisor_pipe[0], buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }

            break;
        }

        if (n == 0)
            break;

        if (requested)
            shutdown_registered_fds();

        if (supervisor_stop)
            break;
    }

    return NULL;
}

int
shutdown_signal_init(void)
{
    requested = 0;
    supervisor_stop = 0;
    registered_fd_count = 0;

    if (pipe(signal_pipe) < 0) {
        perror("shutdown signal pipe");
        return -1;
    }

    if (pipe(supervisor_pipe) < 0) {
        perror("shutdown supervisor pipe");
        shutdown_signal_cleanup();
        return -1;
    }

    if (
        set_nonblock_cloexec(signal_pipe[0]) < 0 ||
        set_nonblock_cloexec(signal_pipe[1]) < 0 ||
        set_nonblock_cloexec(supervisor_pipe[1]) < 0
    ) {
        perror("shutdown pipe flags");
        shutdown_signal_cleanup();
        return -1;
    }

    /* Supervisor read side deliberately stays blocking. */
    int flags = fcntl(supervisor_pipe[0], F_GETFD, 0);
    if (flags < 0 || fcntl(supervisor_pipe[0], F_SETFD, flags | FD_CLOEXEC) < 0) {
        perror("shutdown supervisor FD_CLOEXEC");
        shutdown_signal_cleanup();
        return -1;
    }

    if (
        pthread_create(
            &supervisor_thread,
            NULL,
            shutdown_supervisor,
            NULL
        ) != 0
    ) {
        perror("shutdown supervisor thread");
        shutdown_signal_cleanup();
        return -1;
    }

    supervisor_started = 1;

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (
        sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0
    ) {
        perror("sigaction");
        shutdown_signal_cleanup();
        return -1;
    }

    return 0;
}

void
shutdown_signal_cleanup(void)
{
    if (supervisor_started) {
        supervisor_stop = 1;

        if (supervisor_pipe[1] >= 0) {
            const uint8_t byte = 1;
            ssize_t rc = write(supervisor_pipe[1], &byte, sizeof(byte));
            (void)rc;
        }

        pthread_join(supervisor_thread, NULL);
        supervisor_started = 0;
    }

    if (signal_pipe[0] >= 0) {
        close(signal_pipe[0]);
        signal_pipe[0] = -1;
    }

    if (signal_pipe[1] >= 0) {
        close(signal_pipe[1]);
        signal_pipe[1] = -1;
    }

    if (supervisor_pipe[0] >= 0) {
        close(supervisor_pipe[0]);
        supervisor_pipe[0] = -1;
    }

    if (supervisor_pipe[1] >= 0) {
        close(supervisor_pipe[1]);
        supervisor_pipe[1] = -1;
    }

    pthread_mutex_lock(&fd_mutex);
    registered_fd_count = 0;
    pthread_mutex_unlock(&fd_mutex);
}

int
shutdown_signal_fd(void)
{
    return signal_pipe[0];
}

int
shutdown_signal_requested(void)
{
    return requested != 0;
}

void
shutdown_signal_drain(void)
{
    if (signal_pipe[0] < 0)
        return;

    uint8_t buf[64];

    for (;;) {
        ssize_t n = read(signal_pipe[0], buf, sizeof(buf));

        if (n > 0)
            continue;

        if (n < 0 && errno == EINTR)
            continue;

        break;
    }
}

int
shutdown_signal_register_fd(int fd)
{
    if (fd < 0)
        return -1;

    pthread_mutex_lock(&fd_mutex);

    for (size_t i = 0; i < registered_fd_count; i++) {
        if (registered_fds[i] == fd) {
            pthread_mutex_unlock(&fd_mutex);
            return 0;
        }
    }

    if (registered_fd_count >= MAX_REGISTERED_FDS) {
        pthread_mutex_unlock(&fd_mutex);
        errno = ENOSPC;
        return -1;
    }

    registered_fds[registered_fd_count++] = fd;
    int should_shutdown = requested != 0;

    pthread_mutex_unlock(&fd_mutex);

    if (should_shutdown)
        (void)shutdown(fd, SHUT_RDWR);

    return 0;
}

void
shutdown_signal_unregister_fd(int fd)
{
    if (fd < 0)
        return;

    pthread_mutex_lock(&fd_mutex);

    for (size_t i = 0; i < registered_fd_count; i++) {
        if (registered_fds[i] != fd)
            continue;

        registered_fds[i] = registered_fds[registered_fd_count - 1];
        registered_fd_count--;
        break;
    }

    pthread_mutex_unlock(&fd_mutex);
}
