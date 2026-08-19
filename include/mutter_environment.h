#ifndef VNC_MONITOR_MUTTER_ENVIRONMENT_H
#define VNC_MONITOR_MUTTER_ENVIRONMENT_H

#include "runtime_config.h"

/*
 * Logs the effective hardware-cursor state of the running gnome-shell by
 * inspecting its startup environment. This is diagnostic only: changing
 * MUTTER_DEBUG_DISABLE_HW_CURSORS after gnome-shell has started cannot
 * reconfigure Mutter.
 *
 * Returns 0 when a matching gnome-shell process was inspected, -1 otherwise.
 */
int mutter_environment_log_hardware_cursor(
    MutterHardwareCursorMode requested_mode);

#endif
