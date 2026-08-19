#ifndef VNC_MONITOR_RFB_PROXY_H
#define VNC_MONITOR_RFB_PROXY_H

#include "ra2.h"
#include "runtime_config.h"
#include "pipeline_stats.h"

int rfb_proxy_run(
    int external_fd,
    Ra2Session *session,
    const RuntimeConfig *cfg,
    PipelineStats *stats);

#endif
