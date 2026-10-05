#define _GNU_SOURCE

#include "web_hls.h"
#include "broker_protocol.h"
#include "log.h"

#include <gst/app/gstappsrc.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WEB_HLS_BITRATE_KBIT 2500
#define WEB_HLS_PLAYLIST_FILES 4
#define WEB_HLS_PLAYLIST_LENGTH 3
#define WEB_HLS_TARGET_DURATION 1

static int
stream_should_stop(WebHlsStream *stream)
{
    pthread_mutex_lock(&stream->mutex);
    int stop = stream->stop;
    pthread_mutex_unlock(&stream->mutex);
    return stop;
}

static void
stream_request_stop(WebHlsStream *stream)
{
    pthread_mutex_lock(&stream->mutex);
    stream->stop = 1;
    pthread_mutex_unlock(&stream->mutex);
}

static void
cleanup_hls_files(const char *directory)
{
    if (!directory || !*directory)
        return;

    GError *error = NULL;
    GDir *dir = g_dir_open(directory, 0, &error);
    if (!dir) {
        g_clear_error(&error);
        return;
    }

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (strcmp(name, "index.m3u8") != 0 &&
            !(g_str_has_prefix(name, "segment") &&
              g_str_has_suffix(name, ".ts")))
            continue;

        char *path = g_build_filename(directory, name, NULL);
        (void)g_remove(path);
        g_free(path);
    }

    g_dir_close(dir);
}

static int
playlist_ready(const char *path)
{
    GStatBuf st;
    return path && g_stat(path, &st) == 0 && st.st_size > 0;
}

static int
send_hls_ready(WebHlsStream *stream)
{
    if (stream->ready_sent)
        return 0;

    if (!playlist_ready(stream->playlist))
        return 0;

    if (vnc_broker_send_control(stream->control_fd,
                                VNC_BROKER_CONTROL_HLS_READY,
                                NULL,
                                0) < 0) {
        return -1;
    }

    stream->ready_sent = 1;
    LOG_INFO("Legacy browser HLS playlist ready: %s", stream->playlist);
    return 0;
}

static int
check_pipeline_error(WebHlsStream *stream)
{
    GstBus *bus = gst_element_get_bus(stream->pipeline);
    if (!bus)
        return 0;

    GstMessage *message =
        gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    gst_object_unref(bus);

    if (!message)
        return 0;

    int failed = 1;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        LOG_ERROR("Legacy browser HLS pipeline error: %s",
                  error ? error->message : "unknown error");
        if (debug && *debug)
            LOG_DEBUG("Legacy browser HLS pipeline debug: %s", debug);
        g_clear_error(&error);
        g_free(debug);
    }
    else {
        LOG_INFO("Legacy browser HLS pipeline reached EOS");
    }

    gst_message_unref(message);
    return failed;
}

static int
sleep_frame_interval(int fps)
{
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000000L / fps
    };

    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

static void *
hls_feeder_thread(void *opaque)
{
    WebHlsStream *stream = opaque;
    size_t frame_bytes =
        (size_t)stream->width * (size_t)stream->height * 4u;

    uint8_t *pixels = malloc(frame_bytes);
    if (!pixels) {
        (void)shutdown(stream->control_fd, SHUT_RDWR);
        return NULL;
    }

    uint64_t sequence = 0;
    uint64_t frame_index = 0;
    int have_frame = 0;
    const GstClockTime duration = GST_SECOND / (GstClockTime)stream->fps;

    while (!stream_should_stop(stream)) {
        if (!have_frame) {
            uint64_t changed_sequence = 0;
            int changed =
                frame_bridge_wait_for_change(stream->frames,
                                             sequence,
                                             100,
                                             &changed_sequence);
            if (changed < 0)
                break;
            if (changed == 0) {
                if (check_pipeline_error(stream))
                    break;
                continue;
            }

            uint64_t consume_sequence = sequence;
            if (frame_bridge_consume(stream->frames,
                                     pixels,
                                     &consume_sequence) <= 0)
                continue;

            sequence = consume_sequence;
            have_frame = 1;
        }
        else {
            uint64_t changed_sequence = 0;
            int changed =
                frame_bridge_wait_for_change(stream->frames,
                                             sequence,
                                             0,
                                             &changed_sequence);
            if (changed < 0)
                break;
            if (changed > 0) {
                uint64_t consume_sequence = sequence;
                if (frame_bridge_consume(stream->frames,
                                         pixels,
                                         &consume_sequence) > 0)
                    sequence = consume_sequence;
            }
        }

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, frame_bytes, NULL);
        if (!buffer)
            break;

        GstMapInfo map;
        memset(&map, 0, sizeof(map));
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            break;
        }
        if (map.size < frame_bytes) {
            gst_buffer_unmap(buffer, &map);
            gst_buffer_unref(buffer);
            break;
        }

        memcpy(map.data, pixels, frame_bytes);
        gst_buffer_unmap(buffer, &map);

        GST_BUFFER_PTS(buffer) = frame_index * duration;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer) = duration;

        GstFlowReturn flow =
            gst_app_src_push_buffer(GST_APP_SRC(stream->appsrc), buffer);
        if (flow != GST_FLOW_OK) {
            LOG_INFO("Legacy browser HLS appsrc stopped: %s",
                     gst_flow_get_name(flow));
            break;
        }

        frame_index++;

        if (send_hls_ready(stream) < 0)
            break;

        if (check_pipeline_error(stream))
            break;

        if (sleep_frame_interval(stream->fps) < 0)
            break;
    }

    free(pixels);

    if (!stream_should_stop(stream)) {
        stream_request_stop(stream);
        (void)shutdown(stream->control_fd, SHUT_RDWR);
    }

    return NULL;
}

static int
prepare_runtime_directory(WebHlsStream *stream)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        errno = ENOENT;
        return -1;
    }

    int n = snprintf(stream->directory,
                     sizeof(stream->directory),
                     "%s/vnc-monitor/hls",
                     runtime);
    if (n < 0 || (size_t)n >= sizeof(stream->directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (g_mkdir_with_parents(stream->directory, 0700) < 0)
        return -1;
    (void)chmod(stream->directory, 0700);

    n = snprintf(stream->playlist,
                 sizeof(stream->playlist),
                 "%s/index.m3u8",
                 stream->directory);
    if (n < 0 || (size_t)n >= sizeof(stream->playlist)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    cleanup_hls_files(stream->directory);
    return 0;
}

int
web_hls_start(WebHlsStream *stream,
              int control_fd,
              FrameBridge *frames,
              int width,
              int height,
              int fps)
{
    if (!stream || control_fd < 0 || !frames ||
        width <= 0 || height <= 0 || fps <= 0) {
        errno = EINVAL;
        return -1;
    }

    memset(stream, 0, sizeof(*stream));
    if (pthread_mutex_init(&stream->mutex, NULL) != 0)
        return -1;

    stream->initialized = 1;
    stream->control_fd = control_fd;
    stream->frames = frames;
    stream->width = width;
    stream->height = height;
    stream->fps = fps;

    if (prepare_runtime_directory(stream) < 0)
        goto fail;

    gst_init(NULL, NULL);

    GstElementFactory *x264_factory = gst_element_factory_find("x264enc");
    GstElementFactory *hls_factory = gst_element_factory_find("hlssink2");
    GstElementFactory *parse_factory = gst_element_factory_find("h264parse");
    gboolean have_plugins = x264_factory && hls_factory && parse_factory;
    if (x264_factory) gst_object_unref(x264_factory);
    if (hls_factory) gst_object_unref(hls_factory);
    if (parse_factory) gst_object_unref(parse_factory);

    if (!have_plugins) {
        LOG_INFO("Legacy browser HLS unavailable: x264enc/hlssink2/h264parse plugin missing");
        errno = ENOSYS;
        goto fail;
    }

    char *segment_path =
        g_strdup_printf("%s/segment%%05d.ts", stream->directory);
    char *quoted_segment = g_shell_quote(segment_path);
    char *quoted_playlist = g_shell_quote(stream->playlist);

    char *pipeline_text = g_strdup_printf(
        "appsrc name=src is-live=true block=false format=time emit-signals=false "
        "! queue max-size-buffers=2 leaky=downstream "
        "! videoconvert "
        "! video/x-raw,format=I420 "
        "! x264enc bitrate=%d speed-preset=ultrafast tune=zerolatency "
        "key-int-max=%d bframes=0 "
        "! video/x-h264,profile=baseline "
        "! h264parse config-interval=-1 "
        "! hlssink2 name=hls max-files=%d playlist-length=%d "
        "target-duration=%d send-keyframe-requests=true playlist-root=/live "
        "location=%s playlist-location=%s",
        WEB_HLS_BITRATE_KBIT,
        fps,
        WEB_HLS_PLAYLIST_FILES,
        WEB_HLS_PLAYLIST_LENGTH,
        WEB_HLS_TARGET_DURATION,
        quoted_segment,
        quoted_playlist);

    g_free(segment_path);
    g_free(quoted_segment);
    g_free(quoted_playlist);

    GError *error = NULL;
    stream->pipeline = gst_parse_launch(pipeline_text, &error);
    g_free(pipeline_text);

    if (!stream->pipeline) {
        LOG_INFO("Legacy browser HLS pipeline could not be created: %s",
                 error ? error->message : "unknown error");
        g_clear_error(&error);
        goto fail;
    }
    g_clear_error(&error);

    stream->appsrc = gst_bin_get_by_name(GST_BIN(stream->pipeline), "src");
    if (!stream->appsrc)
        goto fail;

    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "BGRx",
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, fps, 1,
        NULL);
    gst_app_src_set_caps(GST_APP_SRC(stream->appsrc), caps);
    gst_caps_unref(caps);

    g_object_set(stream->appsrc,
                 "max-bytes",
                 (guint64)((size_t)width * (size_t)height * 8u),
                 NULL);

    GstStateChangeReturn state =
        gst_element_set_state(stream->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
        LOG_INFO("Legacy browser HLS pipeline refused PLAYING state");
        goto fail;
    }

    int rc = pthread_create(&stream->thread, NULL,
                            hls_feeder_thread, stream);
    if (rc != 0) {
        errno = rc;
        goto fail;
    }

    stream->thread_started = 1;
    LOG_INFO("Legacy browser HLS test path started: H.264 baseline x264 %dx%d@%dfps target=%ds",
             width, height, fps, WEB_HLS_TARGET_DURATION);
    return 0;

fail:
    web_hls_stop(stream);
    return -1;
}

void
web_hls_stop(WebHlsStream *stream)
{
    if (!stream || !stream->initialized)
        return;

    stream_request_stop(stream);

    if (stream->thread_started) {
        (void)pthread_join(stream->thread, NULL);
        stream->thread_started = 0;
    }

    if (stream->appsrc) {
        (void)gst_app_src_end_of_stream(GST_APP_SRC(stream->appsrc));
        gst_object_unref(stream->appsrc);
        stream->appsrc = NULL;
    }

    if (stream->pipeline) {
        (void)gst_element_set_state(stream->pipeline, GST_STATE_NULL);
        gst_object_unref(stream->pipeline);
        stream->pipeline = NULL;
    }

    cleanup_hls_files(stream->directory);
    pthread_mutex_destroy(&stream->mutex);
    memset(stream, 0, sizeof(*stream));
}
