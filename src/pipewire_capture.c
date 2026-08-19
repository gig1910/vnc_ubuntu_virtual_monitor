#include "pipewire_capture.h"

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>
#include <spa/param/video/raw-utils.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DAMAGE_META_RECTS 32

static uint64_t
monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
record_frame_interval(PipewireCapture *capture)
{
    uint64_t now_ns = monotonic_now_ns();
    uint64_t sequence = ++capture->sample_sequence;
    double gap_ms = 0.0;

    if (capture->last_sample_ns != 0) {
        gap_ms = (double)(now_ns - capture->last_sample_ns) / 1000000.0;
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

        if (capture->capture_stall_ms > 0 &&
            gap_ms >= (double)capture->capture_stall_ms) {
            capture->stall_count++;
            fprintf(stderr,
                    "[CAPTURE][STALL] backend=pipewire seq=%" PRIu64
                    " gap=%.1fms\n",
                    sequence, gap_ms);
        }
    }

    if (capture->capture_trace) {
        fprintf(stderr,
                "[CAPTURE][FRAME] backend=pipewire seq=%" PRIu64
                " gap=%.1fms callbacks=%" PRIu64
                " dequeued=%" PRIu64 " recycled=%" PRIu64 "\n",
                sequence,
                capture->last_sample_ns ? gap_ms : 0.0,
                capture->process_callbacks,
                capture->dequeued_buffers,
                capture->stale_buffers_recycled);
    }

    capture->last_sample_ns = now_ns;
}

static void
log_capture_summary(const PipewireCapture *capture)
{
    if (!capture || capture->sample_sequence == 0)
        return;

    fprintf(stderr,
            "[CAPTURE][SUMMARY] backend=pipewire samples=%" PRIu64
            " intervals=%" PRIu64 " stalls=%" PRIu64
            " max-gap=%.1fms hist[<250=%" PRIu64
            " 250-500=%" PRIu64 " 0.5-1s=%" PRIu64
            " 1-2s=%" PRIu64 " 2-5s=%" PRIu64
            " >=5s=%" PRIu64 "] callbacks=%" PRIu64
            " dequeued=%" PRIu64 " stale-recycled=%" PRIu64
            " invalid=%" PRIu64 "\n",
            capture->sample_sequence,
            capture->interval_count,
            capture->stall_count,
            capture->max_interval_ms,
            capture->interval_histogram[0],
            capture->interval_histogram[1],
            capture->interval_histogram[2],
            capture->interval_histogram[3],
            capture->interval_histogram[4],
            capture->interval_histogram[5],
            capture->process_callbacks,
            capture->dequeued_buffers,
            capture->stale_buffers_recycled,
            capture->invalid_buffers);
}

static void
on_stream_state_changed(void *userdata,
                        enum pw_stream_state old,
                        enum pw_stream_state state,
                        const char *error)
{
    PipewireCapture *capture = userdata;

    fprintf(stderr,
            "[PIPEWIRE] stream state: %s -> %s%s%s%s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state),
            error ? " (" : "",
            error ? error : "",
            error ? ")" : "");

    pthread_mutex_lock(&capture->mutex);
    capture->streaming = (state == PW_STREAM_STATE_STREAMING);
    if (state == PW_STREAM_STATE_ERROR)
        capture->failed = 1;
    pthread_cond_broadcast(&capture->cond);
    pthread_mutex_unlock(&capture->mutex);
}

static void
on_stream_param_changed(void *userdata,
                        uint32_t id,
                        const struct spa_pod *param)
{
    PipewireCapture *capture = userdata;

    if (id != SPA_PARAM_Format || !param)
        return;

    struct spa_video_info_raw info;
    memset(&info, 0, sizeof(info));

    if (spa_format_video_raw_parse(param, &info) < 0) {
        fprintf(stderr, "[PIPEWIRE] could not parse negotiated video format\n");
        return;
    }

    capture->negotiated_width = (int)info.size.width;
    capture->negotiated_height = (int)info.size.height;
    capture->negotiated_format = info.format;
    capture->format_ready = 1;

    fprintf(stderr,
            "[CAPTURE] native PipeWire negotiated: format=%u size=%ux%u "
            "framerate=%u/%u max-framerate=%u/%u\n",
            info.format,
            info.size.width,
            info.size.height,
            info.framerate.num,
            info.framerate.denom,
            info.max_framerate.num,
            info.max_framerate.denom);

    if (info.format == SPA_VIDEO_FORMAT_BGRx &&
        (int)info.size.width == capture->width &&
        (int)info.size.height == capture->height) {
        uint8_t params_buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
        const struct spa_pod *params[2];
        int stride = capture->width * 4;
        int size = stride * capture->height;

        params[0] = spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
                (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd)));

        params[1] = spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size, SPA_POD_Int(
                (int)(sizeof(struct spa_meta_region) * DAMAGE_META_RECTS)));

        int rc = pw_stream_update_params(capture->stream, params, 2);
        if (rc < 0) {
            fprintf(stderr,
                    "[PIPEWIRE] buffer/meta parameter update failed: %s\n",
                    spa_strerror(rc));
        }
        else {
            fprintf(stderr,
                    "[CAPTURE] native PipeWire buffers: requested 8 "
                    "(range 2..16), stride=%d size=%d\n",
                    stride, size);
            fprintf(stderr,
                    "[CAPTURE] native PipeWire damage metadata: requested "
                    "up to %d regions\n",
                    DAMAGE_META_RECTS);
        }
    }

    if (info.format != SPA_VIDEO_FORMAT_BGRx ||
        (int)info.size.width != capture->width ||
        (int)info.size.height != capture->height) {
        fprintf(stderr,
                "[PIPEWIRE] unexpected format; expected BGRx %dx%d\n",
                capture->width, capture->height);
        pthread_mutex_lock(&capture->mutex);
        capture->failed = 1;
        pthread_cond_broadcast(&capture->cond);
        pthread_mutex_unlock(&capture->mutex);
    }
}

static int
copy_pw_region(PipewireCapture *capture,
               const struct spa_data *data,
               int32_t stride,
               int x1,
               int y1,
               int x2,
               int y2)
{
    if (x1 < 0 || y1 < 0 ||
        x2 > capture->width || y2 > capture->height ||
        x2 <= x1 || y2 <= y1)
        return -1;

    const size_t frame_row_bytes = (size_t)capture->width * 4u;
    const size_t region_row_bytes = (size_t)(x2 - x1) * 4u;
    const uint8_t *src = (const uint8_t *)data->data + data->chunk->offset;
    uint8_t *dst = capture->scratch;

    for (int y = y1; y < y2; y++) {
        memcpy(dst + (size_t)y * frame_row_bytes + (size_t)x1 * 4u,
               src + (size_t)y * (size_t)stride + (size_t)x1 * 4u,
               region_row_bytes);
    }

    return 0;
}

/*
 * Return values:
 *   1 = pixel state changed / full frame copied
 *   0 = valid buffer, but VideoDamage says there is no pixel change
 *  -1 = unusable buffer or malformed damage region
 *
 * Mutter may cycle several PipeWire buffers. Unchanged pixels in a reused
 * producer buffer must not be treated as authoritative: SPA_META_VideoDamage
 * describes the pixels produced for this frame. Keep one persistent private
 * framebuffer and apply only those regions. This prevents stale contents from
 * one buffer in the pool from reappearing every time that buffer is reused.
 */
static int
apply_pw_buffer(PipewireCapture *capture, struct pw_buffer *pwbuf)
{
    struct spa_buffer *buf = pwbuf ? pwbuf->buffer : NULL;
    if (!buf || buf->n_datas < 1)
        return -1;

    struct spa_data *data = &buf->datas[0];
    struct spa_chunk *chunk = data->chunk;

    if (!data->data || !chunk)
        return -1;

    const size_t frame_row_bytes = (size_t)capture->width * 4u;
    int32_t stride = chunk->stride;
    if (stride == 0)
        stride = (int32_t)frame_row_bytes;

    if (stride < (int32_t)frame_row_bytes || chunk->offset > data->maxsize)
        return -1;

    size_t needed =
        (size_t)(capture->height - 1) * (size_t)stride + frame_row_bytes;
    if (needed > (size_t)data->maxsize - (size_t)chunk->offset)
        return -1;

    struct spa_meta *damage_meta =
        spa_buffer_find_meta(buf, SPA_META_VideoDamage);

    if (!damage_meta) {
        return copy_pw_region(capture,
                              data,
                              stride,
                              0,
                              0,
                              capture->width,
                              capture->height) == 0 ? 1 : -1;
    }

    int regions_applied = 0;
    struct spa_meta_region *meta_region;

    spa_meta_for_each(meta_region, damage_meta) {
        if (!spa_meta_region_is_valid(meta_region))
            break;

        int64_t x1 = meta_region->region.position.x;
        int64_t y1 = meta_region->region.position.y;
        int64_t x2 = x1 + (int64_t)meta_region->region.size.width;
        int64_t y2 = y1 + (int64_t)meta_region->region.size.height;

        if (x1 < 0)
            x1 = 0;
        if (y1 < 0)
            y1 = 0;
        if (x2 > capture->width)
            x2 = capture->width;
        if (y2 > capture->height)
            y2 = capture->height;

        if (x2 <= x1 || y2 <= y1)
            continue;

        if (copy_pw_region(capture,
                           data,
                           stride,
                           (int)x1,
                           (int)y1,
                           (int)x2,
                           (int)y2) < 0)
            return -1;

        regions_applied++;
    }

    if (regions_applied > 0)
        return 1;

    /*
     * A first frame without usable damage cannot be reconstructed from prior
     * state. Fall back to one full copy so startup remains deterministic.
     */
    if (capture->sample_sequence == 0) {
        return copy_pw_region(capture,
                              data,
                              stride,
                              0,
                              0,
                              capture->width,
                              capture->height) == 0 ? 1 : -1;
    }

    return 0;
}

static void
log_invalid_buffer(PipewireCapture *capture, struct pw_buffer *pwbuf)
{
    capture->invalid_buffers++;

    if (capture->invalid_buffers > 5)
        return;

    struct spa_buffer *buf = pwbuf ? pwbuf->buffer : NULL;
    struct spa_data *data = buf && buf->n_datas ? &buf->datas[0] : NULL;

    fprintf(stderr,
            "[PIPEWIRE] unusable capture buffer: n_datas=%u "
            "type=%u data=%p maxsize=%u\n",
            buf ? buf->n_datas : 0,
            data ? data->type : 0,
            data ? data->data : NULL,
            data ? data->maxsize : 0);
}

static void
on_stream_process(void *userdata)
{
    PipewireCapture *capture = userdata;
    struct pw_buffer *buffer;
    int buffers_this_callback = 0;
    int valid_buffers = 0;
    int pixel_state_changed = 0;

    capture->process_callbacks++;

    /*
     * Drain every available capture buffer in order. We still publish only
     * once per process callback, but each buffer's VideoDamage must first be
     * applied to the persistent private framebuffer. Dropping an intermediate
     * damage buffer before applying it could lose a region that is absent from
     * the newest buffer's damage list.
     *
     * Every pw_buffer is returned immediately after its pixels are applied;
     * no PipeWire buffer survives into FrameBridge/VNC processing.
     */
    while ((buffer = pw_stream_dequeue_buffer(capture->stream)) != NULL) {
        capture->dequeued_buffers++;

        int apply_result = apply_pw_buffer(capture, buffer);

        /* Critical invariant: recycle before FrameBridge/VNC work. */
        pw_stream_queue_buffer(capture->stream, buffer);

        if (buffers_this_callback > 0)
            capture->stale_buffers_recycled++;
        buffers_this_callback++;

        if (apply_result < 0) {
            log_invalid_buffer(capture, buffer);
            continue;
        }

        valid_buffers++;
        if (apply_result > 0)
            pixel_state_changed = 1;
    }

    if (valid_buffers == 0)
        return;

    record_frame_interval(capture);
    pipeline_stats_capture(capture->stats,
                           (size_t)capture->width *
                           (size_t)capture->height * 4u);

    /* No pixel damage: keep capture telemetry, but do not manufacture a VNC frame. */
    if (!pixel_state_changed)
        return;

    if (frame_bridge_publish_bgrx(capture->bridge,
                                  capture->scratch,
                                  capture->width * 4,
                                  capture->width,
                                  capture->height) == 0) {
        pthread_mutex_lock(&capture->mutex);
        if (!capture->first_frame) {
            capture->first_frame = 1;
            pthread_cond_broadcast(&capture->cond);
        }
        pthread_mutex_unlock(&capture->mutex);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

static int
wait_for_first_frame(PipewireCapture *capture, int timeout_ms)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        return -1;

    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&capture->mutex);
    while (!capture->first_frame && !capture->failed) {
        int rc = pthread_cond_timedwait(&capture->cond,
                                        &capture->mutex,
                                        &deadline);
        if (rc == ETIMEDOUT)
            break;
        if (rc != 0) {
            capture->failed = 1;
            break;
        }
    }
    int ok = capture->first_frame && !capture->failed;
    pthread_mutex_unlock(&capture->mutex);
    return ok ? 0 : -1;
}

int
pipewire_capture_start(PipewireCapture *capture,
                       uint32_t node_id,
                       int width,
                       int height,
                       int fps,
                       int timeout_ms,
                       int capture_trace,
                       int capture_stall_ms,
                       FrameBridge *bridge,
                       PipelineStats *stats)
{
    if (!capture || !bridge || width <= 0 || height <= 0 || fps <= 0)
        return -1;

    memset(capture, 0, sizeof(*capture));

    if (pthread_mutex_init(&capture->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&capture->cond, NULL) != 0) {
        pthread_mutex_destroy(&capture->mutex);
        return -1;
    }

    capture->initialized = 1;
    capture->bridge = bridge;
    capture->stats = stats;
    capture->target_node_id = node_id;
    capture->width = width;
    capture->height = height;
    capture->fps = fps;
    capture->capture_trace = capture_trace;
    capture->capture_stall_ms = capture_stall_ms;

    capture->scratch = calloc((size_t)width * (size_t)height, 4u);
    if (!capture->scratch)
        goto fail;

    pw_init(NULL, NULL);

    capture->loop = pw_thread_loop_new("vnc-monitor-pipewire", NULL);
    if (!capture->loop) {
        fprintf(stderr, "Failed to create PipeWire thread loop\n");
        goto fail;
    }

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE((uint32_t)width,
                                                                  (uint32_t)height)));

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_MEDIA_CLASS, "Stream/Input/Video",
        NULL);

    if (!props) {
        fprintf(stderr, "Failed to create PipeWire properties\n");
        goto fail;
    }

    pw_thread_loop_lock(capture->loop);

    if (pw_thread_loop_start(capture->loop) < 0) {
        pw_thread_loop_unlock(capture->loop);
        pw_properties_free(props);
        fprintf(stderr, "Failed to start PipeWire thread loop\n");
        goto fail;
    }

    capture->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(capture->loop),
        "vnc-monitor-native-capture",
        props,
        &stream_events,
        capture);

    if (!capture->stream) {
        pw_thread_loop_unlock(capture->loop);
        fprintf(stderr, "Failed to create native PipeWire stream\n");
        goto fail;
    }

    int rc = pw_stream_connect(
        capture->stream,
        PW_DIRECTION_INPUT,
        node_id,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params,
        1);

    pw_thread_loop_unlock(capture->loop);

    if (rc < 0) {
        fprintf(stderr,
                "Failed to connect native PipeWire capture to node %u: %s\n",
                node_id,
                spa_strerror(rc));
        goto fail;
    }

    fprintf(stderr,
            "[CAPTURE] backend: native PipeWire (target node=%u)\n"
            "[CAPTURE] policy: drain all VideoDamage into persistent BGRx, "
            "publish latest once, recycle pw_buffer before FrameBridge publish\n",
            node_id);

    if (wait_for_first_frame(capture, timeout_ms) < 0) {
        fprintf(stderr,
                "Timed out waiting for first native PipeWire frame from node %u\n",
                node_id);
        goto fail;
    }

    printf("Native PipeWire capture started: node=%u, %dx%d BGRx\n",
           node_id, width, height);
    return 0;

fail:
    pipewire_capture_stop(capture);
    return -1;
}

void
pipewire_capture_stop(PipewireCapture *capture)
{
    if (!capture || !capture->initialized)
        return;

    if (capture->loop) {
        pw_thread_loop_lock(capture->loop);
        if (capture->stream) {
            pw_stream_disconnect(capture->stream);
            pw_stream_destroy(capture->stream);
            capture->stream = NULL;
        }
        pw_thread_loop_unlock(capture->loop);
        pw_thread_loop_stop(capture->loop);
        pw_thread_loop_destroy(capture->loop);
        capture->loop = NULL;
    }

    log_capture_summary(capture);

    free(capture->scratch);
    capture->scratch = NULL;

    pthread_cond_destroy(&capture->cond);
    pthread_mutex_destroy(&capture->mutex);

    pw_deinit();
    memset(capture, 0, sizeof(*capture));
}