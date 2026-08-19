#ifndef VNC_MONITOR_REAL_MONITOR_H
#define VNC_MONITOR_REAL_MONITOR_H

#include "frame_bridge.h"
#include "gstreamer_capture.h"
#include "mutter_virtual_monitor.h"
#include "pipewire_capture.h"
#include "runtime_config.h"
#include "pipeline_stats.h"

typedef struct {
    MutterVirtualMonitor monitor;
    PipewireCapture pipewire_capture;
    GstreamerCapture gstreamer_capture;
    CaptureBackend capture_backend;
    int active;
} RealMonitor;

int real_monitor_start(
    RealMonitor *real,
    const RuntimeConfig *cfg,
    FrameBridge *bridge,
    PipelineStats *stats);

void real_monitor_stop(RealMonitor *real);

#endif
