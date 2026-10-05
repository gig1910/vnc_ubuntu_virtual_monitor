#define _GNU_SOURCE

#include "auth_client.h"
#include "broker_protocol.h"
#include "config.h"
#include "ra2.h"
#include "rfb_backend.h"
#include "rfb_proxy.h"
#include "runtime_config.h"
#include "shutdown_signal.h"
#include "frame_bridge.h"
#include "web_jpeg.h"
#include "web_hls.h"
#include "real_monitor.h"
#include "monitor_layout_cache.h"
#include "pipeline_stats.h"
#include "io.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int running;
    int client_fd;
    int control_fd;
    uint16_t transport;
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
    char session_id[VNC_BROKER_SESSION_ID_MAX];

    const RuntimeConfig *cfg;
    FrameBridge *frames;
    PipelineStats *pipeline_stats;
} ClientSlot;

typedef struct {
    int control_fd;
    int client_fd;
} ControlGuard;

typedef struct {
    int control_fd;
    VncBrokerHandoff handoff;
    const RuntimeConfig *cfg;
    ClientSlot *slot;
    FrameBridge *frames;
    PipelineStats *pipeline_stats;
} WebControlTask;

typedef struct {
    pthread_mutex_t mutex;
    int stop;
    int control_fd;
    int width;
    int height;
    FrameBridge *frames;
} WebMediaSender;

#define WEB_LEGACY_JPEG_QUALITY 65
#define WEB_LEGACY_MAX_FPS       8
#define WEB_HLS_TEST_FPS         15

static int
create_public_listener(const RuntimeConfig *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->public_port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
set_client_socket_option(int fd,
                         int level,
                         int option,
                         int value,
                         const char *name,
                         int required)
{
    if (setsockopt(fd, level, option, &value, sizeof(value)) == 0)
        return 0;

    if (required) {
        LOG_ERROR("Could not set required client %s=%d: %s",
                  name,
                  value,
                  strerror(errno));
    }
    else {
        LOG_DEBUG("Could not set client %s=%d: %s",
                  name,
                  value,
                  strerror(errno));
    }

    return -1;
}

static int
configure_external_socket(int client_fd, const RuntimeConfig *cfg)
{
    (void)set_client_socket_option(client_fd,
                                   SOL_SOCKET,
                                   SO_SNDBUF,
                                   cfg->external_send_buffer,
                                   "SO_SNDBUF",
                                   0);

    /*
     * Detect a vanished Wi-Fi/LAN peer even when the framebuffer is static.
     * This is deliberately TCP liveness, not an application-idle timeout:
     * healthy viewers may stay connected indefinitely without RFB activity.
     */
    if (set_client_socket_option(client_fd,
                                 SOL_SOCKET,
                                 SO_KEEPALIVE,
                                 1,
                                 "SO_KEEPALIVE",
                                 1) < 0 ||
        set_client_socket_option(client_fd,
                                 IPPROTO_TCP,
                                 TCP_KEEPIDLE,
                                 cfg->client_keepalive_idle_s,
                                 "TCP_KEEPIDLE",
                                 1) < 0 ||
        set_client_socket_option(client_fd,
                                 IPPROTO_TCP,
                                 TCP_KEEPINTVL,
                                 cfg->client_keepalive_interval_s,
                                 "TCP_KEEPINTVL",
                                 1) < 0 ||
        set_client_socket_option(client_fd,
                                 IPPROTO_TCP,
                                 TCP_KEEPCNT,
                                 cfg->client_keepalive_probes,
                                 "TCP_KEEPCNT",
                                 1) < 0) {
        return -1;
    }

#ifdef TCP_USER_TIMEOUT
    if (set_client_socket_option(client_fd,
                                 IPPROTO_TCP,
                                 TCP_USER_TIMEOUT,
                                 cfg->client_user_timeout_ms,
                                 "TCP_USER_TIMEOUT",
                                 1) < 0) {
        return -1;
    }
#endif

    if (vnc_log_enabled(VNC_LOG_DEBUG)) {
        int actual = 0;
        socklen_t actual_len = sizeof(actual);
        if (getsockopt(client_fd,
                       SOL_SOCKET,
                       SO_SNDBUF,
                       &actual,
                       &actual_len) == 0) {
            LOG_DEBUG("External TCP send buffer: requested=%d actual=%d bytes",
                      cfg->external_send_buffer,
                      actual);
        }

        LOG_DEBUG("Client liveness: keepalive idle=%ds interval=%ds probes=%d user-timeout=%dms handshake-deadline=%dms",
                  cfg->client_keepalive_idle_s,
                  cfg->client_keepalive_interval_s,
                  cfg->client_keepalive_probes,
                  cfg->client_user_timeout_ms,
                  cfg->client_handshake_timeout_ms);
    }

    return 0;
}

static void
reject_additional_client(int client_fd, const char *peer_addr)
{
    /* RST makes a second viewer fail immediately instead of waiting in EOF. */
    struct linger reset = {
        .l_onoff = 1,
        .l_linger = 0
    };
    (void)setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));

    LOG_INFO("Rejected additional client: %s (strong single-connect policy)",
             peer_addr ? peer_addr : "unknown");
    close(client_fd);
}

static void
serve_client(int client_fd,
             const RuntimeConfig *base_cfg,
             FrameBridge *frames,
             PipelineStats *pipeline_stats)
{
    /* Client-driven resize is session-local; configured fallback stays intact. */
    RuntimeConfig cfg = *base_cfg;
    Ra2Session session;

    /*
     * A connected, stalled or deliberately slow peer must not monopolize the
     * only client slot. One monotonic deadline covers all io_* waits during
     * the external RA2 negotiation and the auth-helper request/response. It is
     * cleared completely before the long-lived RFB session begins.
     */
    if (io_deadline_set_ms(cfg.client_handshake_timeout_ms) < 0) {
        LOG_ERROR("Could not start RA2 handshake deadline: %s", strerror(errno));
        return;
    }

    int handshake_rc = ra2_server_handshake(client_fd, &session, &cfg);
    io_deadline_clear();

    if (handshake_rc < 0) {
        LOG_ERROR("RA2r handshake failed or exceeded %d ms I/O deadline",
                  cfg.client_handshake_timeout_ms);
        return;
    }

    if (frame_bridge_resize(frames, cfg.width, cfg.height) < 0) {
        LOG_ERROR("Could not prepare FrameBridge at %dx%d", cfg.width, cfg.height);
        ra2_session_clear(&session);
        return;
    }

    RealMonitor real = {0};
    MonitorLayoutCache layout_cache = {0};
    RfbBackend backend = {0};
    int backend_started = 0;

    frame_bridge_clear(frames);

    if (monitor_layout_cache_prepare(&layout_cache, &cfg) < 0) {
        LOG_DEBUG("Monitor-layout cache preparation failed; continuing without cached layout");
    }

    if (real_monitor_start(&real, &cfg, frames, pipeline_stats) < 0) {
        LOG_ERROR("Virtual monitor/capture source failed; closing client");
        monitor_layout_cache_clear(&layout_cache);
        ra2_session_clear(&session);
        return;
    }

    if (monitor_layout_cache_apply(&layout_cache,
                                   &cfg,
                                   cfg.capture_timeout_ms) < 0) {
        LOG_DEBUG("Cached monitor layout could not be applied; using Mutter's current layout");
    }

    if (vnc_log_enabled(VNC_LOG_DEBUG))
        (void)monitor_layout_log_matching_modes(&layout_cache, &cfg);

    if (rfb_backend_start(&backend,
                          &cfg,
                          frames,
                          pipeline_stats,
                          &real,
                          &layout_cache) < 0) {
        LOG_ERROR("Failed to start per-session LibVNCServer backend");
        goto out;
    }

    backend_started = 1;

    (void)rfb_proxy_run(client_fd,
                        &session,
                        &cfg,
                        pipeline_stats);

out:
    /* Stop the backend first: its SetDesktopSize hook references real/layout. */
    if (backend_started)
        rfb_backend_stop(&backend);

    if (monitor_layout_cache_save(&layout_cache, &cfg) < 0)
        LOG_DEBUG("Monitor layout was not saved");

    real_monitor_stop(&real);
    frame_bridge_clear(frames);
    monitor_layout_cache_clear(&layout_cache);
    ra2_session_clear(&session);
}

/*
 * The broker keeps the control channel open for the complete VNC session.
 * If the broker crashes/restarts, EOF on that channel must revoke the handed
 * off TCP connection as well; otherwise the agent could outlive the system
 * policy process and continue showing a session without active-seat checks.
 */
static void *
control_guard_worker(void *arg)
{
    ControlGuard *guard = arg;
    unsigned char byte = 0;

    ssize_t n;
    do {
        n = recv(guard->control_fd, &byte, sizeof(byte), 0);
    } while (n < 0 && errno == EINTR);

    (void)n;
    (void)shutdown(guard->client_fd, SHUT_RDWR);
    close(guard->client_fd);
    close(guard->control_fd);
    free(guard);
    return NULL;
}

static int
start_control_guard(int control_fd, int client_fd)
{
    if (control_fd < 0)
        return 0;

    ControlGuard *guard = calloc(1, sizeof(*guard));
    if (!guard)
        return -1;

    guard->control_fd = dup(control_fd);
    guard->client_fd = dup(client_fd);
    if (guard->control_fd < 0 || guard->client_fd < 0) {
        if (guard->control_fd >= 0)
            close(guard->control_fd);
        if (guard->client_fd >= 0)
            close(guard->client_fd);
        free(guard);
        return -1;
    }

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0)
        goto fail;

    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        goto fail;
    }

    pthread_t thread;
    rc = pthread_create(&thread, &attr, control_guard_worker, guard);
    pthread_attr_destroy(&attr);

    if (rc != 0)
        goto fail;

    return 0;

fail:
    close(guard->control_fd);
    close(guard->client_fd);
    free(guard);
    errno = rc;
    return -1;
}

static void *
client_worker(void *arg)
{
    ClientSlot *slot = arg;

    pthread_mutex_lock(&slot->mutex);
    int client_fd = slot->client_fd;
    int control_fd = slot->control_fd;
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    snprintf(peer_addr, sizeof(peer_addr), "%s", slot->peer_addr);
    snprintf(session_id, sizeof(session_id), "%s", slot->session_id);
    const RuntimeConfig *cfg = slot->cfg;
    FrameBridge *frames = slot->frames;
    PipelineStats *pipeline_stats = slot->pipeline_stats;
    pthread_mutex_unlock(&slot->mutex);

    if (control_fd >= 0) {
        LOG_INFO("Client connected through broker: %s logind-session=%s",
                 peer_addr,
                 session_id);

        if (start_control_guard(control_fd, client_fd) < 0) {
            LOG_ERROR("Could not start broker control guard: %s", strerror(errno));
            (void)vnc_broker_send_status(control_fd, VNC_BROKER_STATUS_REJECT);
            goto done;
        }
    }
    else {
        LOG_INFO("Client connected: %s", peer_addr);
    }

    serve_client(client_fd, cfg, frames, pipeline_stats);

done:
    /*
     * The broker owns another descriptor for this same TCP socket. shutdown()
     * is therefore required before close() so the network session actually
     * terminates instead of surviving through the broker's duplicate.
     */
    (void)shutdown(client_fd, SHUT_RDWR);
    shutdown_signal_unregister_fd(client_fd);
    close(client_fd);

    if (control_fd >= 0) {
        (void)vnc_broker_send_status(control_fd, VNC_BROKER_STATUS_DONE);
        (void)shutdown(control_fd, SHUT_RDWR);
        close(control_fd);
    }

    LOG_INFO("Client disconnected: %s", peer_addr);

    pthread_mutex_lock(&slot->mutex);
    slot->client_fd = -1;
    slot->control_fd = -1;
    slot->transport = VNC_BROKER_TRANSPORT_LEGACY_VNC;
    slot->session_id[0] = '\0';
    slot->running = 0;
    pthread_cond_broadcast(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
    return NULL;
}

static void
release_unstarted_client_slot(ClientSlot *slot, int client_fd)
{
    shutdown_signal_unregister_fd(client_fd);

    pthread_mutex_lock(&slot->mutex);
    slot->running = 0;
    slot->client_fd = -1;
    slot->control_fd = -1;
    slot->transport = VNC_BROKER_TRANSPORT_LEGACY_VNC;
    slot->session_id[0] = '\0';
    pthread_cond_broadcast(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
}

static int
start_client_worker(ClientSlot *slot,
                    int client_fd,
                    int control_fd,
                    const char *peer_addr,
                    const char *session_id,
                    const RuntimeConfig *cfg,
                    FrameBridge *frames,
                    PipelineStats *pipeline_stats)
{
    pthread_mutex_lock(&slot->mutex);

    if (slot->running) {
        pthread_mutex_unlock(&slot->mutex);
        return 1;
    }

    slot->running = 1;
    slot->client_fd = client_fd;
    slot->control_fd = control_fd;
    slot->transport = VNC_BROKER_TRANSPORT_VNC;
    slot->cfg = cfg;
    slot->frames = frames;
    slot->pipeline_stats = pipeline_stats;
    snprintf(slot->peer_addr,
             sizeof(slot->peer_addr),
             "%s",
             peer_addr ? peer_addr : "unknown");
    snprintf(slot->session_id,
             sizeof(slot->session_id),
             "%s",
             session_id ? session_id : "");

    pthread_mutex_unlock(&slot->mutex);

    if (shutdown_signal_register_fd(client_fd) < 0)
        LOG_DEBUG("Could not register client socket for shutdown");

    if (configure_external_socket(client_fd, cfg) < 0) {
        release_unstarted_client_slot(slot, client_fd);
        errno = EIO;
        return -1;
    }

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        release_unstarted_client_slot(slot, client_fd);
        errno = rc;
        return -1;
    }

    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        release_unstarted_client_slot(slot, client_fd);
        errno = rc;
        return -1;
    }

    pthread_t thread;
    rc = pthread_create(&thread, &attr, client_worker, slot);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        release_unstarted_client_slot(slot, client_fd);
        errno = rc;
        return -1;
    }

    return 0;
}

static int
claim_web_control_slot(ClientSlot *slot,
                       int control_fd,
                       const char *peer_addr,
                       const char *session_id,
                       const RuntimeConfig *cfg)
{
    pthread_mutex_lock(&slot->mutex);

    if (slot->running) {
        pthread_mutex_unlock(&slot->mutex);
        return 1;
    }

    slot->running = 1;
    slot->client_fd = -1;
    slot->control_fd = control_fd;
    slot->transport = VNC_BROKER_TRANSPORT_WEBRTC;
    slot->cfg = cfg;
    snprintf(slot->peer_addr,
             sizeof(slot->peer_addr),
             "%s",
             peer_addr ? peer_addr : "unknown");
    snprintf(slot->session_id,
             sizeof(slot->session_id),
             "%s",
             session_id ? session_id : "");

    pthread_mutex_unlock(&slot->mutex);

    if (shutdown_signal_register_fd(control_fd) < 0) {
        pthread_mutex_lock(&slot->mutex);
        slot->running = 0;
        slot->control_fd = -1;
        slot->transport = VNC_BROKER_TRANSPORT_LEGACY_VNC;
        slot->session_id[0] = '\0';
        pthread_cond_broadcast(&slot->cond);
        pthread_mutex_unlock(&slot->mutex);
        return -1;
    }

    return 0;
}

static void
release_web_control_slot(ClientSlot *slot, int control_fd)
{
    pthread_mutex_lock(&slot->mutex);
    if (slot->control_fd == control_fd) {
        slot->client_fd = -1;
        slot->control_fd = -1;
        slot->transport = VNC_BROKER_TRANSPORT_LEGACY_VNC;
        slot->session_id[0] = '\0';
        slot->running = 0;
        pthread_cond_broadcast(&slot->cond);
    }
    pthread_mutex_unlock(&slot->mutex);
}

static int
web_media_sender_should_stop(WebMediaSender *sender)
{
    int stop = 1;
    if (!sender)
        return stop;

    pthread_mutex_lock(&sender->mutex);
    stop = sender->stop;
    pthread_mutex_unlock(&sender->mutex);
    return stop;
}

static void
web_media_sender_stop(WebMediaSender *sender)
{
    if (!sender)
        return;

    pthread_mutex_lock(&sender->mutex);
    sender->stop = 1;
    pthread_mutex_unlock(&sender->mutex);
    frame_bridge_wake_all(sender->frames);
}

static uint64_t
web_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000u +
           (uint64_t)ts.tv_nsec / 1000000u;
}

static void *
web_media_sender_worker(void *opaque)
{
    WebMediaSender *sender = opaque;
    size_t frame_bytes =
        (size_t)sender->width * (size_t)sender->height * 4u;
    uint8_t *pixels = malloc(frame_bytes);
    if (!pixels) {
        (void)shutdown(sender->control_fd, SHUT_RDWR);
        return NULL;
    }

    uint64_t last_sequence = 0;
    uint64_t last_sent_ms = 0;
    uint64_t frames_sent = 0;
    const uint64_t min_interval_ms = 1000u / WEB_LEGACY_MAX_FPS;

    while (!web_media_sender_should_stop(sender)) {
        uint64_t current_sequence = 0;
        int wait_rc = frame_bridge_wait_for_change(sender->frames,
                                                   last_sequence,
                                                   100,
                                                   &current_sequence);
        if (wait_rc < 0)
            break;
        if (wait_rc == 0)
            continue;

        uint64_t now_ms = web_monotonic_ms();
        if (last_sent_ms != 0 && now_ms > last_sent_ms &&
            now_ms - last_sent_ms < min_interval_ms) {
            uint64_t remaining = min_interval_ms - (now_ms - last_sent_ms);
            struct timespec delay = {
                .tv_sec = (time_t)(remaining / 1000u),
                .tv_nsec = (long)(remaining % 1000u) * 1000000L
            };

            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
                if (web_media_sender_should_stop(sender))
                    break;
            }
        }

        if (web_media_sender_should_stop(sender))
            break;

        if (frame_bridge_consume(sender->frames,
                                 pixels,
                                 &last_sequence) <= 0)
            continue;

        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0;
        if (web_jpeg_encode_bgrx(pixels,
                                 sender->width,
                                 sender->height,
                                 WEB_LEGACY_JPEG_QUALITY,
                                 &jpeg,
                                 &jpeg_size) < 0) {
            LOG_ERROR("Legacy browser JPEG encode failed");
            (void)shutdown(sender->control_fd, SHUT_RDWR);
            break;
        }

        if (jpeg_size > VNC_BROKER_VIDEO_FRAME_MAX ||
            vnc_broker_send_video_frame(sender->control_fd,
                                        (uint32_t)sender->width,
                                        (uint32_t)sender->height,
                                        jpeg,
                                        jpeg_size) < 0) {
            LOG_INFO("Legacy browser media channel ended while sending frame: %s",
                     strerror(errno));
            free(jpeg);
            (void)shutdown(sender->control_fd, SHUT_RDWR);
            break;
        }

        free(jpeg);
        last_sent_ms = web_monotonic_ms();
        frames_sent++;

        if (frames_sent == 1) {
            LOG_INFO("Legacy browser media sent first JPEG frame: %dx%d quality=%d",
                     sender->width,
                     sender->height,
                     WEB_LEGACY_JPEG_QUALITY);
        }
    }

    free(pixels);
    return NULL;
}

static int
serve_web_media_lifetime(int control_fd,
                         const RuntimeConfig *cfg,
                         FrameBridge *frames,
                         PipelineStats *pipeline_stats)
{
    if (cfg->width <= 0 || cfg->height <= 0 ||
        (uint32_t)cfg->width > VNC_BROKER_VIDEO_DIMENSION_MAX ||
        (uint32_t)cfg->height > VNC_BROKER_VIDEO_DIMENSION_MAX) {
        LOG_ERROR("Legacy browser media size %dx%d exceeds safe limit %u",
                  cfg->width,
                  cfg->height,
                  VNC_BROKER_VIDEO_DIMENSION_MAX);
        errno = EINVAL;
        return -1;
    }

    RealMonitor real;
    memset(&real, 0, sizeof(real));

    WebMediaSender sender;
    memset(&sender, 0, sizeof(sender));
    sender.control_fd = control_fd;
    sender.width = cfg->width;
    sender.height = cfg->height;
    sender.frames = frames;

    WebHlsStream hls;
    memset(&hls, 0, sizeof(hls));

    int mutex_rc = pthread_mutex_init(&sender.mutex, NULL);
    if (mutex_rc != 0) {
        errno = mutex_rc;
        return -1;
    }

    pthread_t sender_thread;
    int sender_started = 0;
    int hls_started = 0;
    int media_started = 0;
    int result = 0;

    for (;;) {
        uint8_t payload[VNC_BROKER_CONTROL_PAYLOAD_MAX];
        VncBrokerControlType type;
        size_t payload_len = 0;

        if (vnc_broker_recv_control(control_fd,
                                    &type,
                                    payload,
                                    sizeof(payload),
                                    &payload_len) < 0)
            break;

        if (type == VNC_BROKER_CONTROL_REVOKE && payload_len == 0)
            break;

        if (type == VNC_BROKER_CONTROL_MEDIA_START && payload_len == 0) {
            if (media_started) {
                LOG_ERROR("Duplicate legacy browser media start request");
                result = -1;
                break;
            }

            if (frame_bridge_resize(frames, cfg->width, cfg->height) < 0) {
                LOG_ERROR("Could not prepare browser FrameBridge at %dx%d",
                          cfg->width, cfg->height);
                result = -1;
                break;
            }

            frame_bridge_clear(frames);

            if (real_monitor_start(&real, cfg, frames, pipeline_stats) < 0) {
                LOG_ERROR("Could not start legacy browser virtual monitor/capture");
                result = -1;
                break;
            }

            media_started = 1;

            if (web_hls_start(&hls,
                              control_fd,
                              frames,
                              cfg->width,
                              cfg->height,
                              WEB_HLS_TEST_FPS) == 0) {
                hls_started = 1;
                LOG_INFO("Legacy browser media selected HLS/H.264 test transport");
                continue;
            }

            LOG_INFO("Legacy browser HLS test path unavailable; falling back to WSS/JPEG");

            int rc = pthread_create(&sender_thread, NULL,
                                    web_media_sender_worker, &sender);
            if (rc != 0) {
                LOG_ERROR("Could not start legacy browser media sender: %s",
                          strerror(rc));
                result = -1;
                break;
            }

            sender_started = 1;
            LOG_INFO("Legacy browser WSS/JPEG fallback active: %dx%d max-fps=%d quality=%d",
                     cfg->width, cfg->height,
                     WEB_LEGACY_MAX_FPS, WEB_LEGACY_JPEG_QUALITY);
            continue;
        }

        /* SDP/ICE will be handled here when the modern WebRTC backend lands. */
        LOG_DEBUG("Ignoring unsupported browser control message type=%u payload=%zu",
                  (unsigned)type, payload_len);
    }

    if (hls_started)
        web_hls_stop(&hls);

    if (sender_started) {
        web_media_sender_stop(&sender);
        (void)pthread_join(sender_thread, NULL);
    }

    if (media_started) {
        real_monitor_stop(&real);
        frame_bridge_clear(frames);
    }

    pthread_mutex_destroy(&sender.mutex);
    return result;
}

static VncBrokerWebAuthResult
authenticate_control_request(int control_fd,
                             const RuntimeConfig *cfg,
                             const char *purpose)
{
    VncBrokerWebAuthRequest request;
    memset(&request, 0, sizeof(request));

    VncBrokerWebAuthResult result = VNC_BROKER_WEB_AUTH_ERROR;

    if (vnc_broker_recv_web_auth_request(control_fd, &request) < 0) {
        LOG_ERROR("Invalid %s authentication request from broker: %s",
                  purpose,
                  strerror(errno));
        goto done;
    }

    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_name) {
        LOG_ERROR("Cannot resolve local Unix account for %s authentication",
                  purpose);
        goto done;
    }

    if (strcmp(request.username, pw->pw_name) != 0) {
        LOG_INFO("Rejected %s authentication for user '%s': agent belongs to '%s'",
                 purpose,
                 request.username,
                 pw->pw_name);
        result = VNC_BROKER_WEB_AUTH_DENIED;
        goto done;
    }

    if (io_deadline_set_ms(cfg->client_handshake_timeout_ms) < 0) {
        LOG_ERROR("Could not start %s PAM deadline: %s",
                  purpose,
                  strerror(errno));
        goto done;
    }

    int auth_rc = auth_client_check(cfg->auth_socket,
                                    request.username,
                                    request.password);
    io_deadline_clear();

    if (auth_rc == 1)
        result = VNC_BROKER_WEB_AUTH_OK;
    else if (auth_rc == 0)
        result = VNC_BROKER_WEB_AUTH_DENIED;

done:
    vnc_broker_web_auth_request_clear(&request);
    return result;
}

static void
serve_web_control_session(int control_fd,
                          const VncBrokerHandoff *handoff,
                          const RuntimeConfig *cfg,
                          ClientSlot *slot,
                          FrameBridge *frames,
                          PipelineStats *pipeline_stats)
{
    VncBrokerWebAuthResult result =
        authenticate_control_request(control_fd, cfg, "WebRTC");

    if (vnc_broker_send_web_auth_result(control_fd, result) < 0) {
        LOG_DEBUG("Could not return WebRTC authentication result to broker: %s",
                  strerror(errno));
        result = VNC_BROKER_WEB_AUTH_ERROR;
    }

    if (result == VNC_BROKER_WEB_AUTH_OK) {
        LOG_INFO("WebRTC browser authenticated for active user; peer=%s logind-session=%s",
                 handoff->peer_addr,
                 handoff->session_id);

        /*
         * Keep the exact broker control channel as the lifetime authority.
         * Media starts only after the authenticated WSS bind. Closing or
         * revoking this channel tears down capture and the virtual monitor.
         */
        if (serve_web_media_lifetime(control_fd,
                                     cfg,
                                     frames,
                                     pipeline_stats) < 0)
            LOG_INFO("Legacy browser media session ended with an internal error");
    }

    /*
     * Unregister before close so a rapidly reused descriptor can never be
     * mistaken for the old WebRTC lifetime guard by the shutdown supervisor.
     */
    shutdown_signal_unregister_fd(control_fd);
    (void)shutdown(control_fd, SHUT_RDWR);
    close(control_fd);
    release_web_control_slot(slot, control_fd);

    LOG_INFO("WebRTC control session ended: %s", handoff->peer_addr);
}

static void *
web_control_worker(void *opaque)
{
    WebControlTask *task = opaque;
    serve_web_control_session(task->control_fd,
                              &task->handoff,
                              task->cfg,
                              task->slot,
                              task->frames,
                              task->pipeline_stats);
    free(task);
    return NULL;
}

static int
start_web_control_worker(ClientSlot *slot,
                         int control_fd,
                         const VncBrokerHandoff *handoff,
                         const RuntimeConfig *cfg,
                         FrameBridge *frames,
                         PipelineStats *pipeline_stats)
{
    int claim_rc = claim_web_control_slot(slot,
                                          control_fd,
                                          handoff->peer_addr,
                                          handoff->session_id,
                                          cfg);
    if (claim_rc != 0)
        return claim_rc;

    WebControlTask *task = calloc(1, sizeof(*task));
    if (!task) {
        shutdown_signal_unregister_fd(control_fd);
        release_web_control_slot(slot, control_fd);
        errno = ENOMEM;
        return -1;
    }

    task->control_fd = control_fd;
    task->handoff = *handoff;
    task->cfg = cfg;
    task->slot = slot;
    task->frames = frames;
    task->pipeline_stats = pipeline_stats;

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        free(task);
        shutdown_signal_unregister_fd(control_fd);
        release_web_control_slot(slot, control_fd);
        errno = rc;
        return -1;
    }

    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (rc == 0)
        rc = pthread_create(&thread, &attr, web_control_worker, task);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(task);
        shutdown_signal_unregister_fd(control_fd);
        release_web_control_slot(slot, control_fd);
        errno = rc;
        return -1;
    }

    return 0;
}

static void
serve_management_auth_session(int control_fd,
                               const VncBrokerHandoff *handoff,
                               const RuntimeConfig *cfg)
{
    if (shutdown_signal_register_fd(control_fd) < 0)
        LOG_DEBUG("Could not register management control channel for shutdown");

    VncBrokerWebAuthResult result =
        authenticate_control_request(control_fd, cfg, "management");

    if (vnc_broker_send_web_auth_result(control_fd, result) < 0)
        LOG_DEBUG("Could not return management authentication result to broker: %s",
                  strerror(errno));

    if (result == VNC_BROKER_WEB_AUTH_OK) {
        LOG_INFO("Management browser authenticated for active user; peer=%s logind-session=%s",
                 handoff->peer_addr,
                 handoff->session_id);
    }

    shutdown_signal_unregister_fd(control_fd);
    (void)shutdown(control_fd, SHUT_RDWR);
    close(control_fd);
}

static void
wait_for_client_slot(ClientSlot *slot)
{
    pthread_mutex_lock(&slot->mutex);

    if (slot->running) {
        if (slot->client_fd >= 0)
            (void)shutdown(slot->client_fd, SHUT_RDWR);
        if (slot->control_fd >= 0)
            (void)shutdown(slot->control_fd, SHUT_RDWR);
    }

    while (slot->running)
        pthread_cond_wait(&slot->cond, &slot->mutex);

    pthread_mutex_unlock(&slot->mutex);
}

static int
run_standalone_listener(const RuntimeConfig *cfg,
                        FrameBridge *frames,
                        PipelineStats *pipeline_stats,
                        ClientSlot *client_slot)
{
    int listen_fd = create_public_listener(cfg);
    if (listen_fd < 0) {
        LOG_ERROR("Could not create public listener on TCP/%d: %s",
                  cfg->public_port,
                  strerror(errno));
        return -1;
    }

    if (shutdown_signal_register_fd(listen_fd) < 0)
        LOG_DEBUG("Could not register public listener for shutdown");

    LOG_INFO("VNC Monitor %s standalone ready on TCP/%d (strong single-connect; screen=%s fallback=%dx%d)",
             VNC_MONITOR_VERSION,
             cfg->public_port,
             runtime_config_screen_size_mode_name(cfg->screen_size_mode),
             cfg->width,
             cfg->height);

    while (!shutdown_signal_requested()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        int stop_fd = shutdown_signal_fd();
        if (stop_fd >= 0)
            FD_SET(stop_fd, &readfds);

        int maxfd = listen_fd > stop_fd ? listen_fd : stop_fd;

        int sel = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) {
                if (shutdown_signal_requested())
                    break;
                continue;
            }

            LOG_ERROR("Listener select failed: %s", strerror(errno));
            break;
        }

        if (stop_fd >= 0 && FD_ISSET(stop_fd, &readfds)) {
            shutdown_signal_drain();
            break;
        }

        if (!FD_ISSET(listen_fd, &readfds))
            continue;

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept4(listen_fd,
                                (struct sockaddr *)&peer,
                                &peer_len,
                                SOCK_CLOEXEC);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            LOG_ERROR("accept failed: %s", strerror(errno));
            continue;
        }

        char peer_addr[VNC_BROKER_PEER_ADDR_MAX] = "unknown";
        (void)inet_ntop(AF_INET,
                        &peer.sin_addr,
                        peer_addr,
                        sizeof(peer_addr));

        int start_rc = start_client_worker(client_slot,
                                           client_fd,
                                           -1,
                                           peer_addr,
                                           NULL,
                                           cfg,
                                           frames,
                                           pipeline_stats);

        if (start_rc > 0) {
            reject_additional_client(client_fd, peer_addr);
            continue;
        }

        if (start_rc < 0) {
            LOG_ERROR("Could not start protected client session for %s: %s",
                      peer_addr,
                      strerror(errno));
            close(client_fd);
        }
    }

    shutdown_signal_unregister_fd(listen_fd);
    close(listen_fd);
    return 0;
}

static int
create_agent_listener(char *socket_path, size_t socket_path_size)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];

    if (!runtime_dir || !*runtime_dir) {
        snprintf(fallback,
                 sizeof(fallback),
                 "/run/user/%lu",
                 (unsigned long)getuid());
        runtime_dir = fallback;
    }

    char agent_dir[256];
    int n = snprintf(agent_dir,
                     sizeof(agent_dir),
                     "%s/vnc-monitor",
                     runtime_dir);
    if (n < 0 || (size_t)n >= sizeof(agent_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mkdir(agent_dir, 0700) < 0 && errno != EEXIST)
        return -1;
    if (chmod(agent_dir, 0700) < 0)
        return -1;

    n = snprintf(socket_path,
                 socket_path_size,
                 "%s/agent.sock",
                 agent_dir);
    if (n < 0 || (size_t)n >= socket_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    (void)unlink(socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        chmod(socket_path, 0600) < 0 ||
        listen(fd, 8) < 0) {
        int saved = errno;
        close(fd);
        (void)unlink(socket_path);
        errno = saved;
        return -1;
    }

    return fd;
}

static int
handle_broker_handoff(int control_fd,
                      const RuntimeConfig *cfg,
                      FrameBridge *frames,
                      PipelineStats *pipeline_stats,
                      ClientSlot *client_slot)
{
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    if (getsockopt(control_fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &peer,
                   &peer_len) < 0 ||
        peer.uid != 0) {
        LOG_ERROR("Rejected agent control connection: broker peer is not root");
        return -1;
    }

    int client_fd = -1;
    VncBrokerHandoff handoff;

    if (vnc_broker_recv_handoff(control_fd, &client_fd, &handoff) < 0) {
        LOG_ERROR("Invalid broker handoff: %s", strerror(errno));
        return -1;
    }

    if ((uid_t)handoff.uid != getuid()) {
        LOG_ERROR("Rejected broker handoff for uid=%lu; agent uid=%lu",
                  (unsigned long)handoff.uid,
                  (unsigned long)getuid());
        if (handoff.transport == VNC_BROKER_TRANSPORT_WEBRTC ||
            handoff.transport == VNC_BROKER_TRANSPORT_MANAGEMENT)
            (void)vnc_broker_send_web_auth_result(control_fd,
                                                  VNC_BROKER_WEB_AUTH_ERROR);
        else
            (void)vnc_broker_send_status(control_fd, VNC_BROKER_STATUS_REJECT);
        if (client_fd >= 0)
            close(client_fd);
        return -1;
    }

    if (handoff.transport == VNC_BROKER_TRANSPORT_WEBRTC) {
        if (client_fd >= 0) {
            LOG_ERROR("Rejected WebRTC broker handoff carrying an unexpected client fd");
            close(client_fd);
            return -1;
        }

        int web_rc = start_web_control_worker(client_slot,
                                              control_fd,
                                              &handoff,
                                              cfg,
                                              frames,
                                              pipeline_stats);
        if (web_rc != 0) {
            if (web_rc > 0)
                LOG_INFO("Agent rejected WebRTC handoff for %s: local session already busy",
                         handoff.peer_addr);
            else
                LOG_ERROR("Agent could not start WebRTC control worker for %s: %s",
                          handoff.peer_addr,
                          strerror(errno));

            (void)vnc_broker_send_web_auth_result(control_fd,
                                                  VNC_BROKER_WEB_AUTH_ERROR);
            return -1;
        }

        /* Detached WebRTC worker owns control_fd. */
        return 1;
    }

    if (handoff.transport == VNC_BROKER_TRANSPORT_MANAGEMENT) {
        if (client_fd >= 0) {
            LOG_ERROR("Rejected management handoff carrying an unexpected client fd");
            close(client_fd);
            return -1;
        }

        /* Short-lived PAM control request owns/closes control_fd here. */
        serve_management_auth_session(control_fd, &handoff, cfg);
        return 2;
    }

    int start_rc = start_client_worker(client_slot,
                                       client_fd,
                                       control_fd,
                                       handoff.peer_addr,
                                       handoff.session_id,
                                       cfg,
                                       frames,
                                       pipeline_stats);

    if (start_rc > 0) {
        LOG_INFO("Agent rejected broker handoff for %s: local session already busy",
                 handoff.peer_addr);
        (void)vnc_broker_send_status(control_fd, VNC_BROKER_STATUS_BUSY);
        close(client_fd);
        return -1;
    }

    if (start_rc < 0) {
        LOG_ERROR("Agent could not start handed-off client %s: %s",
                  handoff.peer_addr,
                  strerror(errno));
        (void)vnc_broker_send_status(control_fd, VNC_BROKER_STATUS_REJECT);
        close(client_fd);
        return -1;
    }

    /* Worker owns both descriptors after a successful start. */
    return 1;
}

static int
run_agent_listener(const RuntimeConfig *cfg,
                   FrameBridge *frames,
                   PipelineStats *pipeline_stats,
                   ClientSlot *client_slot)
{
    char socket_path[256];
    int listen_fd = create_agent_listener(socket_path, sizeof(socket_path));
    if (listen_fd < 0) {
        LOG_ERROR("Could not create broker agent socket: %s", strerror(errno));
        return -1;
    }

    if (shutdown_signal_register_fd(listen_fd) < 0)
        LOG_DEBUG("Could not register agent listener for shutdown");

    LOG_INFO("VNC Monitor %s agent ready at %s; public VNC/HTTPS listeners are owned by system broker",
             VNC_MONITOR_VERSION,
             socket_path);

    while (!shutdown_signal_requested()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        int stop_fd = shutdown_signal_fd();
        if (stop_fd >= 0)
            FD_SET(stop_fd, &readfds);

        int maxfd = listen_fd > stop_fd ? listen_fd : stop_fd;

        int sel = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) {
                if (shutdown_signal_requested())
                    break;
                continue;
            }

            LOG_ERROR("Agent listener select failed: %s", strerror(errno));
            break;
        }

        if (stop_fd >= 0 && FD_ISSET(stop_fd, &readfds)) {
            shutdown_signal_drain();
            break;
        }

        if (!FD_ISSET(listen_fd, &readfds))
            continue;

        int control_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (control_fd < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR("Agent accept failed: %s", strerror(errno));
            continue;
        }

        int handoff_rc = handle_broker_handoff(control_fd,
                                                cfg,
                                                frames,
                                                pipeline_stats,
                                                client_slot);

        if (handoff_rc <= 0)
            close(control_fd);
        /* handoff_rc==1: VNC/WebRTC worker owns fd; ==2: management handler closed it. */
    }

    shutdown_signal_unregister_fd(listen_fd);
    close(listen_fd);
    (void)unlink(socket_path);
    return 0;
}

static int
filter_internal_agent_option(int argc,
                             char **argv,
                             int *agent_mode,
                             int *filtered_argc,
                             char ***filtered_argv)
{
    char **copy = calloc((size_t)argc + 1, sizeof(*copy));
    if (!copy)
        return -1;

    int out = 0;
    copy[out++] = argv[0];
    *agent_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent") == 0) {
            *agent_mode = 1;
            continue;
        }
        copy[out++] = argv[i];
    }

    copy[out] = NULL;
    *filtered_argc = out;
    *filtered_argv = copy;
    return 0;
}

int
main(int argc, char **argv)
{
    int agent_mode = 0;
    int config_argc = 0;
    char **config_argv = NULL;

    if (filter_internal_agent_option(argc,
                                     argv,
                                     &agent_mode,
                                     &config_argc,
                                     &config_argv) < 0) {
        fprintf(stderr, "Could not allocate argument parser state\n");
        return 1;
    }

    RuntimeConfig cfg;
    runtime_config_defaults(&cfg);

    if (runtime_config_parse(&cfg, config_argc, config_argv) < 0) {
        free(config_argv);
        runtime_config_usage(argv[0]);
        return 2;
    }
    free(config_argv);

    vnc_log_set_level(cfg.verbose);

    if (vnc_log_enabled(VNC_LOG_DEBUG))
        runtime_config_print(&cfg);

    PipelineStats pipeline_stats;
    if (pipeline_stats_init(&pipeline_stats, &cfg) < 0) {
        LOG_ERROR("Failed to initialize pipeline statistics");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (shutdown_signal_init() < 0) {
        LOG_ERROR("Failed to initialize shutdown signal handling");
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    FrameBridge frames;
    if (frame_bridge_init(&frames, cfg.width, cfg.height) < 0) {
        LOG_ERROR("Failed to initialize framebuffer bridge");
        shutdown_signal_cleanup();
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    ClientSlot client_slot;
    memset(&client_slot, 0, sizeof(client_slot));
    client_slot.client_fd = -1;
    client_slot.control_fd = -1;
    client_slot.transport = VNC_BROKER_TRANSPORT_LEGACY_VNC;

    int mutex_rc = pthread_mutex_init(&client_slot.mutex, NULL);
    if (mutex_rc != 0) {
        LOG_ERROR("Failed to initialize single-client mutex: %s",
                  strerror(mutex_rc));
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    int cond_rc = pthread_cond_init(&client_slot.cond, NULL);
    if (cond_rc != 0) {
        LOG_ERROR("Failed to initialize single-client condition: %s",
                  strerror(cond_rc));
        pthread_mutex_destroy(&client_slot.mutex);
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    int run_rc;
    if (agent_mode) {
        run_rc = run_agent_listener(&cfg,
                                    &frames,
                                    &pipeline_stats,
                                    &client_slot);
    }
    else {
        run_rc = run_standalone_listener(&cfg,
                                         &frames,
                                         &pipeline_stats,
                                         &client_slot);
    }

    LOG_INFO("Stopping VNC Monitor%s", agent_mode ? " agent" : "");

    /* The shared framebuffer/stats outlive the only permitted client worker. */
    wait_for_client_slot(&client_slot);

    pthread_cond_destroy(&client_slot.cond);
    pthread_mutex_destroy(&client_slot.mutex);
    frame_bridge_destroy(&frames);
    shutdown_signal_cleanup();
    pipeline_stats_destroy(&pipeline_stats);

    LOG_INFO("VNC Monitor%s stopped", agent_mode ? " agent" : "");
    return run_rc < 0 ? 1 : 0;
}
