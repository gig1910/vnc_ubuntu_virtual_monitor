#ifndef VNC_MONITOR_PIPEWIRE_CAPTURE_H
#define VNC_MONITOR_PIPEWIRE_CAPTURE_H

#include <pipewire/pipewire.h>
#include <pthread.h>
#include <stdint.h>

#include "frame_bridge.h"
#include "pipeline_stats.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    struct pw_thread_loop *loop;
    struct pw_stream *stream;

    FrameBridge *bridge;
    PipelineStats *stats;
    uint8_t *scratch;

    uint32_t target_node_id;
    int width;
    int height;
    int fps;
    int negotiated_width;
    int negotiated_height;
    uint32_t negotiated_format;
    int format_ready;

    int capture_trace;
    int capture_stall_ms;
    uint64_t sample_sequence;
    uint64_t last_sample_ns;
    uint64_t interval_count;
    uint64_t interval_histogram[6];
    uint64_t stall_count;
    double max_interval_ms;

    uint64_t process_callbacks;
    uint64_t dequeued_buffers;
    uint64_t stale_buffers_recycled;
    uint64_t invalid_buffers;

    int initialized;
    int first_frame;
    int failed;
    int streaming;
} PipewireCapture;

int pipewire_capture_start(
    PipewireCapture *capture,
    uint32_t node_id,
    int width,
    int height,
    int fps,
    int timeout_ms,
    int capture_trace,
    int capture_stall_ms,
    FrameBridge *bridge,
    PipelineStats *stats);

void pipewire_capture_stop(PipewireCapture *capture);

#endif
