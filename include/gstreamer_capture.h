#ifndef VNC_MONITOR_GSTREAMER_CAPTURE_H
#define VNC_MONITOR_GSTREAMER_CAPTURE_H

#include <gst/gst.h>
#include <pthread.h>
#include <stdint.h>

#include "frame_bridge.h"
#include "pipeline_stats.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    GstElement *pipeline;
    GstElement *appsink;

    FrameBridge *bridge;
    PipelineStats *stats;
    uint64_t object_serial;

    int width;
    int height;
    int fps;
    int caps_logged;

    int capture_trace;
    int capture_stall_ms;
    uint64_t sample_sequence;
    uint64_t last_sample_ns;
    uint64_t interval_count;
    uint64_t interval_histogram[6];
    uint64_t stall_count;
    double max_interval_ms;

    int initialized;
    int stop;
    int running;
    int first_frame;
    int failed;
} GstreamerCapture;

int gstreamer_capture_start(
    GstreamerCapture *capture,
    uint64_t object_serial,
    int width,
    int height,
    int fps,
    int timeout_ms,
    int capture_trace,
    int capture_stall_ms,
    FrameBridge *bridge,
    PipelineStats *stats);

void gstreamer_capture_stop(
    GstreamerCapture *capture);

#endif
