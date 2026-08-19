#include "gstreamer_capture.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return
        (uint64_t)ts.tv_sec * 1000000000ULL +
        (uint64_t)ts.tv_nsec;
}

static double
clocktime_ms(GstClockTime value)
{
    if (!GST_CLOCK_TIME_IS_VALID(value))
        return -1.0;

    return (double)value / (double)GST_MSECOND;
}

static void
record_sample_interval(
    GstreamerCapture *capture,
    GstBuffer *buffer)
{
    uint64_t now_ns = monotonic_now_ns();
    uint64_t sequence = ++capture->sample_sequence;
    double gap_ms = 0.0;

    if (capture->last_sample_ns != 0) {
        gap_ms =
            (double)(now_ns - capture->last_sample_ns) /
            1000000.0;

        capture->interval_count++;

        if (gap_ms < 250.0)
            capture->interval_histogram[0]++;
        else if (gap_ms < 500.0)
            capture->interval_histogram[1]++;
        else if (gap_ms < 1000.0)
            capture->interval_histogram[2]++;
        else if (gap_ms < 2000.0)
            capture->interval_histogram[3]++;
        else if (gap_ms < 5000.0)
            capture->interval_histogram[4]++;
        else
            capture->interval_histogram[5]++;

        if (gap_ms > capture->max_interval_ms)
            capture->max_interval_ms = gap_ms;

        if (
            capture->capture_stall_ms > 0 &&
            gap_ms >= (double)capture->capture_stall_ms
        ) {
            capture->stall_count++;

            fprintf(
                stderr,
                "[CAPTURE][STALL] seq=%" PRIu64
                " gap=%.1fms pts=%.3fms duration=%.3fms\n",
                sequence,
                gap_ms,
                clocktime_ms(GST_BUFFER_PTS(buffer)),
                clocktime_ms(GST_BUFFER_DURATION(buffer))
            );
        }
    }

    if (capture->capture_trace) {
        fprintf(
            stderr,
            "[CAPTURE][FRAME] seq=%" PRIu64
            " gap=%.1fms pts=%.3fms dts=%.3fms duration=%.3fms\n",
            sequence,
            capture->last_sample_ns ? gap_ms : 0.0,
            clocktime_ms(GST_BUFFER_PTS(buffer)),
            clocktime_ms(GST_BUFFER_DTS(buffer)),
            clocktime_ms(GST_BUFFER_DURATION(buffer))
        );
    }

    capture->last_sample_ns = now_ns;
}

static void
log_capture_summary(const GstreamerCapture *capture)
{
    if (!capture || capture->sample_sequence == 0)
        return;

    fprintf(
        stderr,
        "[CAPTURE][SUMMARY] samples=%" PRIu64
        " intervals=%" PRIu64
        " stalls=%" PRIu64
        " max-gap=%.1fms "
        "hist[<250=%" PRIu64
        " 250-500=%" PRIu64
        " 0.5-1s=%" PRIu64
        " 1-2s=%" PRIu64
        " 2-5s=%" PRIu64
        " >=5s=%" PRIu64 "]\n",
        capture->sample_sequence,
        capture->interval_count,
        capture->stall_count,
        capture->max_interval_ms,
        capture->interval_histogram[0],
        capture->interval_histogram[1],
        capture->interval_histogram[2],
        capture->interval_histogram[3],
        capture->interval_histogram[4],
        capture->interval_histogram[5]
    );
}

static void
log_gstreamer_bus(
    GstElement *pipeline,
    GstClockTime timeout)
{
    if (!pipeline)
        return;

    GstBus *bus =
        gst_element_get_bus(pipeline);

    if (!bus)
        return;

    for (;;) {
        GstMessage *message =
            gst_bus_timed_pop_filtered(
                bus,
                timeout,
                GST_MESSAGE_ERROR |
                GST_MESSAGE_WARNING |
                GST_MESSAGE_INFO
            );

        timeout = 0;

        if (!message)
            break;

        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError *error = NULL;
                gchar *debug = NULL;

                gst_message_parse_error(
                    message,
                    &error,
                    &debug
                );

                fprintf(
                    stderr,
                    "[GSTREAMER][ERROR] %s: %s\n",
                    GST_OBJECT_NAME(
                        message->src
                    ),
                    error
                        ? error->message
                        : "unknown error"
                );

                if (
                    debug &&
                    *debug
                ) {
                    fprintf(
                        stderr,
                        "[GSTREAMER][DEBUG] %s\n",
                        debug
                    );
                }

                g_clear_error(&error);
                g_free(debug);
                break;
            }

            case GST_MESSAGE_WARNING: {
                GError *error = NULL;
                gchar *debug = NULL;

                gst_message_parse_warning(
                    message,
                    &error,
                    &debug
                );

                fprintf(
                    stderr,
                    "[GSTREAMER][WARN] %s: %s\n",
                    GST_OBJECT_NAME(
                        message->src
                    ),
                    error
                        ? error->message
                        : "unknown warning"
                );

                if (
                    debug &&
                    *debug
                ) {
                    fprintf(
                        stderr,
                        "[GSTREAMER][DEBUG] %s\n",
                        debug
                    );
                }

                g_clear_error(&error);
                g_free(debug);
                break;
            }

            case GST_MESSAGE_INFO: {
                GError *error = NULL;
                gchar *debug = NULL;

                gst_message_parse_info(
                    message,
                    &error,
                    &debug
                );

                fprintf(
                    stderr,
                    "[GSTREAMER][INFO] %s: %s\n",
                    GST_OBJECT_NAME(
                        message->src
                    ),
                    error
                        ? error->message
                        : "info"
                );

                g_clear_error(&error);
                g_free(debug);
                break;
            }

            default:
                break;
        }

        gst_message_unref(message);
    }

    gst_object_unref(bus);
}

static void
set_first_frame(
    GstreamerCapture *capture)
{
    pthread_mutex_lock(&capture->mutex);

    if (!capture->first_frame) {
        capture->first_frame = 1;
        pthread_cond_broadcast(
            &capture->cond
        );
    }

    pthread_mutex_unlock(&capture->mutex);
}

static int
should_stop(
    GstreamerCapture *capture)
{
    pthread_mutex_lock(&capture->mutex);
    int stop = capture->stop;
    pthread_mutex_unlock(&capture->mutex);
    return stop;
}

static void *
capture_thread(void *arg)
{
    GstreamerCapture *capture = arg;

    while (!should_stop(capture)) {
        GstSample *sample =
            gst_app_sink_try_pull_sample(
                GST_APP_SINK(
                    capture->appsink
                ),
                100 * GST_MSECOND
            );

        if (!sample)
            continue;

        GstCaps *caps =
            gst_sample_get_caps(sample);

        GstBuffer *buffer =
            gst_sample_get_buffer(sample);

        if (buffer)
            record_sample_interval(capture, buffer);

        if (
            caps &&
            !capture->caps_logged
        ) {
            char *caps_text =
                gst_caps_to_string(caps);

            fprintf(
                stderr,
                "[CAPTURE] negotiated caps: %s\n",
                caps_text
                    ? caps_text
                    : "(unknown)"
            );

            g_free(caps_text);
            capture->caps_logged = 1;
        }

        GstVideoInfo info;

        if (
            !caps ||
            !buffer ||
            !gst_video_info_from_caps(
                &info,
                caps
            )
        ) {
            gst_sample_unref(sample);
            continue;
        }

        if (
            GST_VIDEO_INFO_WIDTH(&info) !=
                capture->width ||
            GST_VIDEO_INFO_HEIGHT(&info) !=
                capture->height
        ) {
            fprintf(
                stderr,
                "Unexpected capture size: %ux%u\n",
                GST_VIDEO_INFO_WIDTH(&info),
                GST_VIDEO_INFO_HEIGHT(&info)
            );

            gst_sample_unref(sample);
            continue;
        }

        GstVideoFrame frame;

        if (
            !gst_video_frame_map(
                &frame,
                &info,
                buffer,
                GST_MAP_READ
            )
        ) {
            gst_sample_unref(sample);
            continue;
        }

        const uint8_t *pixels =
            GST_VIDEO_FRAME_PLANE_DATA(
                &frame,
                0
            );

        int stride =
            GST_VIDEO_FRAME_PLANE_STRIDE(
                &frame,
                0
            );

        if (
            frame_bridge_publish_bgrx(
                capture->bridge,
                pixels,
                stride,
                capture->width,
                capture->height
            ) == 0
        ) {
            pipeline_stats_capture(
                capture->stats,
                (size_t)capture->width *
                (size_t)capture->height *
                4
            );

            set_first_frame(capture);
        }

        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);
    }

    return NULL;
}

static int
wait_for_first_frame(
    GstreamerCapture *capture,
    int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(
        CLOCK_REALTIME,
        &deadline
    );

    deadline.tv_sec +=
        timeout_ms / 1000;

    deadline.tv_nsec +=
        (long)(
            timeout_ms % 1000
        ) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&capture->mutex);

    while (
        !capture->first_frame &&
        !capture->failed
    ) {
        int rc =
            pthread_cond_timedwait(
                &capture->cond,
                &capture->mutex,
                &deadline
            );

        if (rc != 0)
            break;
    }

    int ok =
        capture->first_frame &&
        !capture->failed;

    pthread_mutex_unlock(&capture->mutex);
    return ok ? 0 : -1;
}

int
gstreamer_capture_start(
    GstreamerCapture *capture,
    uint64_t object_serial,
    int width,
    int height,
    int fps,
    int timeout_ms,
    int capture_trace,
    int capture_stall_ms,
    FrameBridge *bridge,
    PipelineStats *stats)
{
    if (
        !capture ||
        !bridge ||
        width <= 0 ||
        height <= 0 ||
        fps <= 0
    )
        return -1;

    memset(capture, 0, sizeof(*capture));

    if (
        pthread_mutex_init(
            &capture->mutex,
            NULL
        ) != 0
    )
        return -1;

    if (
        pthread_cond_init(
            &capture->cond,
            NULL
        ) != 0
    ) {
        pthread_mutex_destroy(
            &capture->mutex
        );

        return -1;
    }

    capture->initialized = 1;
    capture->bridge = bridge;
    capture->stats = stats;
    capture->object_serial =
        object_serial;

    capture->width = width;
    capture->height = height;
    capture->fps = fps;
    capture->capture_trace = capture_trace;
    capture->capture_stall_ms = capture_stall_ms;

    gst_init(NULL, NULL);

    char *pipeline_description =
        g_strdup_printf(
            "pipewiresrc name=pw_source target-object=%" PRIu64 " "
            "do-timestamp=true ! "
            "video/x-raw,format=BGRx,width=%d,height=%d,max-framerate=%d/1 ! "
            "queue name=capture_queue max-size-buffers=1 "
            "max-size-bytes=0 max-size-time=0 leaky=downstream ! "
            "appsink name=frame_sink "
            "sync=false max-buffers=1 drop=true",
            object_serial,
            width,
            height,
            fps
        );

    GError *error = NULL;

    fprintf(
        stderr,
        "[CAPTURE] pipeline: %s\n",
        pipeline_description
    );

    capture->pipeline =
        gst_parse_launch(
            pipeline_description,
            &error
        );

    g_free(pipeline_description);

    if (!capture->pipeline) {
        fprintf(
            stderr,
            "Failed to create GStreamer pipeline: %s\n",
            error ? error->message : "unknown error"
        );

        g_clear_error(&error);
        goto fail;
    }

    /*
     * RecordVirtual supplies a small PipeWire buffer pool.  The default
     * pipewiresrc video path shares those buffers with downstream GStreamer
     * elements, so a slow/reordering consumer can temporarily exhaust the
     * producer pool.  V19 deliberately detaches each frame at pipewiresrc:
     * our architecture copies into FrameBridge anyway, so zero-copy buys us
     * nothing while keeping Mutter's buffers alive longer than necessary.
     *
     * Newer pipewiresrc exposes use-bufferpool=false.  Older compatible
     * versions expose the deprecated always-copy=true spelling.  Select the
     * best available property before the pipeline enters PLAYING.
     */
    GstElement *pipewire_src =
        gst_bin_get_by_name(
            GST_BIN(capture->pipeline),
            "pw_source"
        );

    if (!pipewire_src) {
        fprintf(stderr, "GStreamer pipewiresrc not found\n");
        goto fail;
    }

    GObjectClass *pipewire_src_class =
        G_OBJECT_GET_CLASS(pipewire_src);

    if (
        g_object_class_find_property(
            pipewire_src_class,
            "use-bufferpool"
        )
    ) {
        g_object_set(
            pipewire_src,
            "use-bufferpool", FALSE,
            NULL
        );

        fprintf(
            stderr,
            "[CAPTURE] PipeWire buffer mode: detached copy "
            "(use-bufferpool=false)\n"
        );
    }
    else if (
        g_object_class_find_property(
            pipewire_src_class,
            "always-copy"
        )
    ) {
        g_object_set(
            pipewire_src,
            "always-copy", TRUE,
            NULL
        );

        fprintf(
            stderr,
            "[CAPTURE] PipeWire buffer mode: detached copy "
            "(always-copy=true compatibility mode)\n"
        );
    }
    else {
        fprintf(
            stderr,
            "pipewiresrc cannot detach producer buffers: "
            "neither use-bufferpool nor always-copy is available\n"
        );
        gst_object_unref(pipewire_src);
        goto fail;
    }

    gst_object_unref(pipewire_src);

    capture->appsink =
        gst_bin_get_by_name(
            GST_BIN(capture->pipeline),
            "frame_sink"
        );

    if (!capture->appsink) {
        fprintf(
            stderr,
            "GStreamer appsink not found\n"
        );
        goto fail;
    }

    GstStateChangeReturn state_rc =
        gst_element_set_state(
            capture->pipeline,
            GST_STATE_PLAYING
        );

    if (
        state_rc ==
        GST_STATE_CHANGE_FAILURE
    ) {
        fprintf(
            stderr,
            "GStreamer pipeline failed to enter PLAYING state\n"
        );

        log_gstreamer_bus(
            capture->pipeline,
            500 * GST_MSECOND
        );

        goto fail;
    }

    if (
        pthread_create(
            &capture->thread,
            NULL,
            capture_thread,
            capture
        ) != 0
    ) {
        fprintf(
            stderr,
            "Failed to create capture thread\n"
        );
        goto fail;
    }

    capture->running = 1;

    if (
        wait_for_first_frame(
            capture,
            timeout_ms
        ) < 0
    ) {
        fprintf(
            stderr,
            "Timed out waiting for first PipeWire/GStreamer frame\n"
        );

        log_gstreamer_bus(
            capture->pipeline,
            100 * GST_MSECOND
        );

        gstreamer_capture_stop(capture);
        return -1;
    }

    printf(
        "GStreamer capture started: serial=%" PRIu64 ", %dx%d BGRx\n",
        object_serial,
        width,
        height
    );

    return 0;

fail:
    gstreamer_capture_stop(capture);
    return -1;
}

void
gstreamer_capture_stop(
    GstreamerCapture *capture)
{
    if (
        !capture ||
        !capture->initialized
    )
        return;

    pthread_mutex_lock(&capture->mutex);
    capture->stop = 1;
    pthread_cond_broadcast(&capture->cond);
    pthread_mutex_unlock(&capture->mutex);

    if (capture->pipeline) {
        gst_element_set_state(
            capture->pipeline,
            GST_STATE_NULL
        );
    }

    if (capture->running) {
        pthread_join(
            capture->thread,
            NULL
        );

        capture->running = 0;
    }

    log_capture_summary(capture);

    if (capture->appsink) {
        gst_object_unref(
            capture->appsink
        );

        capture->appsink = NULL;
    }

    if (capture->pipeline) {
        gst_object_unref(
            capture->pipeline
        );

        capture->pipeline = NULL;
    }

    /*
     * pthread_*_destroy on zero/uninitialized state is not safe. In this
     * module start() initializes both before any goto fail.
     */
    pthread_cond_destroy(&capture->cond);
    pthread_mutex_destroy(&capture->mutex);

    memset(capture, 0, sizeof(*capture));
}
