#ifndef VNC_MONITOR_MUTTER_VIRTUAL_MONITOR_H
#define VNC_MONITOR_MUTTER_VIRTUAL_MONITOR_H

#include <gio/gio.h>
#include <stdint.h>

#include "runtime_config.h"

typedef struct {
    GDBusConnection *bus;
    char *rd_session_path;
    char *sc_session_path;
    char *stream_path;
    uint32_t node_id;
    int started;

    /*
     * The RFB relay does not run a GLib main loop. A dedicated context/thread
     * watches ScreenCast.Session.Closed and DisplayConfig.MonitorsChanged.
     */
    GThread *lifecycle_thread;
    GMainContext *lifecycle_context;
    GMutex lifecycle_mutex;
    GCond lifecycle_cond;
    guint lifecycle_closed_subscription;
    guint lifecycle_monitors_subscription;
    guint lifecycle_display_change_seq;
    guint lifecycle_display_change_seq_at_close;
    gint64 lifecycle_closed_at_us;
    gint64 lifecycle_last_display_change_us;
    int lifecycle_sync_initialized;
    int lifecycle_ready;
    int lifecycle_stop_requested;
    int lifecycle_closed;
    int lifecycle_intentional_stop;
} MutterVirtualMonitor;

int mutter_virtual_monitor_start(
    MutterVirtualMonitor *monitor,
    int timeout_ms,
    MutterCursorMode cursor_mode);

/*
 * Stable process-wide notification fd used by the single active RFB relay.
 * It becomes readable when Mutter closes the active ScreenCast session for an
 * external reason (for example GNOME Shell's Stop sharing action). Internal
 * monitor recreation/teardown does not signal this fd.
 */
int mutter_virtual_monitor_lifecycle_fd(void);
void mutter_virtual_monitor_lifecycle_drain(void);

void mutter_virtual_monitor_stop(
    MutterVirtualMonitor *monitor);

#endif
