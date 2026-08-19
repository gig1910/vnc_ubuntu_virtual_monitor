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
 * identity.  Layout persistence is therefore implemented against Mutter's
 * live org.gnome.Mutter.DisplayConfig state:
 *
 *   first run:  arrange monitors in GNOME, save GetCurrentState() on disconnect
 *   later run:  create virtual monitor, then ApplyMonitorsConfig() using the
 *               saved logical geometry and the current connector/mode IDs
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
 * Diagnostic only: print the current mode/refresh reported by Mutter's
 * DisplayConfig for monitors matching the requested framebuffer size.
 */
int monitor_layout_log_matching_modes(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg);

void monitor_layout_cache_clear(
    MonitorLayoutCache *cache);

#endif
