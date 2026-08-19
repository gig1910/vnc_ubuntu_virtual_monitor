#define _GNU_SOURCE

#include "pipeline_stats.h"

#include <errno.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>

double
pipeline_stats_now(void)
{
    struct timespec ts;
    clock_gettime(
        CLOCK_MONOTONIC,
        &ts
    );

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec /
        1000000000.0;
}

static void
reset_interval_locked(
    PipelineStats *stats,
    double now)
{
    stats->interval_started_at = now;

    stats->capture_frames = 0;
    stats->capture_bytes = 0;

    stats->publish_checks = 0;
    stats->publish_frames = 0;
    stats->source_frames_skipped = 0;

    stats->diff_rectangles = 0;
    stats->diff_pixels = 0;
    stats->diff_ms_sum = 0.0;
    stats->diff_ms_max = 0.0;

    stats->rfb_process_ms_sum = 0.0;
    stats->rfb_process_ms_max = 0.0;
    stats->rfb_process_calls = 0;

    stats->backend_reads = 0;
    stats->backend_bytes = 0;

    stats->ra2_records = 0;
    stats->ra2_payload_bytes = 0;

    stats->external_outq_max =
        stats->external_outq_current;

    stats->backend_inq_max =
        stats->backend_inq_current;

    stats->tcp_unacked_max =
        stats->tcp_unacked_current;

    stats->tcp_rtt_us_max =
        stats->tcp_rtt_us_current;
}

int
pipeline_stats_init(
    PipelineStats *stats,
    const RuntimeConfig *cfg)
{
    if (!stats || !cfg)
        return -1;

    memset(stats, 0, sizeof(*stats));

    if (
        pthread_mutex_init(
            &stats->mutex,
            NULL
        ) != 0
    )
        return -1;

    stats->cfg = cfg;
    stats->started_at =
        pipeline_stats_now();

    stats->interval_started_at =
        stats->started_at;

    stats->initialized = 1;
    return 0;
}

void
pipeline_stats_destroy(
    PipelineStats *stats)
{
    if (!stats || !stats->initialized)
        return;

    pthread_mutex_destroy(
        &stats->mutex
    );

    memset(
        stats,
        0,
        sizeof(*stats)
    );
}

void
pipeline_stats_capture(
    PipelineStats *stats,
    size_t bytes)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    pthread_mutex_lock(&stats->mutex);

    stats->capture_frames++;
    stats->capture_bytes += bytes;

    pthread_mutex_unlock(&stats->mutex);
}

void
pipeline_stats_publish(
    PipelineStats *stats,
    uint64_t source_sequence_delta,
    int rect_count,
    uint64_t changed_pixels,
    double diff_ms,
    double rfb_process_ms)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    pthread_mutex_lock(&stats->mutex);

    stats->publish_checks++;
    stats->rfb_process_calls++;
    stats->rfb_process_ms_sum +=
        rfb_process_ms;

    if (
        rfb_process_ms >
        stats->rfb_process_ms_max
    )
        stats->rfb_process_ms_max =
            rfb_process_ms;

    if (source_sequence_delta > 0) {
        stats->publish_frames++;

        if (source_sequence_delta > 1) {
            stats->source_frames_skipped +=
                source_sequence_delta - 1;
        }
    }

    if (rect_count > 0) {
        stats->diff_rectangles +=
            (uint64_t)rect_count;

        stats->diff_pixels +=
            changed_pixels;
    }

    stats->diff_ms_sum += diff_ms;

    if (diff_ms > stats->diff_ms_max)
        stats->diff_ms_max = diff_ms;

    pthread_mutex_unlock(&stats->mutex);
}

void
pipeline_stats_backend_read(
    PipelineStats *stats,
    size_t bytes)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    pthread_mutex_lock(&stats->mutex);

    stats->backend_reads++;
    stats->backend_bytes += bytes;

    pthread_mutex_unlock(&stats->mutex);
}

void
pipeline_stats_ra2_send(
    PipelineStats *stats,
    size_t payload_bytes)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    pthread_mutex_lock(&stats->mutex);

    stats->ra2_records++;
    stats->ra2_payload_bytes +=
        payload_bytes;

    pthread_mutex_unlock(&stats->mutex);
}

void
pipeline_stats_transport_queues(
    PipelineStats *stats,
    int external_fd,
    int backend_fd)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    int outq = 0;
    int inq = 0;

    if (external_fd >= 0) {
        if (
            ioctl(
                external_fd,
                SIOCOUTQ,
                &outq
            ) < 0
        )
            outq = -1;
    }
    else {
        outq = -1;
    }

    if (backend_fd >= 0) {
        if (
            ioctl(
                backend_fd,
                FIONREAD,
                &inq
            ) < 0
        )
            inq = -1;
    }
    else {
        inq = -1;
    }

    uint32_t unacked = 0;
    uint32_t rtt_us = 0;

    if (external_fd >= 0) {
        struct tcp_info info;
        memset(
            &info,
            0,
            sizeof(info)
        );

        socklen_t info_len =
            sizeof(info);

        if (
            getsockopt(
                external_fd,
                IPPROTO_TCP,
                TCP_INFO,
                &info,
                &info_len
            ) == 0
        ) {
            unacked =
                info.tcpi_unacked;

            rtt_us =
                info.tcpi_rtt;
        }
    }

    pthread_mutex_lock(&stats->mutex);

    stats->external_outq_current =
        outq;

    if (
        outq >
        stats->external_outq_max
    )
        stats->external_outq_max =
            outq;

    stats->backend_inq_current =
        inq;

    if (
        inq >
        stats->backend_inq_max
    )
        stats->backend_inq_max =
            inq;

    stats->tcp_unacked_current =
        unacked;

    if (
        unacked >
        stats->tcp_unacked_max
    )
        stats->tcp_unacked_max =
            unacked;

    stats->tcp_rtt_us_current =
        rtt_us;

    if (
        rtt_us >
        stats->tcp_rtt_us_max
    )
        stats->tcp_rtt_us_max =
            rtt_us;

    pthread_mutex_unlock(&stats->mutex);
}

void
pipeline_stats_trace_publish(
    PipelineStats *stats,
    uint64_t source_sequence_delta,
    int rect_count,
    uint64_t changed_pixels,
    double diff_ms,
    double rfb_process_ms)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_trace
    )
        return;

    double percent =
        (
            (double)changed_pixels /
            (
                (double)stats->cfg->width *
                (double)stats->cfg->height
            )
        ) * 100.0;

    fprintf(
        stderr,
        "[FRAME] source_delta=%llu skipped=%llu "
        "rects=%d changed=%llu px (%.2f%%) "
        "diff=%.3fms rfb=%.3fms "
        "outq=%dB backend_inq=%dB "
        "unacked=%u rtt=%.2fms\n",
        (unsigned long long)source_sequence_delta,
        (unsigned long long)(
            source_sequence_delta > 0
                ? source_sequence_delta - 1
                : 0
        ),
        rect_count,
        (unsigned long long)changed_pixels,
        percent,
        diff_ms,
        rfb_process_ms,
        stats->external_outq_current,
        stats->backend_inq_current,
        stats->tcp_unacked_current,
        (double)stats->tcp_rtt_us_current /
        1000.0
    );
}

void
pipeline_stats_maybe_report(
    PipelineStats *stats)
{
    if (
        !stats ||
        !stats->initialized ||
        !stats->cfg->frame_stats
    )
        return;

    double now =
        pipeline_stats_now();

    pthread_mutex_lock(&stats->mutex);

    double interval =
        now -
        stats->interval_started_at;

    if (
        interval <
        (double)stats->cfg->
            frame_stats_interval_ms /
            1000.0
    ) {
        pthread_mutex_unlock(
            &stats->mutex
        );
        return;
    }

    double capture_fps =
        (double)stats->capture_frames /
        interval;

    double publish_fps =
        (double)stats->publish_frames /
        interval;

    double skipped_fps =
        (double)stats->source_frames_skipped /
        interval;

    double rects_per_frame =
        stats->publish_frames
            ? (double)stats->diff_rectangles /
              (double)stats->publish_frames
            : 0.0;

    double changed_percent =
        stats->publish_frames
            ? (
                (double)stats->diff_pixels /
                (double)stats->publish_frames /
                (
                    (double)stats->cfg->width *
                    (double)stats->cfg->height
                ) *
                100.0
            )
            : 0.0;

    double diff_avg =
        stats->publish_checks
            ? stats->diff_ms_sum /
              (double)stats->publish_checks
            : 0.0;

    double rfb_avg =
        stats->rfb_process_calls
            ? stats->rfb_process_ms_sum /
              (double)stats->rfb_process_calls
            : 0.0;

    double backend_mbps =
        (
            (double)stats->backend_bytes *
            8.0 /
            interval /
            1000000.0
        );

    double ra2_mbps =
        (
            (double)stats->ra2_payload_bytes *
            8.0 /
            interval /
            1000000.0
        );

    double ra2_records_s =
        (double)stats->ra2_records /
        interval;

    double ra2_avg =
        stats->ra2_records
            ? (double)stats->ra2_payload_bytes /
              (double)stats->ra2_records
            : 0.0;

    fprintf(
        stderr,
        "[PIPE] cap=%5.1ffps pub=%5.1ffps skip=%5.1ffps "
        "rect=%5.1f/frame changed=%5.1f%% "
        "diff=%5.2f/%5.2fms rfb=%5.2f/%5.2fms | "
        "backend=%6.2fMbit/s reads=%6.1f/s | "
        "RA2=%6.2fMbit/s rec=%6.1f/s avg=%6.0fB | "
        "queues ext=%d/%dB backend=%d/%dB "
        "unacked=%u/%u rtt=%.2f/%.2fms\n",
        capture_fps,
        publish_fps,
        skipped_fps,
        rects_per_frame,
        changed_percent,
        diff_avg,
        stats->diff_ms_max,
        rfb_avg,
        stats->rfb_process_ms_max,
        backend_mbps,
        (double)stats->backend_reads /
            interval,
        ra2_mbps,
        ra2_records_s,
        ra2_avg,
        stats->external_outq_current,
        stats->external_outq_max,
        stats->backend_inq_current,
        stats->backend_inq_max,
        stats->tcp_unacked_current,
        stats->tcp_unacked_max,
        (double)stats->tcp_rtt_us_current /
            1000.0,
        (double)stats->tcp_rtt_us_max /
            1000.0
    );

    reset_interval_locked(
        stats,
        now
    );

    pthread_mutex_unlock(
        &stats->mutex
    );
}
