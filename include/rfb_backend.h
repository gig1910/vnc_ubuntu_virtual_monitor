#ifndef VNC_MONITOR_RFB_BACKEND_H
#define VNC_MONITOR_RFB_BACKEND_H

#include <pthread.h>

#include "runtime_config.h"
#include "frame_bridge.h"
#include "pipeline_stats.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int failed;
    int stop;

    const RuntimeConfig *cfg;
    FrameBridge *frames;
    PipelineStats *stats;
} RfbBackend;

int rfb_backend_start(
    RfbBackend *backend,
    const RuntimeConfig *cfg,
    FrameBridge *frames,
    PipelineStats *stats);

void rfb_backend_stop(RfbBackend *backend);

#endif
