#include "rfb_backend.h"
#include "frame_diff.h"
#include "adaptive_rfb.h"
#include "shutdown_signal.h"
#include "log.h"

#include <rfb/rfb.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    RfbBackend *backend;
    rfbScreenInfoPtr screen;
    uint8_t *fb;
    AdaptiveRfbState *adaptive;
    FrameDiffRect *diff_rects;
    size_t max_diff_rects;
    int width;
    int height;
    uint64_t resize_generation;
} BackendRuntime;

static void
ignore_keyboard(rfbBool down, rfbKeySym keySym, rfbClientPtr cl)
{
    (void)down;
    (void)keySym;
    (void)cl;
}

static void
ignore_pointer(int buttonMask, int x, int y, rfbClientPtr cl)
{
    (void)buttonMask;
    (void)x;
    (void)y;
    (void)cl;
}

static void
ignore_cut_text(char *text, int len, rfbClientPtr cl)
{
    (void)text;
    (void)len;
    (void)cl;
}

static double
monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void
signal_ready(RfbBackend *backend, int failed)
{
    pthread_mutex_lock(&backend->mutex);
    backend->failed = failed;
    backend->ready = 1;
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);
}

static size_t
max_diff_rectangles(int width, int height, int tile_size)
{
    return
        (size_t)((width + tile_size - 1) / tile_size) *
        (size_t)((height + tile_size - 1) / tile_size);
}

/*
 * The relay thread updates these counters from real sockets. The publisher
 * uses them only as a coarse backpressure signal so it never manufactures a
 * long queue of stale framebuffer work.
 */
static int
transport_backpressured(PipelineStats *stats,
                         const RuntimeConfig *cfg,
                         int *external_outq,
                         int *backend_inq,
                         uint32_t *unacked)
{
    if (!stats || !stats->initialized)
        return 0;

    pthread_mutex_lock(&stats->mutex);
    int ext = stats->external_outq_current;
    int back = stats->backend_inq_current;
    uint32_t ua = stats->tcp_unacked_current;
    pthread_mutex_unlock(&stats->mutex);

    if (external_outq)
        *external_outq = ext;
    if (backend_inq)
        *backend_inq = back;
    if (unacked)
        *unacked = ua;

    return ext >= cfg->external_send_buffer ||
           back >= cfg->backend_receive_buffer ||
           ua >= 24;
}

static int
transport_recovered(const RuntimeConfig *cfg,
                    int external_outq,
                    int backend_inq,
                    uint32_t unacked)
{
    int external_low = cfg->external_send_buffer / 4;
    int backend_low = cfg->backend_receive_buffer / 4;

    if (external_low < 4096)
        external_low = 4096;
    if (backend_low < 4096)
        backend_low = 4096;

    return external_outq <= external_low &&
           backend_inq <= backend_low &&
           unacked <= 8;
}

static int
transport_idle_ready(RfbBackend *backend,
                     const RuntimeConfig *cfg,
                     int *external_outq,
                     int *backend_inq,
                     uint32_t *unacked)
{
    int ext = 0;
    int back = 0;
    uint32_t ua = 0;

    int pressured = transport_backpressured(backend->stats,
                                            cfg,
                                            &ext,
                                            &back,
                                            &ua);

    if (external_outq)
        *external_outq = ext;
    if (backend_inq)
        *backend_inq = back;
    if (unacked)
        *unacked = ua;

    return !pressured && transport_recovered(cfg, ext, back, ua);
}

static void
queue_full_update(AdaptiveRfbState *adaptive,
                  rfbScreenInfoPtr screen,
                  int width,
                  int height,
                  uint64_t sequence,
                  double changed_percent,
                  int *queued_jpeg)
{
    if (adaptive_rfb_should_queue_jpeg(adaptive, changed_percent)) {
        adaptive_rfb_queue_jpeg_rect(adaptive,
                                     0,
                                     0,
                                     width,
                                     height,
                                     sequence,
                                     changed_percent);
        *queued_jpeg = 1;
    }
    else {
        rfbMarkRectAsModified(screen, 0, 0, width, height);
        adaptive_rfb_note_lossless_rect(adaptive,
                                        0,
                                        0,
                                        width,
                                        height);
    }
}

static int
valid_single_screen_layout(int width,
                           int height,
                           int num_screens,
                           rfbExtDesktopScreen *screens)
{
    if (num_screens != 1 || !screens)
        return 0;

    return screens[0].x == 0 &&
           screens[0].y == 0 &&
           screens[0].width == width &&
           screens[0].height == height;
}

static void
refresh_layout_cache_after_resize(RfbBackend *backend)
{
    if (!backend || !backend->layout_cache || !backend->cfg)
        return;

    monitor_layout_cache_clear(backend->layout_cache);

    if (monitor_layout_cache_prepare(backend->layout_cache, backend->cfg) < 0) {
        LOG_DEBUG("No cached layout prepared for resized monitor %dx%d",
                  backend->cfg->width,
                  backend->cfg->height);
        return;
    }

    if (monitor_layout_cache_apply(backend->layout_cache,
                                   backend->cfg,
                                   backend->cfg->capture_timeout_ms) < 0) {
        LOG_DEBUG("No cached layout applied for resized monitor %dx%d",
                  backend->cfg->width,
                  backend->cfg->height);
    }

    if (vnc_log_enabled(VNC_LOG_DEBUG))
        (void)monitor_layout_log_matching_modes(backend->layout_cache,
                                                backend->cfg);
}

static int
set_desktop_size(int width,
                 int height,
                 int num_screens,
                 rfbExtDesktopScreen *screens,
                 rfbClientPtr cl)
{
    if (!cl || !cl->screen || !cl->screen->screenData)
        return rfbExtDesktopSize_OutOfResources;

    BackendRuntime *runtime = cl->screen->screenData;
    RfbBackend *backend = runtime->backend;
    RuntimeConfig *cfg = backend ? backend->cfg : NULL;

    if (!backend || !cfg || !backend->real_monitor)
        return rfbExtDesktopSize_OutOfResources;

    if (cfg->screen_size_mode == SCREEN_SIZE_FIXED) {
        LOG_DEBUG("Client resize rejected: display mode is fixed");
        return rfbExtDesktopSize_ResizeProhibited;
    }

    if (width < 64 || height < 64 || width > 16384 || height > 16384) {
        LOG_DEBUG("Client resize rejected: unsupported size %dx%d", width, height);
        return rfbExtDesktopSize_InvalidScreenLayout;
    }

    if (!valid_single_screen_layout(width, height, num_screens, screens)) {
        LOG_DEBUG("Client resize rejected: expected one screen spanning %dx%d",
                  width,
                  height);
        return rfbExtDesktopSize_InvalidScreenLayout;
    }

    if (width == runtime->width && height == runtime->height)
        return rfbExtDesktopSize_Success;

    size_t new_max_diff_rects =
        max_diff_rectangles(width, height, cfg->diff_tile_size);

    uint8_t *new_fb = calloc((size_t)width * (size_t)height, 4);
    FrameDiffRect *new_diff_rects =
        calloc(new_max_diff_rects, sizeof(*new_diff_rects));

    if (!new_fb || !new_diff_rects) {
        free(new_fb);
        free(new_diff_rects);
        LOG_ERROR("Client resize %dx%d rejected: backend allocation failed",
                  width,
                  height);
        return rfbExtDesktopSize_OutOfResources;
    }

    int old_width = runtime->width;
    int old_height = runtime->height;

    if (real_monitor_resize(backend->real_monitor,
                            cfg,
                            backend->frames,
                            backend->stats,
                            width,
                            height) < 0) {
        free(new_fb);
        free(new_diff_rects);
        return rfbExtDesktopSize_OutOfResources;
    }

    if (adaptive_rfb_resize(runtime->adaptive, width, height) < 0) {
        LOG_ERROR("Client resize %dx%d failed while resizing adaptive transport; rolling back monitor",
                  width,
                  height);

        (void)real_monitor_resize(backend->real_monitor,
                                  cfg,
                                  backend->frames,
                                  backend->stats,
                                  old_width,
                                  old_height);
        refresh_layout_cache_after_resize(backend);
        free(new_fb);
        free(new_diff_rects);
        return rfbExtDesktopSize_OutOfResources;
    }

    uint8_t *old_fb = runtime->fb;
    FrameDiffRect *old_diff_rects = runtime->diff_rects;

    /*
     * LibVNCServer updates all connected-client regions and schedules
     * NewFBSize/ExtendedDesktopSize as appropriate. The SetDesktopSize
     * handler will send the mandatory ExtendedDesktopSize result after this
     * hook returns.
     */
    rfbNewFramebuffer(runtime->screen,
                      (char *)new_fb,
                      width,
                      height,
                      8,
                      3,
                      4);

    runtime->fb = new_fb;
    runtime->diff_rects = new_diff_rects;
    runtime->max_diff_rects = new_max_diff_rects;
    runtime->width = width;
    runtime->height = height;
    runtime->resize_generation++;

    free(old_diff_rects);
    free(old_fb);

    refresh_layout_cache_after_resize(backend);

    LOG_INFO("Accepted client framebuffer resize: %dx%d", width, height);
    return rfbExtDesktopSize_Success;
}

static void *
backend_thread(void *arg)
{
    RfbBackend *backend = arg;
    RuntimeConfig *cfg = backend->cfg;

    BackendRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.backend = backend;
    runtime.width = cfg->width;
    runtime.height = cfg->height;
    runtime.max_diff_rects =
        max_diff_rectangles(runtime.width,
                            runtime.height,
                            cfg->diff_tile_size);

    runtime.fb =
        calloc((size_t)runtime.width * (size_t)runtime.height, 4);
    runtime.diff_rects =
        calloc(runtime.max_diff_rects, sizeof(*runtime.diff_rects));

    if (!runtime.fb || !runtime.diff_rects) {
        free(runtime.diff_rects);
        free(runtime.fb);
        signal_ready(backend, 1);
        return NULL;
    }

    int argc = 1;
    char arg0[] = "vnc-monitor-backend";
    char *argv[] = {arg0, NULL};

    rfbScreenInfoPtr screen = rfbGetScreen(&argc,
                                           argv,
                                           runtime.width,
                                           runtime.height,
                                           8,
                                           3,
                                           4);
    if (!screen) {
        free(runtime.diff_rects);
        free(runtime.fb);
        signal_ready(backend, 1);
        return NULL;
    }

    runtime.screen = screen;
    screen->screenData = &runtime;
    screen->desktopName = "VNC Monitor";
    screen->frameBuffer = (char *)runtime.fb;

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

    struct in_addr backend_addr;
    if (inet_pton(AF_INET, cfg->backend_bind, &backend_addr) != 1) {
        LOG_ERROR("Invalid backend IPv4 bind address: %s", cfg->backend_bind);
        rfbScreenCleanup(screen);
        free(runtime.diff_rects);
        free(runtime.fb);
        signal_ready(backend, 1);
        return NULL;
    }

    screen->listenInterface = backend_addr.s_addr;
    screen->port = cfg->backend_port;
    screen->ipv6port = -1;
    screen->passwordCheck = NULL;
    screen->authPasswdData = NULL;

    /* Beta security invariant: no client input reaches the desktop. */
    screen->kbdAddEvent = ignore_keyboard;
    screen->ptrAddEvent = ignore_pointer;
    screen->setXCutText = ignore_cut_text;
    screen->permitFileTransfer = FALSE;

    screen->alwaysShared = TRUE;
    screen->deferUpdateTime = 0;
    screen->progressiveSliceHeight = 0;
    screen->maxRectsPerUpdate = 0;
    screen->setDesktopSizeHook = set_desktop_size;

    runtime.adaptive = adaptive_rfb_create(screen,
                                           runtime.width,
                                           runtime.height);
    if (!runtime.adaptive) {
        LOG_ERROR("Failed to initialize adaptive RFB transport");
        rfbScreenCleanup(screen);
        free(runtime.diff_rects);
        free(runtime.fb);
        signal_ready(backend, 1);
        return NULL;
    }

    /* LibVNCServer's own protocol chatter belongs to debug/trace, not info. */
    rfbLogEnable(vnc_log_enabled(VNC_LOG_DEBUG));
    rfbInitServer(screen);

    if (!rfbIsActive(screen)) {
        LOG_ERROR("LibVNCServer backend failed to start");
        adaptive_rfb_destroy(runtime.adaptive);
        rfbScreenCleanup(screen);
        free(runtime.diff_rects);
        free(runtime.fb);
        signal_ready(backend, 1);
        return NULL;
    }

    LOG_DEBUG("Internal LibVNCServer backend ready at %s:%d framebuffer=%dx%d resize=%s",
              cfg->backend_bind,
              cfg->backend_port,
              runtime.width,
              runtime.height,
              runtime_config_screen_size_mode_name(cfg->screen_size_mode));
    signal_ready(backend, 0);

    uint64_t last_bridge_sequence = 0;
    uint64_t observed_bridge_sequence = 0;
    uint64_t seen_resize_generation = 0;
    int pending_source_frame = 0;
    double next_vnc_publish = 0.0;
    double last_source_arrival = 0.0;
    int backpressure_active = 0;
    int first_real_frame = 1;
    uint64_t backpressure_events = 0;
    uint64_t dropped_source_frames = 0;

    while (rfbIsActive(screen)) {
        pthread_mutex_lock(&backend->mutex);
        int stop = backend->stop;
        pthread_mutex_unlock(&backend->mutex);

        if (stop || shutdown_signal_requested())
            break;

        double now = monotonic_seconds();
        int wait_ms = 20;

        if (pending_source_frame && cfg->vnc_max_fps > 0 &&
            next_vnc_publish > now) {
            double remain_ms = (next_vnc_publish - now) * 1000.0;
            if (remain_ms < (double)wait_ms) {
                wait_ms = (int)remain_ms;
                if (wait_ms < 1)
                    wait_ms = 1;
            }
        }
        else if (pending_source_frame) {
            wait_ms = 0;
        }

        if (wait_ms > 0) {
            uint64_t current_sequence = observed_bridge_sequence;
            int changed = frame_bridge_wait_for_change(backend->frames,
                                                       observed_bridge_sequence,
                                                       wait_ms,
                                                       &current_sequence);

            if (changed < 0) {
                LOG_ERROR("FrameBridge wait failed; continuing with polling");
            }
            else if (changed > 0) {
                double arrived = monotonic_seconds();
                if (vnc_log_enabled(VNC_LOG_TRACE) &&
                    last_source_arrival > 0.0 &&
                    (arrived - last_source_arrival) * 1000.0 >=
                        (double)cfg->capture_stall_ms) {
                    LOG_TRACE("Capture idle gap %.0f ms",
                              (arrived - last_source_arrival) * 1000.0);
                }

                last_source_arrival = arrived;
                observed_bridge_sequence = current_sequence;
                pending_source_frame = 1;
            }
        }

        /* Keep client request processing alive even when capture is quiet. */
        rfbProcessEvents(screen, 0);

        if (seen_resize_generation != runtime.resize_generation) {
            seen_resize_generation = runtime.resize_generation;
            observed_bridge_sequence = last_bridge_sequence;
            pending_source_frame = 1;
            first_real_frame = 1;
            backpressure_active = 0;
            next_vnc_publish = 0.0;
            LOG_DEBUG("Publisher switched to resized framebuffer %dx%d",
                      runtime.width,
                      runtime.height);
        }

        if (stop || shutdown_signal_requested())
            break;

        now = monotonic_seconds();

        if (!pending_source_frame) {
            int ext = 0;
            int back = 0;
            uint32_t ua = 0;
            int idle_ready = transport_idle_ready(backend,
                                                   cfg,
                                                   &ext,
                                                   &back,
                                                   &ua);

            if (adaptive_rfb_try_send_pending(runtime.adaptive,
                                              idle_ready,
                                              now) < 0)
                LOG_ERROR("Pending JPEG send failed");

            (void)adaptive_rfb_try_schedule_repair(runtime.adaptive,
                                                   idle_ready,
                                                   now,
                                                   last_source_arrival);
            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        if (cfg->vnc_max_fps > 0 && next_vnc_publish > now) {
            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        int external_outq = 0;
        int backend_inq = 0;
        uint32_t unacked = 0;
        int pressured = transport_backpressured(backend->stats,
                                                cfg,
                                                &external_outq,
                                                &backend_inq,
                                                &unacked);

        if (cfg->latest_only && backpressure_active && !pressured &&
            !transport_recovered(cfg,
                                 external_outq,
                                 backend_inq,
                                 unacked)) {
            pressured = 1;
        }

        if (cfg->latest_only && pressured) {
            if (!backpressure_active) {
                backpressure_active = 1;
                backpressure_events++;
                LOG_DEBUG("Backpressure: defer publish, keep latest only (outq=%dB backend_inq=%dB unacked=%u)",
                          external_outq,
                          backend_inq,
                          unacked);
            }

            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        if (backpressure_active) {
            backpressure_active = 0;
            LOG_DEBUG("Backpressure cleared at low-water (outq=%dB backend_inq=%dB unacked=%u)",
                      external_outq,
                      backend_inq,
                      unacked);
        }

        uint64_t sequence_before = last_bridge_sequence;
        double diff_started = pipeline_stats_now();
        int published_rect_count = 0;
        uint64_t published_pixels = 0;
        int queued_jpeg = 0;
        double changed_percent = 0.0;
        int initial_update = 0;

        if (first_real_frame) {
            int changed = frame_bridge_consume(backend->frames,
                                               runtime.fb,
                                               &last_bridge_sequence);
            if (changed > 0) {
                rfbMarkRectAsModified(screen,
                                      0,
                                      0,
                                      runtime.width,
                                      runtime.height);
                adaptive_rfb_note_lossless_rect(runtime.adaptive,
                                                0,
                                                0,
                                                runtime.width,
                                                runtime.height);
                published_rect_count = 1;
                published_pixels =
                    (uint64_t)runtime.width * (uint64_t)runtime.height;
                changed_percent = 100.0;
                initial_update = 1;
                first_real_frame = 0;
                LOG_DEBUG("Initial lossless framebuffer submitted: seq=%llu %dx%d",
                          (unsigned long long)last_bridge_sequence,
                          runtime.width,
                          runtime.height);
            }
        }
        else if (cfg->diff_detect) {
            int rect_count = frame_diff_consume(backend->frames,
                                                runtime.fb,
                                                &last_bridge_sequence,
                                                cfg->diff_tile_size,
                                                runtime.diff_rects,
                                                runtime.max_diff_rects);

            if (rect_count < 0) {
                LOG_ERROR("Frame diff failed; falling back to full-frame copy");
                int changed = frame_bridge_consume(backend->frames,
                                                   runtime.fb,
                                                   &last_bridge_sequence);
                if (changed > 0) {
                    published_rect_count = 1;
                    published_pixels =
                        (uint64_t)runtime.width * (uint64_t)runtime.height;
                    changed_percent = 100.0;
                    queue_full_update(runtime.adaptive,
                                      screen,
                                      runtime.width,
                                      runtime.height,
                                      last_bridge_sequence,
                                      changed_percent,
                                      &queued_jpeg);
                }
            }
            else {
                published_rect_count = rect_count;

                for (int i = 0; i < rect_count; i++) {
                    FrameDiffRect *rect = &runtime.diff_rects[i];
                    published_pixels +=
                        (uint64_t)(rect->x2 - rect->x1) *
                        (uint64_t)(rect->y2 - rect->y1);
                }

                uint64_t total_pixels =
                    (uint64_t)runtime.width * (uint64_t)runtime.height;
                changed_percent = total_pixels > 0
                    ? (double)published_pixels * 100.0 / (double)total_pixels
                    : 0.0;

                if (rect_count > 0 &&
                    adaptive_rfb_should_queue_jpeg(runtime.adaptive,
                                                   changed_percent)) {
                    for (int i = 0; i < rect_count; i++) {
                        FrameDiffRect *rect = &runtime.diff_rects[i];
                        adaptive_rfb_queue_jpeg_rect(runtime.adaptive,
                                                     rect->x1,
                                                     rect->y1,
                                                     rect->x2,
                                                     rect->y2,
                                                     last_bridge_sequence,
                                                     changed_percent);
                    }
                    queued_jpeg = 1;
                }
                else {
                    for (int i = 0; i < rect_count; i++) {
                        FrameDiffRect *rect = &runtime.diff_rects[i];
                        rfbMarkRectAsModified(screen,
                                              rect->x1,
                                              rect->y1,
                                              rect->x2,
                                              rect->y2);
                        adaptive_rfb_note_lossless_rect(runtime.adaptive,
                                                        rect->x1,
                                                        rect->y1,
                                                        rect->x2,
                                                        rect->y2);
                    }
                }
            }
        }
        else {
            int changed = frame_bridge_consume(backend->frames,
                                               runtime.fb,
                                               &last_bridge_sequence);
            if (changed > 0) {
                published_rect_count = 1;
                published_pixels =
                    (uint64_t)runtime.width * (uint64_t)runtime.height;
                changed_percent = 100.0;
                queue_full_update(runtime.adaptive,
                                  screen,
                                  runtime.width,
                                  runtime.height,
                                  last_bridge_sequence,
                                  changed_percent,
                                  &queued_jpeg);
            }
        }

        observed_bridge_sequence = last_bridge_sequence;
        pending_source_frame = 0;

        next_vnc_publish = cfg->vnc_max_fps > 0
            ? now + 1.0 / (double)cfg->vnc_max_fps
            : 0.0;

        double rfb_started = pipeline_stats_now();
        rfbProcessEvents(screen, 0);

        if (seen_resize_generation != runtime.resize_generation) {
            seen_resize_generation = runtime.resize_generation;
            observed_bridge_sequence = last_bridge_sequence;
            pending_source_frame = 1;
            first_real_frame = 1;
            next_vnc_publish = 0.0;
        }

        int adaptive_ready =
            !pressured &&
            transport_recovered(cfg,
                                external_outq,
                                backend_inq,
                                unacked);

        int jpeg_sent = adaptive_rfb_try_send_pending(runtime.adaptive,
                                                      adaptive_ready,
                                                      monotonic_seconds());

        double rfb_ms = (pipeline_stats_now() - rfb_started) * 1000.0;
        uint64_t sequence_delta = last_bridge_sequence >= sequence_before
            ? last_bridge_sequence - sequence_before
            : 0;

        if (sequence_delta > 1)
            dropped_source_frames += sequence_delta - 1;

        double diff_ms = (pipeline_stats_now() - diff_started) * 1000.0;

        pipeline_stats_publish(backend->stats,
                               sequence_delta,
                               published_rect_count,
                               published_pixels,
                               diff_ms,
                               rfb_ms);

        pipeline_stats_trace_publish(backend->stats,
                                     sequence_delta,
                                     published_rect_count,
                                     published_pixels,
                                     diff_ms,
                                     rfb_ms);

        if (cfg->latency_trace) {
            const char *mode = initial_update
                ? "initial-zrle"
                : queued_jpeg
                    ? (jpeg_sent > 0 ? "jpeg21" : "jpeg21-pending")
                    : "zrle";

            LOG_TRACE("LATENCY seq=%llu source_delta=%llu dropped=%llu mode=%s changed=%.1f%% diff=%.3fms rfb=%.3fms outq=%dB backend_inq=%dB unacked=%u",
                      (unsigned long long)last_bridge_sequence,
                      (unsigned long long)sequence_delta,
                      (unsigned long long)(sequence_delta > 1 ? sequence_delta - 1 : 0),
                      mode,
                      changed_percent,
                      diff_ms,
                      rfb_ms,
                      external_outq,
                      backend_inq,
                      unacked);
        }

        pipeline_stats_maybe_report(backend->stats);
    }

    LOG_DEBUG("Publisher summary: backpressure-events=%llu dropped-source=%llu",
              (unsigned long long)backpressure_events,
              (unsigned long long)dropped_source_frames);

    adaptive_rfb_print_summary(runtime.adaptive);

    rfbShutdownServer(screen, TRUE);
    adaptive_rfb_destroy(runtime.adaptive);
    rfbScreenCleanup(screen);
    free(runtime.diff_rects);
    free(runtime.fb);
    return NULL;
}

int
rfb_backend_start(RfbBackend *backend,
                  RuntimeConfig *cfg,
                  FrameBridge *frames,
                  PipelineStats *stats,
                  RealMonitor *real_monitor,
                  MonitorLayoutCache *layout_cache)
{
    if (!backend || !cfg || !frames || !real_monitor || !layout_cache)
        return -1;

    memset(backend, 0, sizeof(*backend));
    backend->cfg = cfg;
    backend->frames = frames;
    backend->stats = stats;
    backend->real_monitor = real_monitor;
    backend->layout_cache = layout_cache;

    if (pthread_mutex_init(&backend->mutex, NULL) != 0)
        return -1;

    if (pthread_cond_init(&backend->cond, NULL) != 0) {
        pthread_mutex_destroy(&backend->mutex);
        return -1;
    }

    if (pthread_create(&backend->thread,
                       NULL,
                       backend_thread,
                       backend) != 0) {
        pthread_cond_destroy(&backend->cond);
        pthread_mutex_destroy(&backend->mutex);
        return -1;
    }

    pthread_mutex_lock(&backend->mutex);
    while (!backend->ready)
        pthread_cond_wait(&backend->cond, &backend->mutex);

    int failed = backend->failed;
    pthread_mutex_unlock(&backend->mutex);

    if (failed) {
        pthread_join(backend->thread, NULL);
        pthread_cond_destroy(&backend->cond);
        pthread_mutex_destroy(&backend->mutex);
        memset(backend, 0, sizeof(*backend));
        return -1;
    }

    return 0;
}

void
rfb_backend_stop(RfbBackend *backend)
{
    if (!backend || !backend->ready)
        return;

    pthread_mutex_lock(&backend->mutex);
    backend->stop = 1;
    pthread_mutex_unlock(&backend->mutex);

    frame_bridge_wake_all(backend->frames);
    pthread_join(backend->thread, NULL);
    pthread_cond_destroy(&backend->cond);
    pthread_mutex_destroy(&backend->mutex);
    memset(backend, 0, sizeof(*backend));
}
