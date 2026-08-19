#ifndef VNC_MONITOR_PIPELINE_STATS_H
#define VNC_MONITOR_PIPELINE_STATS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_config.h"

typedef struct {
    pthread_mutex_t mutex;
    int initialized;
    const RuntimeConfig *cfg;

    double started_at;
    double interval_started_at;

    uint64_t capture_frames;
    uint64_t capture_bytes;

    uint64_t publish_checks;
    uint64_t publish_frames;
    uint64_t source_frames_skipped;

    uint64_t diff_rectangles;
    uint64_t diff_pixels;
    double diff_ms_sum;
    double diff_ms_max;

    double rfb_process_ms_sum;
    double rfb_process_ms_max;
    uint64_t rfb_process_calls;

    uint64_t backend_reads;
    uint64_t backend_bytes;

    uint64_t ra2_records;
    uint64_t ra2_payload_bytes;

    int external_outq_current;
    int external_outq_max;

    int backend_inq_current;
    int backend_inq_max;

    uint32_t tcp_unacked_current;
    uint32_t tcp_unacked_max;
    uint32_t tcp_rtt_us_current;
    uint32_t tcp_rtt_us_max;
} PipelineStats;

int pipeline_stats_init(
    PipelineStats *stats,
    const RuntimeConfig *cfg);

void pipeline_stats_destroy(
    PipelineStats *stats);

double pipeline_stats_now(void);

void pipeline_stats_capture(
    PipelineStats *stats,
    size_t bytes);

void pipeline_stats_publish(
    PipelineStats *stats,
    uint64_t source_sequence_delta,
    int rect_count,
    uint64_t changed_pixels,
    double diff_ms,
    double rfb_process_ms);

void pipeline_stats_backend_read(
    PipelineStats *stats,
    size_t bytes);

void pipeline_stats_ra2_send(
    PipelineStats *stats,
    size_t payload_bytes);

void pipeline_stats_transport_queues(
    PipelineStats *stats,
    int external_fd,
    int backend_fd);

void pipeline_stats_maybe_report(
    PipelineStats *stats);

void pipeline_stats_trace_publish(
    PipelineStats *stats,
    uint64_t source_sequence_delta,
    int rect_count,
    uint64_t changed_pixels,
    double diff_ms,
    double rfb_process_ms);

#endif
