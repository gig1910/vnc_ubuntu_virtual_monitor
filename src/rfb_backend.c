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
                  const RuntimeConfig *cfg,
                  uint64_t sequence,
                  double changed_percent,
                  int *queued_jpeg)
{
    if (adaptive_rfb_should_queue_jpeg(adaptive, changed_percent)) {
        adaptive_rfb_queue_jpeg_rect(adaptive,
                                     0,
                                     0,
                                     cfg->width,
                                     cfg->height,
                                     sequence,
                                     changed_percent);
        *queued_jpeg = 1;
    }
    else {
        rfbMarkRectAsModified(screen, 0, 0, cfg->width, cfg->height);
        adaptive_rfb_note_lossless_rect(adaptive,
                                        0,
                                        0,
                                        cfg->width,
                                        cfg->height);
    }
}

static void *
backend_thread(void *arg)
{
    RfbBackend *backend = arg;
    const RuntimeConfig *cfg = backend->cfg;

    uint8_t *fb = calloc((size_t)cfg->width * (size_t)cfg->height, 4);
    if (!fb) {
        signal_ready(backend, 1);
        return NULL;
    }

    int argc = 1;
    char arg0[] = "vnc-monitor-backend";
    char *argv[] = {arg0, NULL};

    rfbScreenInfoPtr screen = rfbGetScreen(&argc,
                                           argv,
                                           cfg->width,
                                           cfg->height,
                                           8,
                                           3,
                                           4);
    if (!screen) {
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    screen->desktopName = "VNC Monitor";
    screen->frameBuffer = (char *)fb;

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
        free(fb);
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

    AdaptiveRfbState *adaptive = adaptive_rfb_create(screen,
                                                     cfg->width,
                                                     cfg->height);
    if (!adaptive) {
        LOG_ERROR("Failed to initialize adaptive RFB transport");
        rfbScreenCleanup(screen);
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    /* LibVNCServer's own protocol chatter belongs to debug/trace, not info. */
    rfbLogEnable(vnc_log_enabled(VNC_LOG_DEBUG));
    rfbInitServer(screen);

    if (!rfbIsActive(screen)) {
        LOG_ERROR("LibVNCServer backend failed to start");
        adaptive_rfb_destroy(adaptive);
        rfbScreenCleanup(screen);
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    LOG_DEBUG("Internal LibVNCServer backend ready at %s:%d",
              cfg->backend_bind,
              cfg->backend_port);
    signal_ready(backend, 0);

    uint64_t last_bridge_sequence = 0;
    uint64_t observed_bridge_sequence = 0;
    int pending_source_frame = 0;
    double next_vnc_publish = 0.0;
    double last_source_arrival = 0.0;
    int backpressure_active = 0;
    int first_real_frame = 1;
    uint64_t backpressure_events = 0;
    uint64_t dropped_source_frames = 0;

    size_t max_diff_rects =
        (size_t)((cfg->width + cfg->diff_tile_size - 1) / cfg->diff_tile_size) *
        (size_t)((cfg->height + cfg->diff_tile_size - 1) / cfg->diff_tile_size);

    FrameDiffRect *diff_rects = calloc(max_diff_rects, sizeof(*diff_rects));
    if (!diff_rects) {
        rfbShutdownServer(screen, TRUE);
        adaptive_rfb_destroy(adaptive);
        rfbScreenCleanup(screen);
        free(fb);
        return NULL;
    }

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

            if (adaptive_rfb_try_send_pending(adaptive, idle_ready, now) < 0)
                LOG_ERROR("Pending JPEG send failed");

            (void)adaptive_rfb_try_schedule_repair(adaptive,
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
                                               fb,
                                               &last_bridge_sequence);
            if (changed > 0) {
                rfbMarkRectAsModified(screen, 0, 0, cfg->width, cfg->height);
                adaptive_rfb_note_lossless_rect(adaptive,
                                                0,
                                                0,
                                                cfg->width,
                                                cfg->height);
                published_rect_count = 1;
                published_pixels = (uint64_t)cfg->width * (uint64_t)cfg->height;
                changed_percent = 100.0;
                initial_update = 1;
                first_real_frame = 0;
                LOG_DEBUG("Initial lossless framebuffer submitted: seq=%llu %dx%d",
                          (unsigned long long)last_bridge_sequence,
                          cfg->width,
                          cfg->height);
            }
        }
        else if (cfg->diff_detect) {
            int rect_count = frame_diff_consume(backend->frames,
                                                fb,
                                                &last_bridge_sequence,
                                                cfg->diff_tile_size,
                                                diff_rects,
                                                max_diff_rects);

            if (rect_count < 0) {
                LOG_ERROR("Frame diff failed; falling back to full-frame copy");
                int changed = frame_bridge_consume(backend->frames,
                                                   fb,
                                                   &last_bridge_sequence);
                if (changed > 0) {
                    published_rect_count = 1;
                    published_pixels = (uint64_t)cfg->width * (uint64_t)cfg->height;
                    changed_percent = 100.0;
                    queue_full_update(adaptive,
                                      screen,
                                      cfg,
                                      last_bridge_sequence,
                                      changed_percent,
                                      &queued_jpeg);
                }
            }
            else {
                published_rect_count = rect_count;

                for (int i = 0; i < rect_count; i++) {
                    FrameDiffRect *rect = &diff_rects[i];
                    published_pixels +=
                        (uint64_t)(rect->x2 - rect->x1) *
                        (uint64_t)(rect->y2 - rect->y1);
                }

                uint64_t total_pixels =
                    (uint64_t)cfg->width * (uint64_t)cfg->height;
                changed_percent = total_pixels > 0
                    ? (double)published_pixels * 100.0 / (double)total_pixels
                    : 0.0;

                if (rect_count > 0 &&
                    adaptive_rfb_should_queue_jpeg(adaptive,
                                                   changed_percent)) {
                    for (int i = 0; i < rect_count; i++) {
                        FrameDiffRect *rect = &diff_rects[i];
                        adaptive_rfb_queue_jpeg_rect(adaptive,
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
                        FrameDiffRect *rect = &diff_rects[i];
                        rfbMarkRectAsModified(screen,
                                              rect->x1,
                                              rect->y1,
                                              rect->x2,
                                              rect->y2);
                        adaptive_rfb_note_lossless_rect(adaptive,
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
                                               fb,
                                               &last_bridge_sequence);
            if (changed > 0) {
                published_rect_count = 1;
                published_pixels = (uint64_t)cfg->width * (uint64_t)cfg->height;
                changed_percent = 100.0;
                queue_full_update(adaptive,
                                  screen,
                                  cfg,
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

        int adaptive_ready =
            !pressured &&
            transport_recovered(cfg,
                                external_outq,
                                backend_inq,
                                unacked);

        int jpeg_sent = adaptive_rfb_try_send_pending(adaptive,
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

    adaptive_rfb_print_summary(adaptive);

    rfbShutdownServer(screen, TRUE);
    adaptive_rfb_destroy(adaptive);
    rfbScreenCleanup(screen);
    free(diff_rects);
    free(fb);
    return NULL;
}

int
rfb_backend_start(RfbBackend *backend,
                  const RuntimeConfig *cfg,
                  FrameBridge *frames,
                  PipelineStats *stats)
{
    if (!backend || !cfg || !frames)
        return -1;

    memset(backend, 0, sizeof(*backend));
    backend->cfg = cfg;
    backend->frames = frames;
    backend->stats = stats;

    if (pthread_mutex_init(&backend->mutex, NULL) != 0 ||
        pthread_cond_init(&backend->cond, NULL) != 0)
        return -1;

    if (pthread_create(&backend->thread,
                       NULL,
                       backend_thread,
                       backend) != 0)
        return -1;

    pthread_mutex_lock(&backend->mutex);
    while (!backend->ready)
        pthread_cond_wait(&backend->cond, &backend->mutex);

    int failed = backend->failed;
    pthread_mutex_unlock(&backend->mutex);
    return failed ? -1 : 0;
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
}
