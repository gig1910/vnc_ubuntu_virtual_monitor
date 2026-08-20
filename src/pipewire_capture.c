#include "pipewire_capture.h"

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/utils/result.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CURSOR_MAX_WIDTH 384
#define CURSOR_MAX_HEIGHT 384
#define CURSOR_META_SIZE(width, height) \
    ((int)(sizeof(struct spa_meta_cursor) + \
           sizeof(struct spa_meta_bitmap) + \
           (size_t)(width) * (size_t)(height) * 4u))

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
                    " update-gap=%.1fms\n",
                    sequence,
                    gap_ms);
        }
    }

    if (capture->capture_trace) {
        fprintf(stderr,
                "[CAPTURE][FRAME] backend=pipewire seq=%" PRIu64
                " gap=%.1fms callbacks=%" PRIu64
                " dequeued=%" PRIu64
                " video=%" PRIu64
                " cursor=%" PRIu64
                " cursor-only=%" PRIu64
                " empty=%" PRIu64 "\n",
                sequence,
                capture->last_sample_ns ? gap_ms : 0.0,
                capture->process_callbacks,
                capture->dequeued_buffers,
                capture->video_frames,
                capture->cursor_updates,
                capture->cursor_only_updates,
                capture->empty_buffers);
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
            " intervals=%" PRIu64
            " stalls=%" PRIu64
            " max-gap=%.1fms hist[<250=%" PRIu64
            " 250-500=%" PRIu64
            " 0.5-1s=%" PRIu64
            " 1-2s=%" PRIu64
            " 2-5s=%" PRIu64
            " >=5s=%" PRIu64
            "] callbacks=%" PRIu64
            " dequeued=%" PRIu64
            " stale-recycled=%" PRIu64
            " video=%" PRIu64
            " empty=%" PRIu64
            " cursor=%" PRIu64
            " cursor-only=%" PRIu64
            " cursor-invalid=%" PRIu64
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
            capture->video_frames,
            capture->empty_buffers,
            capture->cursor_updates,
            capture->cursor_only_updates,
            capture->invalid_cursor_metadata,
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
        fprintf(stderr,
                "[PIPEWIRE] could not parse negotiated video format\n");
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
        struct spa_pod_builder b =
            SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
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
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
                CURSOR_META_SIZE(CURSOR_MAX_WIDTH, CURSOR_MAX_HEIGHT),
                CURSOR_META_SIZE(1, 1),
                CURSOR_META_SIZE(CURSOR_MAX_WIDTH, CURSOR_MAX_HEIGHT)));

        int rc = pw_stream_update_params(capture->stream, params, 2);
        if (rc < 0) {
            fprintf(stderr,
                    "[PIPEWIRE] buffer/cursor-meta parameter update failed: %s\n",
                    spa_strerror(rc));
        }
        else {
            fprintf(stderr,
                    "[CAPTURE] native PipeWire buffers: requested 8 "
                    "(range 2..16), stride=%d size=%d\n",
                    stride,
                    size);
            fprintf(stderr,
                    "[CAPTURE] native PipeWire cursor metadata: requested "
                    "up to %dx%d RGBA\n",
                    CURSOR_MAX_WIDTH,
                    CURSOR_MAX_HEIGHT);
        }
    }

    if (info.format != SPA_VIDEO_FORMAT_BGRx ||
        (int)info.size.width != capture->width ||
        (int)info.size.height != capture->height) {
        fprintf(stderr,
                "[PIPEWIRE] unexpected format; expected BGRx %dx%d\n",
                capture->width,
                capture->height);

        pthread_mutex_lock(&capture->mutex);
        capture->failed = 1;
        pthread_cond_broadcast(&capture->cond);
        pthread_mutex_unlock(&capture->mutex);
    }
}

/*
 * Return values:
 *   1 = complete usable BGRx frame copied to base_frame
 *   0 = intentionally empty/corrupted video payload
 *  -1 = malformed/unusable video payload
 *
 * Empty video payload does NOT mean the PipeWire buffer is useless: in
 * cursor-metadata mode Mutter deliberately sends cursor-only updates with no
 * video pixels. on_stream_process() therefore reads SPA_META_Cursor before
 * recycling every buffer, regardless of this return value.
 */
static int
copy_pw_buffer(PipewireCapture *capture, struct pw_buffer *pwbuf)
{
    struct spa_buffer *buf = pwbuf ? pwbuf->buffer : NULL;
    if (!buf || buf->n_datas < 1)
        return -1;

    struct spa_data *data = &buf->datas[0];
    struct spa_chunk *chunk = data->chunk;

    if (!chunk)
        return -1;

    if ((chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0 ||
        chunk->size == 0)
        return 0;

    if (!data->data)
        return -1;

    const size_t row_bytes = (size_t)capture->width * 4u;
    int32_t stride = chunk->stride;
    if (stride == 0)
        stride = (int32_t)row_bytes;

    if (stride < (int32_t)row_bytes ||
        chunk->offset > data->maxsize)
        return -1;

    size_t needed =
        (size_t)(capture->height - 1) * (size_t)stride + row_bytes;

    if (needed > (size_t)data->maxsize - (size_t)chunk->offset)
        return -1;

    if ((size_t)chunk->size < needed)
        return -1;

    const uint8_t *src =
        (const uint8_t *)data->data + (size_t)chunk->offset;
    uint8_t *dst = capture->base_frame;

    for (int y = 0; y < capture->height; y++) {
        memcpy(dst + (size_t)y * row_bytes,
               src + (size_t)y * (size_t)stride,
               row_bytes);
    }

    return 1;
}

/*
 * Read a Mutter SPA_META_Cursor record and copy any new bitmap into private
 * memory before the pw_buffer is returned.
 *
 * Mutter sends the bitmap as premultiplied RGBA. Position-only metadata has
 * bitmap_offset == 0; in that case the previously cached bitmap and hotspot
 * must be retained.
 *
 * Return:
 *   1 = cursor state changed
 *   0 = no cursor meta, or state unchanged
 *  -1 = malformed cursor meta
 */
static int
read_cursor_metadata(PipewireCapture *capture, struct spa_buffer *buf)
{
    struct spa_meta *meta =
        buf ? spa_buffer_find_meta(buf, SPA_META_Cursor) : NULL;

    if (!meta || !meta->data || meta->size < sizeof(struct spa_meta_cursor))
        return 0;

    struct spa_meta_cursor *cursor = meta->data;

    if (!spa_meta_cursor_is_valid(cursor)) {
        int changed = capture->cursor_visible;
        capture->cursor_visible = 0;
        return changed;
    }

    int changed = 0;

    if (!capture->cursor_visible ||
        capture->cursor_x != cursor->position.x ||
        capture->cursor_y != cursor->position.y)
        changed = 1;

    capture->cursor_visible = 1;
    capture->cursor_x = cursor->position.x;
    capture->cursor_y = cursor->position.y;

    if (cursor->bitmap_offset == 0)
        return changed;

    size_t bitmap_pos = (size_t)cursor->bitmap_offset;
    if (bitmap_pos < sizeof(struct spa_meta_cursor) ||
        bitmap_pos > meta->size ||
        meta->size - bitmap_pos < sizeof(struct spa_meta_bitmap))
        return -1;

    struct spa_meta_bitmap *bitmap =
        SPA_MEMBER(cursor,
                   cursor->bitmap_offset,
                   struct spa_meta_bitmap);

    /*
     * SPA says format/offset 0 means "no new bitmap information". Keep the
     * previously cached cursor shape in that case; position is still valid.
     */
    if (!spa_meta_bitmap_is_valid(bitmap) ||
        bitmap->offset == 0 ||
        bitmap->size.width == 0 ||
        bitmap->size.height == 0)
        return changed;

    if (bitmap->format != SPA_VIDEO_FORMAT_RGBA ||
        bitmap->size.width > CURSOR_MAX_WIDTH ||
        bitmap->size.height > CURSOR_MAX_HEIGHT ||
        bitmap->offset < sizeof(struct spa_meta_bitmap))
        return -1;

    size_t width = bitmap->size.width;
    size_t height = bitmap->size.height;
    size_t row_bytes = width * 4u;
    size_t stride = bitmap->stride ? (size_t)bitmap->stride : row_bytes;

    if (stride < row_bytes)
        return -1;

    size_t bitmap_data_pos = bitmap_pos + (size_t)bitmap->offset;
    if (bitmap_data_pos > meta->size)
        return -1;

    size_t needed =
        (height - 1u) * stride + row_bytes;

    if (needed > (size_t)meta->size - bitmap_data_pos ||
        width * height * 4u > capture->cursor_bitmap_capacity)
        return -1;

    const uint8_t *src =
        SPA_MEMBER(bitmap, bitmap->offset, uint8_t);

    for (size_t y = 0; y < height; y++) {
        memcpy(capture->cursor_bitmap + y * row_bytes,
               src + y * stride,
               row_bytes);
    }

    capture->cursor_bitmap_valid = 1;
    capture->cursor_width = (int)width;
    capture->cursor_height = (int)height;
    capture->cursor_stride = (int)row_bytes;
    capture->cursor_hotspot_x = cursor->hotspot.x;
    capture->cursor_hotspot_y = cursor->hotspot.y;

    /* A bitmap is sent only for a shape/scale change, so treat it as damage. */
    return 1;
}

static uint8_t
blend_premultiplied(uint8_t src, uint8_t dst, uint8_t alpha)
{
    unsigned int value =
        (unsigned int)src +
        ((unsigned int)dst * (unsigned int)(255u - alpha) + 127u) / 255u;

    return (uint8_t)(value > 255u ? 255u : value);
}

static void
compose_output_frame(PipewireCapture *capture)
{
    size_t frame_bytes =
        (size_t)capture->width * (size_t)capture->height * 4u;

    memcpy(capture->scratch, capture->base_frame, frame_bytes);

    if (!capture->cursor_visible ||
        !capture->cursor_bitmap_valid ||
        capture->cursor_width <= 0 ||
        capture->cursor_height <= 0)
        return;

    int origin_x = capture->cursor_x - capture->cursor_hotspot_x;
    int origin_y = capture->cursor_y - capture->cursor_hotspot_y;

    for (int sy = 0; sy < capture->cursor_height; sy++) {
        int dy = origin_y + sy;
        if (dy < 0 || dy >= capture->height)
            continue;

        const uint8_t *src_row =
            capture->cursor_bitmap +
            (size_t)sy * (size_t)capture->cursor_stride;

        for (int sx = 0; sx < capture->cursor_width; sx++) {
            int dx = origin_x + sx;
            if (dx < 0 || dx >= capture->width)
                continue;

            const uint8_t *src = src_row + (size_t)sx * 4u;
            uint8_t alpha = src[3];

            if (alpha == 0)
                continue;

            uint8_t *dst =
                capture->scratch +
                ((size_t)dy * (size_t)capture->width + (size_t)dx) * 4u;

            if (alpha == 255) {
                /* Cursor bitmap is RGBA; framebuffer is BGRx. */
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            }
            else {
                /*
                 * Mutter exports COGL_PIXEL_FORMAT_RGBA_8888_PRE, i.e.
                 * premultiplied-alpha RGBA. Do not multiply src by alpha
                 * a second time.
                 */
                dst[0] = blend_premultiplied(src[2], dst[0], alpha);
                dst[1] = blend_premultiplied(src[1], dst[1], alpha);
                dst[2] = blend_premultiplied(src[0], dst[2], alpha);
            }

            dst[3] = 0xff;
        }
    }
}

static void
log_invalid_buffer(PipewireCapture *capture, struct pw_buffer *pwbuf)
{
    capture->invalid_buffers++;

    if (capture->invalid_buffers > 5)
        return;

    struct spa_buffer *buf = pwbuf ? pwbuf->buffer : NULL;
    struct spa_data *data = buf && buf->n_datas ? &buf->datas[0] : NULL;
    struct spa_chunk *chunk = data ? data->chunk : NULL;

    fprintf(stderr,
            "[PIPEWIRE] unusable capture buffer: n_datas=%u "
            "type=%u data=%p maxsize=%u chunk-size=%u chunk-flags=0x%x\n",
            buf ? buf->n_datas : 0,
            data ? data->type : 0,
            data ? data->data : NULL,
            data ? data->maxsize : 0,
            chunk ? chunk->size : 0,
            chunk ? chunk->flags : 0);
}

static void
on_stream_process(void *userdata)
{
    PipewireCapture *capture = userdata;
    struct pw_buffer *buffer;
    int valid_video_buffers = 0;
    int cursor_changed = 0;

    capture->process_callbacks++;

    /*
     * Drain every available buffer in producer order. Cursor metadata is read
     * even from an empty/corrupted video chunk, exactly because Mutter uses
     * those buffers for cursor-only updates in metadata mode.
     *
     * Video and cursor bitmap data are copied into private memory before each
     * pw_buffer is queued back to PipeWire. We publish only once per process
     * callback, so intermediate pointer positions are naturally coalesced.
     */
    while ((buffer = pw_stream_dequeue_buffer(capture->stream)) != NULL) {
        capture->dequeued_buffers++;

        struct spa_buffer *spa_buffer = buffer->buffer;
        int cursor_result =
            read_cursor_metadata(capture, spa_buffer);

        if (cursor_result < 0) {
            capture->invalid_cursor_metadata++;
            if (capture->invalid_cursor_metadata <= 5) {
                fprintf(stderr,
                        "[PIPEWIRE] malformed SPA_META_Cursor ignored\n");
            }
        }
        else if (cursor_result > 0) {
            cursor_changed = 1;
            capture->cursor_updates++;
        }

        int copy_result = copy_pw_buffer(capture, buffer);

        if (copy_result < 0)
            log_invalid_buffer(capture, buffer);

        /* Critical invariant: recycle before FrameBridge/VNC work. */
        pw_stream_queue_buffer(capture->stream, buffer);

        if (copy_result == 0) {
            capture->empty_buffers++;
            continue;
        }

        if (copy_result < 0)
            continue;

        if (valid_video_buffers > 0)
            capture->stale_buffers_recycled++;

        valid_video_buffers++;
        capture->video_frames++;
        capture->have_base_frame = 1;
    }

    if (!capture->have_base_frame)
        return;

    if (valid_video_buffers == 0 && !cursor_changed)
        return;

    if (valid_video_buffers == 0)
        capture->cursor_only_updates++;

    compose_output_frame(capture);
    record_frame_interval(capture);

    pipeline_stats_capture(capture->stats,
                           (size_t)capture->width *
                           (size_t)capture->height * 4u);

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

    size_t frame_bytes =
        (size_t)width * (size_t)height * 4u;

    capture->base_frame = malloc(frame_bytes);
    capture->scratch = malloc(frame_bytes);
    capture->cursor_bitmap_capacity =
        (size_t)CURSOR_MAX_WIDTH * (size_t)CURSOR_MAX_HEIGHT * 4u;
    capture->cursor_bitmap = malloc(capture->cursor_bitmap_capacity);

    if (!capture->base_frame ||
        !capture->scratch ||
        !capture->cursor_bitmap)
        goto fail;

    pw_init(NULL, NULL);

    capture->loop = pw_thread_loop_new("vnc-monitor-pipewire", NULL);
    if (!capture->loop) {
        fprintf(stderr,
                "Failed to create PipeWire thread loop\n");
        goto fail;
    }

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(
            &SPA_RECTANGLE((uint32_t)width, (uint32_t)height)));

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_MEDIA_CLASS, "Stream/Input/Video",
        NULL);

    if (!props) {
        fprintf(stderr,
                "Failed to create PipeWire properties\n");
        goto fail;
    }

    pw_thread_loop_lock(capture->loop);

    if (pw_thread_loop_start(capture->loop) < 0) {
        pw_thread_loop_unlock(capture->loop);
        pw_properties_free(props);
        fprintf(stderr,
                "Failed to start PipeWire thread loop\n");
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
        fprintf(stderr,
                "Failed to create native PipeWire stream\n");
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
            "[CAPTURE] policy: complete BGRx base + SPA_META_Cursor "
            "software composition; empty video may carry cursor-only "
            "metadata; recycle pw_buffer before FrameBridge publish\n",
            node_id);

    if (wait_for_first_frame(capture, timeout_ms) < 0) {
        fprintf(stderr,
                "Timed out waiting for first native PipeWire frame from node %u\n",
                node_id);
        goto fail;
    }

    printf("Native PipeWire capture started: node=%u, %dx%d BGRx\n",
           node_id,
           width,
           height);

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

    free(capture->cursor_bitmap);
    capture->cursor_bitmap = NULL;
    free(capture->scratch);
    capture->scratch = NULL;
    free(capture->base_frame);
    capture->base_frame = NULL;

    pthread_cond_destroy(&capture->cond);
    pthread_mutex_destroy(&capture->mutex);

    pw_deinit();
    memset(capture, 0, sizeof(*capture));
}
