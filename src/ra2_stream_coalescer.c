#include "ra2_stream_coalescer.h"

#include "benchmark.h"
#include "shutdown_signal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int
wait_for_backend(
    int backend_fd,
    int timeout_us)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(backend_fd, &readfds);

    int stop_fd =
        shutdown_signal_fd();

    if (stop_fd >= 0)
        FD_SET(stop_fd, &readfds);

    int maxfd =
        backend_fd > stop_fd
            ? backend_fd
            : stop_fd;

    struct timeval tv = {
        .tv_sec = timeout_us / 1000000,
        .tv_usec = timeout_us % 1000000
    };

    int rc =
        select(
            maxfd + 1,
            &readfds,
            NULL,
            NULL,
            &tv
        );

    if (rc < 0) {
        if (errno == EINTR)
            return
                shutdown_signal_requested()
                    ? 2
                    : 0;

        return -1;
    }

    if (
        stop_fd >= 0 &&
        FD_ISSET(stop_fd, &readfds)
    )
        return 2;

    return
        FD_ISSET(backend_fd, &readfds)
            ? 1
            : 0;
}

static ssize_t
backend_read(
    int fd,
    uint8_t *buf,
    size_t cap,
    int nonblocking,
    PipelineStats *stats)
{
    int flags =
        nonblocking
            ? MSG_DONTWAIT
            : 0;

    ssize_t n =
        recv(
            fd,
            buf,
            cap,
            flags
        );

    if (n > 0) {
        benchmark_record_backend_read(
            (size_t)n
        );

        pipeline_stats_backend_read(
            stats,
            (size_t)n
        );
    }

    return n;
}

Ra2CoalesceResult
ra2_stream_coalesce_and_send(
    int backend_fd,
    int external_fd,
    Ra2Direction *server_to_client,
    const RuntimeConfig *cfg,
    PipelineStats *stats,
    unsigned long long *records_sent,
    unsigned long long *bytes_sent)
{
    if (
        !server_to_client ||
        !cfg ||
        cfg->ra2_stream_record_max < 1
    )
        return RA2_COALESCE_ERROR;

    size_t cap =
        (size_t)cfg->ra2_stream_record_max;

    uint8_t *buffer =
        malloc(cap);

    if (!buffer)
        return RA2_COALESCE_ERROR;

    size_t used = 0;

    ssize_t n =
        backend_read(
            backend_fd,
            buffer,
            cap,
            0,
            stats
        );

    if (n == 0) {
        free(buffer);
        return RA2_COALESCE_BACKEND_EOF;
    }

    if (n < 0) {
        if (
            errno == EINTR &&
            shutdown_signal_requested()
        ) {
            free(buffer);
            return RA2_COALESCE_SHUTDOWN;
        }

        perror("read from LibVNCServer backend");
        free(buffer);
        return RA2_COALESCE_ERROR;
    }

    used = (size_t)n;

    if (
        cfg->ra2_coalesce &&
        used < cap
    ) {
        /*
         * First drain everything already queued. This costs no deliberate
         * latency and collapses the common "many tiny loopback writes" case.
         */
        for (;;) {
            n =
                backend_read(
                    backend_fd,
                    buffer + used,
                    cap - used,
                    1,
                    stats
                );

            if (n > 0) {
                used += (size_t)n;

                if (used == cap)
                    break;

                continue;
            }

            if (n == 0) {
                /*
                 * Backend closed after producing a final partial burst.
                 * Send what we have; EOF will be observed on the next call.
                 */
                break;
            }

            if (
                errno == EAGAIN ||
                errno == EWOULDBLOCK
            )
                break;

            if (errno == EINTR)
                continue;

            perror("coalescer recv(MSG_DONTWAIT)");
            free(buffer);
            return RA2_COALESCE_ERROR;
        }

        /*
         * Optional tiny aggregation window. Once any additional data arrives,
         * drain it immediately again. The total wait is bounded by the
         * configured deadline, not repeated for every tiny backend write.
         */
        if (
            used < cap &&
            cfg->ra2_coalesce_us > 0
        ) {
            double deadline =
                benchmark_now() +
                (double)cfg->ra2_coalesce_us /
                1000000.0;

            while (used < cap) {
                double remain =
                    deadline -
                    benchmark_now();

                if (remain <= 0.0)
                    break;

                int remain_us =
                    (int)(remain * 1000000.0);

                if (remain_us < 1)
                    remain_us = 1;

                int ready =
                    wait_for_backend(
                        backend_fd,
                        remain_us
                    );

                if (ready == 2) {
                    free(buffer);
                    return RA2_COALESCE_SHUTDOWN;
                }

                if (ready < 0) {
                    perror("coalescer select");
                    free(buffer);
                    return RA2_COALESCE_ERROR;
                }

                if (ready == 0)
                    break;

                for (;;) {
                    n =
                        backend_read(
                            backend_fd,
                            buffer + used,
                            cap - used,
                            1,
                            stats
                        );

                    if (n > 0) {
                        used += (size_t)n;

                        if (used == cap)
                            break;

                        continue;
                    }

                    if (n == 0)
                        break;

                    if (
                        errno == EAGAIN ||
                        errno == EWOULDBLOCK
                    )
                        break;

                    if (errno == EINTR)
                        continue;

                    perror("coalescer burst drain");
                    free(buffer);
                    return RA2_COALESCE_ERROR;
                }

                if (used == cap)
                    break;
            }
        }
    }

    double send_started =
        benchmark_now();

    int send_rc =
        ra2_send_record(
            external_fd,
            server_to_client,
            buffer,
            used
        );

    double send_ms =
        (
            benchmark_now() -
            send_started
        ) * 1000.0;

    if (send_rc < 0) {
        free(buffer);
        return RA2_COALESCE_ERROR;
    }

    benchmark_record_ra2_out(
        used,
        send_ms
    );

    pipeline_stats_ra2_send(
        stats,
        used
    );

    if (records_sent)
        (*records_sent)++;

    if (bytes_sent)
        *bytes_sent +=
            (unsigned long long)used;

    free(buffer);
    return RA2_COALESCE_OK;
}
