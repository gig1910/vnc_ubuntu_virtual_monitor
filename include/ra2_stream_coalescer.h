#ifndef VNC_MONITOR_RA2_STREAM_COALESCER_H
#define VNC_MONITOR_RA2_STREAM_COALESCER_H

#include "ra2.h"
#include "runtime_config.h"
#include "pipeline_stats.h"

typedef enum {
    RA2_COALESCE_OK = 0,
    RA2_COALESCE_BACKEND_EOF = 1,
    RA2_COALESCE_SHUTDOWN = 2,
    RA2_COALESCE_ERROR = -1
} Ra2CoalesceResult;

/*
 * Called when backend_fd is readable.
 *
 * Reads the first available backend chunk, then optionally coalesces
 * immediately available/burst data up to cfg->ra2_stream_record_max.
 * A small cfg->ra2_coalesce_us window can be used to let the current
 * LibVNCServer burst accumulate without waiting for a full record.
 */
Ra2CoalesceResult ra2_stream_coalesce_and_send(
    int backend_fd,
    int external_fd,
    Ra2Direction *server_to_client,
    const RuntimeConfig *cfg,
    PipelineStats *stats,
    unsigned long long *records_sent,
    unsigned long long *bytes_sent);

#endif
