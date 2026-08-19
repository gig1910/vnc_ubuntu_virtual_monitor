#include "real_monitor.h"

#include "pipewire_resolver.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int
real_monitor_start(
    RealMonitor *real,
    const RuntimeConfig *cfg,
    FrameBridge *bridge,
    PipelineStats *stats)
{
    if (!real || !cfg || !bridge)
        return -1;

    memset(real, 0, sizeof(*real));
    real->capture_backend = cfg->capture_backend;

    printf("Creating Mutter virtual monitor in current Wayland session...\n");

    if (mutter_virtual_monitor_start(&real->monitor,
                                     cfg->capture_timeout_ms,
                                     cfg->mutter_cursor_mode) < 0)
        return -1;

    printf("Capture source: Mutter RecordVirtual stream=%s node_id=%u "
           "(single virtual monitor stream)\n",
           real->monitor.stream_path ? real->monitor.stream_path : "(unknown)",
           real->monitor.node_id);

    if (cfg->capture_backend == CAPTURE_BACKEND_PIPEWIRE) {
        if (pipewire_capture_start(&real->pipewire_capture,
                                   real->monitor.node_id,
                                   cfg->width,
                                   cfg->height,
                                   cfg->max_fps,
                                   cfg->capture_timeout_ms,
                                   cfg->capture_trace,
                                   cfg->capture_stall_ms,
                                   bridge,
                                   stats) < 0) {
            mutter_virtual_monitor_stop(&real->monitor);
            return -1;
        }
    }
    else {
        uint64_t serial = 0;
        if (pipewire_resolve_object_serial(real->monitor.node_id,
                                           &serial,
                                           cfg->capture_timeout_ms) < 0) {
            fprintf(stderr,
                    "Could not resolve PipeWire object.serial for node %u\n",
                    real->monitor.node_id);
            mutter_virtual_monitor_stop(&real->monitor);
            return -1;
        }

        printf("PipeWire node %u -> object.serial=%" PRIu64 "\n",
               real->monitor.node_id, serial);

        if (gstreamer_capture_start(&real->gstreamer_capture,
                                    serial,
                                    cfg->width,
                                    cfg->height,
                                    cfg->max_fps,
                                    cfg->capture_timeout_ms,
                                    cfg->capture_trace,
                                    cfg->capture_stall_ms,
                                    bridge,
                                    stats) < 0) {
            mutter_virtual_monitor_stop(&real->monitor);
            return -1;
        }
    }

    real->active = 1;
    printf("Real virtual-monitor source is ready (capture backend=%s)\n",
           runtime_config_capture_backend_name(cfg->capture_backend));
    return 0;
}

void
real_monitor_stop(RealMonitor *real)
{
    if (!real)
        return;

    if (real->active)
        printf("Stopping real virtual-monitor source...\n");

    if (real->capture_backend == CAPTURE_BACKEND_PIPEWIRE)
        pipewire_capture_stop(&real->pipewire_capture);
    else
        gstreamer_capture_stop(&real->gstreamer_capture);

    mutter_virtual_monitor_stop(&real->monitor);
    real->active = 0;
}
