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
     * Mutter's shell Stop action is authoritative even on versions where a
     * usable Session.Closed signal is not delivered to this client. A small
     * watcher therefore verifies that the exact RemoteDesktop session object
     * remains alive and translates its disappearance to the RFB relay fd.
     */
    GThread *lifecycle_thread;
    GMutex lifecycle_mutex;
    int lifecycle_sync_initialized;
    int lifecycle_stop_requested;
    int lifecycle_intentional_stop;
    int lifecycle_closed;
    guint32 lifecycle_last_display_serial;
    guint32 lifecycle_display_serial_before_close;
} MutterVirtualMonitor;

int mutter_virtual_monitor_start(
    MutterVirtualMonitor *monitor,
    int timeout_ms,
    MutterCursorMode cursor_mode);

/*
 * Stable process-wide notification fd used by the single active RFB relay.
 * It becomes readable when Mutter destroys the active RemoteDesktop session
 * externally (for example GNOME Shell's Stop sharing action). Internal monitor
 * recreation/teardown never signals this fd.
 */
int mutter_virtual_monitor_lifecycle_fd(void);
void mutter_virtual_monitor_lifecycle_drain(void);

void mutter_virtual_monitor_stop(
    MutterVirtualMonitor *monitor);

#endif
