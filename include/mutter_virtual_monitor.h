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
} MutterVirtualMonitor;

int mutter_virtual_monitor_start(
    MutterVirtualMonitor *monitor,
    int timeout_ms,
    MutterCursorMode cursor_mode);

void mutter_virtual_monitor_stop(
    MutterVirtualMonitor *monitor);

#endif
