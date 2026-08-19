#ifndef VNC_MONITOR_PIPEWIRE_RESOLVER_H
#define VNC_MONITOR_PIPEWIRE_RESOLVER_H

#include <stdint.h>

int pipewire_resolve_object_serial(
    uint32_t node_id,
    uint64_t *serial_out,
    int timeout_ms);

#endif
