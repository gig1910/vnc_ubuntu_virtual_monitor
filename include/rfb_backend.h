#ifndef VNC_MONITOR_RFB_BACKEND_H
#define VNC_MONITOR_RFB_BACKEND_H

#include <pthread.h>
#include <rfb/rfb.h>

#include "runtime_config.h"
#include "frame_bridge.h"
#include "pipeline_stats.h"
#include "real_monitor.h"
#include "monitor_layout_cache.h"

/*
 * rfbNewFramebuffer() reinitializes LibVNCServer's serverFormat from host
 * endianness. Our framebuffer storage is always BGRx, so on little-endian
 * hosts we must restore R=16/G=8/B=0 afterwards and rebuild every client's
 * translation table before any resized pixels are sent.
 *
 * Keep the wrapper at the backend boundary so resize callers cannot forget
 * this invariant. The macro below affects translation units including this
 * backend header; the wrapper itself is defined before the macro and therefore
 * calls LibVNCServer's original rfbNewFramebuffer().
 */
static inline void
rfb_backend_new_framebuffer_bgrx(
    rfbScreenInfoPtr screen,
    char *framebuffer,
    int width,
    int height,
    int bits_per_sample,
    int samples_per_pixel,
    int bytes_per_pixel)
{
    rfbNewFramebuffer(screen,
                      framebuffer,
                      width,
                      height,
                      bits_per_sample,
                      samples_per_pixel,
                      bytes_per_pixel);

    screen->serverFormat.bitsPerPixel = 32;
    screen->serverFormat.depth = 24;
    screen->serverFormat.bigEndian = FALSE;
    screen->serverFormat.trueColour = TRUE;
    screen->serverFormat.redMax = 255;
    screen->serverFormat.greenMax = 255;
    screen->serverFormat.blueMax = 255;
    screen->serverFormat.redShift = 16;
    screen->serverFormat.greenShift = 8;
    screen->serverFormat.blueShift = 0;

    if (!screen->setTranslateFunction)
        return;

    rfbClientIteratorPtr iterator = rfbGetClientIterator(screen);
    rfbClientPtr client;

    while (iterator && (client = rfbClientIteratorNext(iterator)) != NULL)
        (void)screen->setTranslateFunction(client);

    if (iterator)
        rfbReleaseClientIterator(iterator);
}

#define rfbNewFramebuffer rfb_backend_new_framebuffer_bgrx

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int failed;
    int stop;

    RuntimeConfig *cfg;
    FrameBridge *frames;
    PipelineStats *stats;
    RealMonitor *real_monitor;
    MonitorLayoutCache *layout_cache;
} RfbBackend;

int rfb_backend_start(
    RfbBackend *backend,
    RuntimeConfig *cfg,
    FrameBridge *frames,
    PipelineStats *stats,
    RealMonitor *real_monitor,
    MonitorLayoutCache *layout_cache);

void rfb_backend_stop(RfbBackend *backend);

#endif
