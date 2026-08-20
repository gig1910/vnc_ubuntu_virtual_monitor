#include "real_monitor.h"
#include "pipewire_resolver.h"
#include "log.h"

#include <inttypes.h>
#include <string.h>

static void
real_monitor_stop_internal(RealMonitor *real, int log_removed)
{
    if (!real)
        return;

    if (real->active) {
        if (real->capture_backend == CAPTURE_BACKEND_PIPEWIRE)
            pipewire_capture_stop(&real->pipewire_capture);
        else
            gstreamer_capture_stop(&real->gstreamer_capture);

        mutter_virtual_monitor_stop(&real->monitor);
    }

    real->active = 0;

    if (log_removed)
        LOG_INFO("Virtual monitor removed");
}

int
real_monitor_start(RealMonitor *real,
                   const RuntimeConfig *cfg,
                   FrameBridge *bridge,
                   PipelineStats *stats)
{
    if (!real || !cfg || !bridge)
        return -1;

    memset(real, 0, sizeof(*real));
    real->capture_backend = cfg->capture_backend;

    LOG_DEBUG("Creating Mutter virtual monitor in current Wayland session");

    if (mutter_virtual_monitor_start(&real->monitor,
                                     cfg->capture_timeout_ms,
                                     cfg->mutter_cursor_mode) < 0)
        return -1;

    LOG_DEBUG("Mutter RecordVirtual stream=%s node_id=%u",
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
            LOG_ERROR("Could not resolve PipeWire object.serial for node %u",
                      real->monitor.node_id);
            mutter_virtual_monitor_stop(&real->monitor);
            return -1;
        }

        LOG_DEBUG("PipeWire node %u -> object.serial=%" PRIu64,
                  real->monitor.node_id,
                  serial);

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
    LOG_INFO("Virtual monitor active: %dx%d, capture=%s",
             cfg->width,
             cfg->height,
             runtime_config_capture_backend_name(cfg->capture_backend));
    return 0;
}

int
real_monitor_resize(RealMonitor *real,
                    RuntimeConfig *cfg,
                    FrameBridge *bridge,
                    PipelineStats *stats,
                    int width,
                    int height)
{
    if (!real || !cfg || !bridge || width <= 0 || height <= 0)
        return -1;

    if (cfg->width == width && cfg->height == height)
        return 0;

    int old_width = cfg->width;
    int old_height = cfg->height;

    LOG_INFO("Resizing virtual monitor: %dx%d -> %dx%d",
             old_width,
             old_height,
             width,
             height);

    real_monitor_stop_internal(real, 0);

    if (frame_bridge_resize(bridge, width, height) < 0) {
        LOG_ERROR("Could not resize FrameBridge to %dx%d", width, height);
        cfg->width = old_width;
        cfg->height = old_height;
        (void)real_monitor_start(real, cfg, bridge, stats);
        return -1;
    }

    cfg->width = width;
    cfg->height = height;

    if (real_monitor_start(real, cfg, bridge, stats) == 0) {
        LOG_INFO("Virtual monitor resize complete: %dx%d", width, height);
        return 0;
    }

    LOG_ERROR("Virtual monitor resize to %dx%d failed; restoring %dx%d",
              width,
              height,
              old_width,
              old_height);

    real_monitor_stop_internal(real, 0);

    if (frame_bridge_resize(bridge, old_width, old_height) < 0) {
        LOG_ERROR("Could not restore FrameBridge to %dx%d", old_width, old_height);
        return -1;
    }

    cfg->width = old_width;
    cfg->height = old_height;

    if (real_monitor_start(real, cfg, bridge, stats) < 0) {
        LOG_ERROR("Could not restore previous virtual monitor after failed resize");
        return -1;
    }

    LOG_INFO("Previous virtual monitor restored: %dx%d",
             old_width,
             old_height);
    return -1;
}

void
real_monitor_stop(RealMonitor *real)
{
    real_monitor_stop_internal(real, 1);
}
