#ifndef VNC_MONITOR_RFB_BACKEND_H
#define VNC_MONITOR_RFB_BACKEND_H

#include <pthread.h>

#include "runtime_config.h"
#include "frame_bridge.h"
#include "pipeline_stats.h"
#include "real_monitor.h"
#include "monitor_layout_cache.h"

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int failed;
    int stop;

    RuntimeConfig *cfg;
    FrameBridge *frames;
    PipelineStats *stats;
    RealMonitor *real_monitor;
    MonitorLayoutCache *layout_cache;
} RfbBackend;

int rfb_backend_start(
    RfbBackend *backend,
    RuntimeConfig *cfg,
    FrameBridge *frames,
    PipelineStats *stats,
    RealMonitor *real_monitor,
    MonitorLayoutCache *layout_cache);

void rfb_backend_stop(RfbBackend *backend);

#endif
