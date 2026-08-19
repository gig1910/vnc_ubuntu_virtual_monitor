#ifndef VNC_MONITOR_PIPEWIRE_CAPTURE_H
#define VNC_MONITOR_PIPEWIRE_CAPTURE_H

#include <pipewire/pipewire.h>
#include <pthread.h>
#include <stddef.h>
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

    /*
     * base_frame is the latest cursor-free Mutter video image.
     * scratch is base_frame plus the latest SPA_META_Cursor sprite.
     * No PipeWire-owned memory survives the process callback.
     */
    uint8_t *base_frame;
    uint8_t *scratch;
    uint8_t *cursor_bitmap;
    size_t cursor_bitmap_capacity;

    uint32_t target_node_id;
    int width;
    int height;
    int fps;
    int negotiated_width;
    int negotiated_height;
    uint32_t negotiated_format;
    int format_ready;

    int cursor_visible;
    int cursor_bitmap_valid;
    int cursor_x;
    int cursor_y;
    int cursor_hotspot_x;
    int cursor_hotspot_y;
    int cursor_width;
    int cursor_height;
    int cursor_stride;

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
    uint64_t video_frames;
    uint64_t empty_buffers;
    uint64_t cursor_updates;
    uint64_t cursor_only_updates;
    uint64_t invalid_cursor_metadata;
    uint64_t invalid_buffers;

    int initialized;
    int have_base_frame;
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
