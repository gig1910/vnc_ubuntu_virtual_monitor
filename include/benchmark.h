#ifndef VNC_MONITOR_BENCHMARK_H
#define VNC_MONITOR_BENCHMARK_H

#include <stddef.h>
#include <stdint.h>

#include "runtime_config.h"

int benchmark_init(const RuntimeConfig *cfg);
void benchmark_shutdown(void);

double benchmark_now(void);

void benchmark_set_stage(
    const char *stage,
    int square_size);

void benchmark_record_frame(
    uint64_t dirty_pixels,
    double render_ms,
    double rfb_process_ms,
    double frame_lateness_ms);

void benchmark_record_backend_read(size_t bytes);

void benchmark_record_ra2_out(
    size_t payload_bytes,
    double send_ms);

void benchmark_record_ra2_in(
    size_t payload_bytes);

void benchmark_maybe_report(void);

#endif
