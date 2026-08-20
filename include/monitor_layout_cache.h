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
 * Verify that Mutter still exposes the virtual monitor created by
 * RecordVirtual. On the supported GNOME/Mutter path these connectors are
 * named Meta-N (the Shell log reports e.g. "Added virtual monitor Meta-0").
 *
 * This check deliberately fails closed: if DisplayConfig cannot be read, do
 * not overwrite a known-good layout cache. This matters for the Shell Stop
 * action, which removes the virtual monitor before our client worker reaches
 * its normal teardown path.
 */
static inline int
monitor_layout_cache_virtual_present(
    MonitorLayoutCache *cache)
{
    if (!cache || !cache->bus)
        return 0;

    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(
            cache->bus,
            "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            "GetCurrentState",
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            &error
        );

    if (!reply) {
        g_clear_error(&error);
        return 0;
    }

    int found = 0;
    GVariant *physical =
        g_variant_get_child_value(reply, 1);

    if (physical && g_variant_is_container(physical)) {
        const gsize count =
            g_variant_n_children(physical);

        for (gsize i = 0; i < count && !found; i++) {
            GVariant *monitor =
                g_variant_get_child_value(physical, i);
            GVariant *spec = monitor
                ? g_variant_get_child_value(monitor, 0)
                : NULL;
            GVariant *connector_value = spec
                ? g_variant_get_child_value(spec, 0)
                : NULL;

            if (connector_value &&
                g_variant_is_of_type(connector_value,
                                     G_VARIANT_TYPE_STRING)) {
                const char *connector =
                    g_variant_get_string(connector_value, NULL);
                found = connector &&
                        g_str_has_prefix(connector, "Meta-");
            }

            if (connector_value)
                g_variant_unref(connector_value);
            if (spec)
                g_variant_unref(spec);
            if (monitor)
                g_variant_unref(monitor);
        }
    }

    if (physical)
        g_variant_unref(physical);
    g_variant_unref(reply);
    return found;
}

/*
 * Production session policy: layout-remember means "remember the latest
 * arrangement", not merely "save once". Keep layout-resave accepted by the
 * parser for configuration compatibility, but force an overwrite when the
 * active client session is being finalized.
 *
 * A Shell-side Stop has already removed Meta-N by the time normal teardown is
 * reached; in that case leave the previous valid cache untouched rather than
 * replacing it with a physical-monitor-only topology.
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

    if (!monitor_layout_cache_virtual_present(cache))
        return 0;

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
