#include "rfb_backend.h"
#include "test_pattern.h"
#include "benchmark.h"
#include "frame_diff.h"
#include "adaptive_rfb.h"
#include "shutdown_signal.h"

#include <rfb/rfb.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
ignore_keyboard(
    rfbBool down,
    rfbKeySym keySym,
    rfbClientPtr cl)
{
    (void)down;
    (void)keySym;
    (void)cl;
}

static void
ignore_pointer(
    int buttonMask,
    int x,
    int y,
    rfbClientPtr cl)
{
    (void)buttonMask;
    (void)x;
    (void)y;
    (void)cl;
}

static void
ignore_cut_text(
    char *text,
    int len,
    rfbClientPtr cl)
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

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec / 1000000000.0;
}

static void
sleep_until(double target)
{
    for (;;) {
        double now = monotonic_seconds();
        double remain = target - now;

        if (remain <= 0.0)
            return;

        struct timespec ts;
        ts.tv_sec = (time_t)remain;
        ts.tv_nsec =
            (long)(
                (remain - (double)ts.tv_sec) *
                1000000000.0
            );

        nanosleep(&ts, NULL);
    }
}

static void
signal_ready(
    RfbBackend *backend,
    int failed)
{
    pthread_mutex_lock(&backend->mutex);
    backend->failed = failed;
    backend->ready = 1;
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);
}

/*
 * The relay thread updates these queue counters from the actual sockets.
 * The publisher uses them only as a coarse backpressure signal. The point is
 * not to estimate bandwidth perfectly; it is to stop manufacturing stale RFB
 * work while the already-produced update stream is visibly queued.
 */
static int
transport_backpressured(
    PipelineStats *stats,
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

    return
        (ext >= cfg->external_send_buffer) ||
        (back >= cfg->backend_receive_buffer) ||
        (ua >= 24);
}

static int
transport_recovered(
    const RuntimeConfig *cfg,
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

    return
        external_outq <= external_low &&
        backend_inq <= backend_low &&
        unacked <= 8;
}

static int
transport_idle_ready(
    RfbBackend *backend,
    const RuntimeConfig *cfg,
    int *external_outq,
    int *backend_inq,
    uint32_t *unacked)
{
    int ext = 0;
    int back = 0;
    uint32_t ua = 0;

    int pressured =
        transport_backpressured(
            backend->stats,
            cfg,
            &ext,
            &back,
            &ua
        );

    if (external_outq)
        *external_outq = ext;
    if (backend_inq)
        *backend_inq = back;
    if (unacked)
        *unacked = ua;

    return
        !pressured &&
        transport_recovered(
            cfg,
            ext,
            back,
            ua
        );
}

static void *
backend_thread(void *arg)
{
    RfbBackend *backend = arg;
    const RuntimeConfig *cfg = backend->cfg;

    uint8_t *fb =
        calloc(
            (size_t)cfg->width *
            cfg->height,
            4
        );

    if (!fb) {
        signal_ready(backend, 1);
        return NULL;
    }

    int argc = 1;
    char arg0[] = "vnc-monitor-backend";
    char *argv[] = {arg0, NULL};

    rfbScreenInfoPtr screen =
        rfbGetScreen(
            &argc,
            argv,
            cfg->width,
            cfg->height,
            8,
            3,
            4
        );

    if (!screen) {
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    screen->desktopName =
        "VNC Monitor RA2r Test";

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

    if (
        inet_pton(
            AF_INET,
            cfg->backend_bind,
            &backend_addr
        ) != 1
    ) {
        fprintf(
            stderr,
            "Invalid backend IPv4 bind address: %s\n",
            cfg->backend_bind
        );

        rfbScreenCleanup(screen);
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    screen->listenInterface =
        backend_addr.s_addr;

    screen->port = cfg->backend_port;
    screen->ipv6port = -1;

    screen->passwordCheck = NULL;
    screen->authPasswdData = NULL;

    if (cfg->view_only) {
        screen->kbdAddEvent = ignore_keyboard;
        screen->ptrAddEvent = ignore_pointer;
    }

    if (!cfg->enable_clipboard)
        screen->setXCutText = ignore_cut_text;
    screen->permitFileTransfer = cfg->enable_file_transfer ? TRUE : FALSE;

    screen->alwaysShared = TRUE;
    screen->deferUpdateTime = 0;
    screen->progressiveSliceHeight = 0;
    screen->maxRectsPerUpdate = 0;

    AdaptiveRfbState *adaptive =
        adaptive_rfb_create(
            screen,
            cfg->width,
            cfg->height
        );

    if (!adaptive) {
        fprintf(stderr, "Failed to initialize adaptive RFB transport\n");
        rfbScreenCleanup(screen);
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    if (cfg->source_mode == FRAME_SOURCE_TEST) {
        test_pattern_reset();
        test_pattern_init(fb, cfg);
    }

    rfbInitServer(screen);

    if (!rfbIsActive(screen)) {
        fprintf(stderr, "LibVNCServer backend failed to start\n");
        adaptive_rfb_destroy(adaptive);
        rfbScreenCleanup(screen);
        free(fb);
        signal_ready(backend, 1);
        return NULL;
    }

    printf(
        "Internal LibVNCServer backend: %s:%d\n",
        cfg->backend_bind,
        cfg->backend_port
    );

    signal_ready(backend, 0);

    const double frame_interval =
        1.0 / (double)cfg->max_fps;

    double start = monotonic_seconds();
    double next_frame = start;

    unsigned int frame = 0;
    uint64_t last_bridge_sequence = 0;
    uint64_t observed_bridge_sequence = 0;
    int pending_source_frame = 0;
    double next_vnc_publish = 0.0;

    /*
     * 0.0.25 state model:
     * - ordinary ZRLE updates are lossless and require no repair after source
     *   drops or backpressure; newest-vs-reference diff is sufficient;
     * - a JPEG21 update deliberately makes the client framebuffer approximate,
     *   so adaptive_rfb keeps a separate repair bitmap;
     * - only new heavy deltas join a pending JPEG. Small cursor/UI deltas keep
     *   their lossless priority while that JPEG is waiting for an RFB request;
     * - idle repair sends dispersed 32x32 lossless tiles at low priority.
     */
    int force_keyframe = cfg->keyframe ? 1 : 0;
    const char *keyframe_reason = "initial";
    double last_source_arrival = 0.0;
    int backpressure_active = 0;
    uint64_t backpressure_events = 0;
    uint64_t keyframes = 0;
    uint64_t dropped_source_frames = 0;

    size_t max_diff_rects =
        (size_t)(
            (cfg->width + cfg->diff_tile_size - 1) /
            cfg->diff_tile_size
        ) *
        (size_t)(
            (cfg->height + cfg->diff_tile_size - 1) /
            cfg->diff_tile_size
        );

    FrameDiffRect *diff_rects =
        calloc(
            max_diff_rects,
            sizeof(*diff_rects)
        );

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

        if (cfg->source_mode == FRAME_SOURCE_TEST) {
            double now = monotonic_seconds();

            if (now < next_frame) {
                sleep_until(next_frame);
                now = monotonic_seconds();
            }

            if (now - next_frame > frame_interval * 2.0)
                next_frame = now;

            next_frame += frame_interval;

            TestPatternFrame pattern;
            double render_started = benchmark_now();

            test_pattern_render(
                fb,
                cfg,
                now - start,
                frame++,
                &pattern
            );

            double render_ms =
                (benchmark_now() - render_started) * 1000.0;

            uint64_t dirty_pixels = 0;

            if (
                pattern.dirty_x2 > pattern.dirty_x1 &&
                pattern.dirty_y2 > pattern.dirty_y1
            ) {
                dirty_pixels =
                    (uint64_t)(pattern.dirty_x2 - pattern.dirty_x1) *
                    (uint64_t)(pattern.dirty_y2 - pattern.dirty_y1);

                rfbMarkRectAsModified(
                    screen,
                    pattern.dirty_x1,
                    pattern.dirty_y1,
                    pattern.dirty_x2,
                    pattern.dirty_y2
                );
            }

            benchmark_set_stage(pattern.stage, pattern.square_size);

            double rfb_started = benchmark_now();
            rfbProcessEvents(screen, 0);
            double rfb_ms =
                (benchmark_now() - rfb_started) * 1000.0;

            double lateness_ms =
                now > next_frame
                    ? (now - next_frame) * 1000.0
                    : 0.0;

            benchmark_record_frame(
                dirty_pixels,
                render_ms,
                rfb_ms,
                lateness_ms
            );

            benchmark_maybe_report();
            continue;
        }

        double now = monotonic_seconds();
        int wait_ms = 20;

        if (
            pending_source_frame &&
            cfg->vnc_max_fps > 0 &&
            next_vnc_publish > now
        ) {
            double remain_ms =
                (next_vnc_publish - now) * 1000.0;

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
            uint64_t current_sequence =
                observed_bridge_sequence;

            int changed =
                frame_bridge_wait_for_change(
                    backend->frames,
                    observed_bridge_sequence,
                    wait_ms,
                    &current_sequence
                );

            if (changed < 0) {
                fprintf(
                    stderr,
                    "FrameBridge wait failed; continuing with polling\n"
                );
            }
            else if (changed > 0) {
                double arrived = monotonic_seconds();

                if (
                    last_source_arrival > 0.0 &&
                    (arrived - last_source_arrival) * 1000.0 >=
                        (double)cfg->capture_stall_ms &&
                    cfg->verbose
                ) {
                    fprintf(
                        stderr,
                        "[CAPTURE] idle-gap=%.0fms; no repair/keyframe scheduled\n",
                        (arrived - last_source_arrival) * 1000.0
                    );
                }

                last_source_arrival = arrived;
                observed_bridge_sequence = current_sequence;
                pending_source_frame = 1;
            }
        }

        /* Keep RFB request processing alive even when capture is quiet. */
        rfbProcessEvents(screen, 0);

        if (stop || shutdown_signal_requested())
            break;

        now = monotonic_seconds();

        if (!pending_source_frame) {
            int idle_external_outq = 0;
            int idle_backend_inq = 0;
            uint32_t idle_unacked = 0;

            int idle_ready =
                transport_idle_ready(
                    backend,
                    cfg,
                    &idle_external_outq,
                    &idle_backend_inq,
                    &idle_unacked
                );

            int jpeg_rc =
                adaptive_rfb_try_send_pending(
                    adaptive,
                    idle_ready,
                    now
                );

            if (jpeg_rc < 0) {
                fprintf(
                    stderr,
                    "[ADAPT] pending JPEG send failed\n"
                );
            }

            adaptive_rfb_try_schedule_repair(
                adaptive,
                idle_ready,
                now,
                last_source_arrival
            );

            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        if (
            cfg->vnc_max_fps > 0 &&
            next_vnc_publish > now
        ) {
            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        int external_outq = 0;
        int backend_inq = 0;
        uint32_t unacked = 0;
        int pressured =
            transport_backpressured(
                backend->stats,
                cfg,
                &external_outq,
                &backend_inq,
                &unacked
            );

        if (
            cfg->latest_only &&
            backpressure_active &&
            !pressured &&
            !transport_recovered(
                cfg,
                external_outq,
                backend_inq,
                unacked
            )
        ) {
            pressured = 1;
        }

        if (cfg->latest_only && pressured) {
            if (!backpressure_active) {
                backpressure_active = 1;
                backpressure_events++;

                if (cfg->verbose) {
                    fprintf(
                        stderr,
                        "[GOV] backpressure: defer publish, keep latest only "
                        "outq=%dB backend_inq=%dB unacked=%u\n",
                        external_outq,
                        backend_inq,
                        unacked
                    );
                }
            }

            pipeline_stats_maybe_report(backend->stats);
            continue;
        }

        if (backpressure_active) {
            backpressure_active = 0;

            if (cfg->verbose) {
                fprintf(
                    stderr,
                    "[GOV] backpressure cleared at low-water: "
                    "outq=%dB backend_inq=%dB unacked=%u; "
                    "publishing newest state\n",
                    external_outq,
                    backend_inq,
                    unacked
                );
            }
        }

        uint64_t sequence_before =
            last_bridge_sequence;

        double diff_started =
            pipeline_stats_now();

        int published_rect_count = 0;
        uint64_t published_pixels = 0;
        uint64_t dirty_pixels = 0;
        int emitted_keyframe = 0;
        int queued_jpeg = 0;
        double changed_percent = 0.0;

        if (force_keyframe && cfg->keyframe) {
            int changed =
                frame_bridge_consume(
                    backend->frames,
                    fb,
                    &last_bridge_sequence
                );

            if (changed > 0) {
                rfbMarkRectAsModified(
                    screen,
                    0,
                    0,
                    cfg->width,
                    cfg->height
                );

                adaptive_rfb_note_lossless_rect(
                    adaptive,
                    0,
                    0,
                    cfg->width,
                    cfg->height
                );

                published_rect_count = 1;
                published_pixels =
                    (uint64_t)cfg->width *
                    (uint64_t)cfg->height;
                dirty_pixels = published_pixels;
                emitted_keyframe = 1;
                keyframes++;

                fprintf(
                    stderr,
                    "[KF] seq=%llu reason=%s full=%dx%d\n",
                    (unsigned long long)last_bridge_sequence,
                    keyframe_reason ? keyframe_reason : "initial",
                    cfg->width,
                    cfg->height
                );
            }

            force_keyframe = 0;
            keyframe_reason = NULL;
        }
        else if (cfg->diff_detect) {
            int rect_count =
                frame_diff_consume(
                    backend->frames,
                    fb,
                    &last_bridge_sequence,
                    cfg->diff_tile_size,
                    diff_rects,
                    max_diff_rects
                );

            if (rect_count < 0) {
                fprintf(
                    stderr,
                    "Frame diff detection failed; falling back to full-frame copy\n"
                );

                int changed =
                    frame_bridge_consume(
                        backend->frames,
                        fb,
                        &last_bridge_sequence
                    );

                if (changed > 0) {
                    published_rect_count = 1;
                    published_pixels =
                        (uint64_t)cfg->width *
                        (uint64_t)cfg->height;
                    dirty_pixels = published_pixels;
                    changed_percent = 100.0;

                    if (
                        adaptive_rfb_should_queue_jpeg(
                            adaptive,
                            changed_percent
                        )
                    ) {
                        adaptive_rfb_queue_jpeg_rect(
                            adaptive,
                            0,
                            0,
                            cfg->width,
                            cfg->height,
                            last_bridge_sequence,
                            changed_percent
                        );
                        queued_jpeg = 1;
                    }
                    else {
                        rfbMarkRectAsModified(
                            screen,
                            0,
                            0,
                            cfg->width,
                            cfg->height
                        );
                        adaptive_rfb_note_lossless_rect(
                            adaptive,
                            0,
                            0,
                            cfg->width,
                            cfg->height
                        );
                    }
                }
            }
            else {
                published_rect_count = rect_count;

                for (int i = 0; i < rect_count; i++) {
                    FrameDiffRect *rect = &diff_rects[i];

                    uint64_t rect_pixels =
                        (uint64_t)(rect->x2 - rect->x1) *
                        (uint64_t)(rect->y2 - rect->y1);

                    dirty_pixels += rect_pixels;
                    published_pixels += rect_pixels;
                }

                uint64_t total_pixels =
                    (uint64_t)cfg->width *
                    (uint64_t)cfg->height;

                changed_percent =
                    total_pixels > 0
                        ? (double)published_pixels * 100.0 /
                            (double)total_pixels
                        : 0.0;

                int heavy_delta =
                    changed_percent >=
                        ADAPTIVE_RFB_JPEG_THRESHOLD_PERCENT;

                if (
                    rect_count > 0 &&
                    heavy_delta &&
                    adaptive_rfb_should_queue_jpeg(
                        adaptive,
                        changed_percent
                    )
                ) {
                    for (int i = 0; i < rect_count; i++) {
                        FrameDiffRect *rect = &diff_rects[i];

                        adaptive_rfb_queue_jpeg_rect(
                            adaptive,
                            rect->x1,
                            rect->y1,
                            rect->x2,
                            rect->y2,
                            last_bridge_sequence,
                            changed_percent
                        );
                    }

                    queued_jpeg = 1;
                }
                else {
                    for (int i = 0; i < rect_count; i++) {
                        FrameDiffRect *rect = &diff_rects[i];

                        rfbMarkRectAsModified(
                            screen,
                            rect->x1,
                            rect->y1,
                            rect->x2,
                            rect->y2
                        );

                        adaptive_rfb_note_lossless_rect(
                            adaptive,
                            rect->x1,
                            rect->y1,
                            rect->x2,
                            rect->y2
                        );
                    }
                }
            }
        }
        else {
            int changed =
                frame_bridge_consume(
                    backend->frames,
                    fb,
                    &last_bridge_sequence
                );

            if (changed > 0) {
                published_rect_count = 1;
                published_pixels =
                    (uint64_t)cfg->width *
                    (uint64_t)cfg->height;
                dirty_pixels = published_pixels;
                changed_percent = 100.0;

                if (
                    adaptive_rfb_should_queue_jpeg(
                        adaptive,
                        changed_percent
                    )
                ) {
                    adaptive_rfb_queue_jpeg_rect(
                        adaptive,
                        0,
                        0,
                        cfg->width,
                        cfg->height,
                        last_bridge_sequence,
                        changed_percent
                    );
                    queued_jpeg = 1;
                }
                else {
                    rfbMarkRectAsModified(
                        screen,
                        0,
                        0,
                        cfg->width,
                        cfg->height
                    );
                    adaptive_rfb_note_lossless_rect(
                        adaptive,
                        0,
                        0,
                        cfg->width,
                        cfg->height
                    );
                }
            }
        }

        observed_bridge_sequence =
            last_bridge_sequence;
        pending_source_frame = 0;

        if (cfg->vnc_max_fps > 0) {
            next_vnc_publish =
                now + 1.0 / (double)cfg->vnc_max_fps;
        }
        else {
            next_vnc_publish = 0.0;
        }

        benchmark_set_stage("mutter", 0);

        double rfb_started = benchmark_now();
        rfbProcessEvents(screen, 0);

        int adaptive_ready =
            !pressured &&
            transport_recovered(
                cfg,
                external_outq,
                backend_inq,
                unacked
            );

        int jpeg_sent =
            adaptive_rfb_try_send_pending(
                adaptive,
                adaptive_ready,
                monotonic_seconds()
            );

        double rfb_ms =
            (benchmark_now() - rfb_started) * 1000.0;

        uint64_t sequence_delta =
            last_bridge_sequence >= sequence_before
                ? last_bridge_sequence - sequence_before
                : 0;

        if (sequence_delta > 1)
            dropped_source_frames += sequence_delta - 1;

        double diff_ms =
            (pipeline_stats_now() - diff_started) * 1000.0;

        pipeline_stats_publish(
            backend->stats,
            sequence_delta,
            published_rect_count,
            published_pixels,
            diff_ms,
            rfb_ms
        );

        pipeline_stats_trace_publish(
            backend->stats,
            sequence_delta,
            published_rect_count,
            published_pixels,
            diff_ms,
            rfb_ms
        );

        if (cfg->latency_trace) {
            const char *mode =
                emitted_keyframe
                    ? "keyframe-zrle"
                    : queued_jpeg
                        ? (jpeg_sent > 0 ? "jpeg21" : "jpeg21-pending")
                        : "zrle";

            fprintf(
                stderr,
                "[LATENCY] seq=%llu source_delta=%llu dropped=%llu "
                "mode=%s changed=%.1f%% diff=%.3fms rfb=%.3fms "
                "outq=%dB backend_inq=%dB unacked=%u\n",
                (unsigned long long)last_bridge_sequence,
                (unsigned long long)sequence_delta,
                (unsigned long long)(sequence_delta > 1 ? sequence_delta - 1 : 0),
                mode,
                changed_percent,
                diff_ms,
                rfb_ms,
                external_outq,
                backend_inq,
                unacked
            );
        }

        pipeline_stats_maybe_report(backend->stats);

        benchmark_record_frame(
            dirty_pixels,
            diff_ms,
            rfb_ms,
            0.0
        );

        benchmark_maybe_report();
    }

    fprintf(
        stderr,
        "[GOV][SUMMARY] keyframes=%llu backpressure-events=%llu "
        "dropped-source=%llu\n",
        (unsigned long long)keyframes,
        (unsigned long long)backpressure_events,
        (unsigned long long)dropped_source_frames
    );

    adaptive_rfb_print_summary(adaptive);

    rfbShutdownServer(screen, TRUE);
    adaptive_rfb_destroy(adaptive);
    rfbScreenCleanup(screen);
    free(diff_rects);
    free(fb);

    return NULL;
}

int
rfb_backend_start(
    RfbBackend *backend,
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

    if (
        pthread_mutex_init(
            &backend->mutex,
            NULL
        ) != 0 ||
        pthread_cond_init(
            &backend->cond,
            NULL
        ) != 0
    )
        return -1;

    if (
        pthread_create(
            &backend->thread,
            NULL,
            backend_thread,
            backend
        ) != 0
    )
        return -1;

    pthread_mutex_lock(&backend->mutex);

    while (!backend->ready)
        pthread_cond_wait(
            &backend->cond,
            &backend->mutex
        );

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
