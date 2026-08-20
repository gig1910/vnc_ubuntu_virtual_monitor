#ifndef VNC_MONITOR_LAYOUT_CACHE_H
#define VNC_MONITOR_LAYOUT_CACHE_H

#include <gio/gio.h>

#include "runtime_config.h"

typedef struct {
    GDBusConnection *bus;
    char *cache_path;
    int cache_existed;
    int prepared;
} MonitorLayoutCache;

/*
 * The ScreenCast RecordVirtual API does not provide a custom connector
 * identity. Layout persistence is implemented against Mutter's live
 * org.gnome.Mutter.DisplayConfig state:
 *
 *   connect:     create virtual monitor and apply the last saved layout
 *   disconnect:  save the current live layout before removing the monitor
 *
 * Nothing is copied into ~/.config/monitors.xml.
 */
int monitor_layout_cache_prepare(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg);

int monitor_layout_cache_apply(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg,
    int timeout_ms);

int monitor_layout_cache_save(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg);

/*
 * Production session policy: layout-remember means "remember the latest
 * arrangement", not merely "save once". Keep layout-resave accepted by the
 * parser for configuration compatibility, but force an overwrite when the
 * active client session is being finalized.
 *
 * main.c includes real_monitor.h before this header, so only the session
 * orchestration path gets the compatibility wrapper. monitor_layout_cache.c
 * itself still defines/calls the real function normally.
 */
static inline int
monitor_layout_cache_save_latest(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg)
{
    if (!cfg)
        return -1;

    RuntimeConfig effective = *cfg;
    effective.layout_resave = 1;
    return monitor_layout_cache_save(cache, &effective);
}

#ifdef VNC_MONITOR_REAL_MONITOR_H
#define monitor_layout_cache_save(cache, cfg) \
    monitor_layout_cache_save_latest((cache), (cfg))
#endif

/*
 * Diagnostic only: print the current mode/refresh reported by Mutter's
 * DisplayConfig for monitors matching the requested framebuffer size.
 */
int monitor_layout_log_matching_modes(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg);

void monitor_layout_cache_clear(
    MonitorLayoutCache *cache);

#endif
