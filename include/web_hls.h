#ifndef VNC_MONITOR_WEB_HLS_H
#define VNC_MONITOR_WEB_HLS_H

#include "frame_bridge.h"

#include <gst/gst.h>
#include <pthread.h>
#include <limits.h>

typedef struct {
    pthread_mutex_t mutex;
    int initialized;
    int stop;
    int thread_started;
    int ready_sent;
    int control_fd;
    int width;
    int height;
    int fps;

    pthread_t thread;
    GstElement *pipeline;
    GstElement *appsrc;
    FrameBridge *frames;

    char directory[PATH_MAX];
    char playlist[PATH_MAX];
} WebHlsStream;

/*
 * Experimental iOS 9 compatibility path:
 * raw FrameBridge BGRx -> H.264 baseline -> MPEG-TS HLS on disk.
 *
 * Returns 0 when the HLS pipeline was started. Missing runtime GStreamer
 * encoder/HLS plugins are reported as a normal failure so the caller can
 * fall back to the existing WSS/JPEG path.
 */
int web_hls_start(WebHlsStream *stream,
                  int control_fd,
                  FrameBridge *frames,
                  int width,
                  int height,
                  int fps);

void web_hls_stop(WebHlsStream *stream);

#endif
