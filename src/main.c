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
#include "device_profile.h"
#include "pipeline_stats.h"
#include "io.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <inttypes.h>
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
    pthread_cond_t ack_cond;
    int stop;
    int frame_in_flight;
    uint64_t frames_acked;
    uint64_t ack_last_ms;
    double ack_ewma_ms;
    uint64_t adapt_slow_since_ms;
    uint64_t source_starved_until_ms;
    int target_fps;
    int control_fd;
    int width;
    int height;
    FrameBridge *frames;
} WebMediaSender;

#define WEB_LEGACY_JPEG_QUALITY          65
#define WEB_LEGACY_MIN_FPS                3
#define WEB_LEGACY_MAX_FPS                8
#define WEB_LEGACY_CAPTURE_MAX_FPS       12
#define WEB_LEGACY_ADAPT_STABLE_MS     5000u
#define WEB_LEGACY_SOURCE_STALL_MS       250u
#define WEB_LEGACY_SOURCE_HOLD_MS       3000u
#define WEB_LEGACY_TELEMETRY_MS         5000u
#define WEB_HLS_TEST_FPS                  15

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
    pthread_cond_broadcast(&sender->ack_cond);
    pthread_mutex_unlock(&sender->mutex);
    frame_bridge_wake_all(sender->frames);
}

static int
web_media_sender_wait_for_slot(WebMediaSender *sender)
{
    if (!sender)
        return 0;

    pthread_mutex_lock(&sender->mutex);
    while (!sender->stop && sender->frame_in_flight)
        pthread_cond_wait(&sender->ack_cond, &sender->mutex);

    int ready = !sender->stop;
    pthread_mutex_unlock(&sender->mutex);
    return ready;
}

static int
web_media_sender_begin_frame(WebMediaSender *sender)
{
    if (!sender)
        return 0;

    pthread_mutex_lock(&sender->mutex);
    if (sender->stop) {
        pthread_mutex_unlock(&sender->mutex);
        return 0;
    }

    sender->frame_in_flight = 1;
    pthread_mutex_unlock(&sender->mutex);
    return 1;
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

static int
web_media_sender_target_fps(WebMediaSender *sender)
{
    int fps = WEB_LEGACY_MAX_FPS;
    if (!sender)
        return fps;

    pthread_mutex_lock(&sender->mutex);
    if (sender->target_fps >= WEB_LEGACY_MIN_FPS &&
        sender->target_fps <= WEB_LEGACY_MAX_FPS)
        fps = sender->target_fps;
    pthread_mutex_unlock(&sender->mutex);
    return fps;
}

static void
web_media_sender_note_source_wait(WebMediaSender *sender,
                                  uint64_t wait_ms)
{
    if (!sender || wait_ms < WEB_LEGACY_SOURCE_STALL_MS)
        return;

    uint64_t now_ms = web_monotonic_ms();
    pthread_mutex_lock(&sender->mutex);
    sender->source_starved_until_ms = now_ms + WEB_LEGACY_SOURCE_HOLD_MS;
    sender->adapt_slow_since_ms = 0;
    pthread_mutex_unlock(&sender->mutex);
}

static void
web_media_sender_ack(WebMediaSender *sender)
{
    if (!sender)
        return;

    uint64_t now_ms = web_monotonic_ms();
    pthread_mutex_lock(&sender->mutex);
    if (sender->frame_in_flight) {
        sender->frame_in_flight = 0;
        sender->frames_acked++;

        if (sender->ack_last_ms != 0 && now_ms > sender->ack_last_ms) {
            double sample_ms = (double)(now_ms - sender->ack_last_ms);
            if (sender->ack_ewma_ms <= 0.0)
                sender->ack_ewma_ms = sample_ms;
            else
                sender->ack_ewma_ms +=
                    (sample_ms - sender->ack_ewma_ms) / 8.0;

            int source_starved =
                now_ms < sender->source_starved_until_ms;

            /*
             * ACK cadence is a WEB-only back-pressure signal. Never reduce
             * the sender because the source itself is idle/starved; that
             * would turn capture stalls or static-screen periods into a
             * self-reinforcing FPS reduction.
             */
            if (!source_starved &&
                sender->target_fps > WEB_LEGACY_MIN_FPS) {
                double budget_ms =
                    1000.0 / (double)sender->target_fps;
                if (sender->ack_ewma_ms > budget_ms * 1.35) {
                    if (sender->adapt_slow_since_ms == 0)
                        sender->adapt_slow_since_ms = now_ms;
                    else if (now_ms - sender->adapt_slow_since_ms >=
                             WEB_LEGACY_ADAPT_STABLE_MS) {
                        sender->target_fps--;
                        sender->adapt_slow_since_ms = 0;
                        LOG_INFO("[WEB][ADAPT] target-fps=%d reason=ack-bound ack-ewma=%.1fms",
                                 sender->target_fps,
                                 sender->ack_ewma_ms);
                    }
                }
                else {
                    sender->adapt_slow_since_ms = 0;
                }
            }
            else {
                sender->adapt_slow_since_ms = 0;
            }

            /* Recovery is intentionally faster than reduction. */
            if (!source_starved &&
                sender->target_fps < WEB_LEGACY_MAX_FPS) {
                double faster_budget_ms =
                    1000.0 / (double)(sender->target_fps + 1);
                if (sender->ack_ewma_ms <= faster_budget_ms * 1.05) {
                    sender->target_fps++;
                    sender->adapt_slow_since_ms = 0;
                    LOG_INFO("[WEB][ADAPT] target-fps=%d reason=ack-recovered ack-ewma=%.1fms",
                             sender->target_fps,
                             sender->ack_ewma_ms);
                }
            }
        }

        sender->ack_last_ms = now_ms;
        pthread_cond_signal(&sender->ack_cond);
    }
    pthread_mutex_unlock(&sender->mutex);
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
    uint64_t telemetry_started_ms = web_monotonic_ms();
    uint64_t telemetry_frames = 0;
    uint64_t telemetry_jpeg_bytes = 0;
    uint64_t telemetry_encode_ms = 0;
    uint64_t latest_source_wait_ms = 0;

    while (!web_media_sender_should_stop(sender)) {
        /*
         * At most one JPEG may exist beyond the agent. Once the browser ACKs
         * that frame, consume the then-current FrameBridge state. Intermediate
         * source changes are intentionally collapsed instead of queued.
         */
        if (!web_media_sender_wait_for_slot(sender))
            break;

        uint64_t current_sequence = 0;
        uint64_t source_wait_started_ms = web_monotonic_ms();
        int wait_rc = frame_bridge_wait_for_change(sender->frames,
                                                   last_sequence,
                                                   100,
                                                   &current_sequence);
        uint64_t source_wait_finished_ms = web_monotonic_ms();
        latest_source_wait_ms =
            source_wait_finished_ms >= source_wait_started_ms ?
                source_wait_finished_ms - source_wait_started_ms : 0;
        if (wait_rc < 0)
            break;
        if (wait_rc == 0)
            continue;

        web_media_sender_note_source_wait(sender, latest_source_wait_ms);

        int target_fps = web_media_sender_target_fps(sender);
        uint64_t min_interval_ms =
            1000u / (uint64_t)(target_fps > 0 ? target_fps : WEB_LEGACY_MAX_FPS);
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
        uint64_t encode_started_ms = web_monotonic_ms();
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

        if (frames_sent == 0) {
            int soi_ok = jpeg_size >= 2 &&
                         jpeg[0] == 0xffu &&
                         jpeg[1] == 0xd8u;
            int eoi_ok = jpeg_size >= 2 &&
                         jpeg[jpeg_size - 2] == 0xffu &&
                         jpeg[jpeg_size - 1] == 0xd9u;
            gchar *sha256 =
                g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            jpeg,
                                            (gsize)jpeg_size);

            LOG_INFO("Legacy browser first JPEG integrity: bytes=%zu soi=%s eoi=%s sha256=%s",
                     jpeg_size,
                     soi_ok ? "ok" : "bad",
                     eoi_ok ? "ok" : "bad",
                     sha256 ? sha256 : "unavailable");
            g_free(sha256);
        }

        if (!web_media_sender_begin_frame(sender)) {
            free(jpeg);
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

        uint64_t sent_ms = web_monotonic_ms();
        uint64_t encode_ms =
            sent_ms >= encode_started_ms ? sent_ms - encode_started_ms : 0;
        free(jpeg);
        last_sent_ms = sent_ms;
        frames_sent++;
        telemetry_frames++;
        telemetry_jpeg_bytes += (uint64_t)jpeg_size;
        telemetry_encode_ms += encode_ms;

        if (telemetry_started_ms == 0)
            telemetry_started_ms = sent_ms;
        if (sent_ms >= telemetry_started_ms &&
            sent_ms - telemetry_started_ms >= WEB_LEGACY_TELEMETRY_MS) {
            double ack_ewma_ms = 0.0;
            int source_starved = 0;
            int telemetry_target_fps = WEB_LEGACY_MAX_FPS;
            pthread_mutex_lock(&sender->mutex);
            ack_ewma_ms = sender->ack_ewma_ms;
            source_starved = sent_ms < sender->source_starved_until_ms;
            telemetry_target_fps = sender->target_fps;
            pthread_mutex_unlock(&sender->mutex);

            LOG_INFO("[WEB][PIPELINE] capture-cap=%d target-fps=%d sent=%" PRIu64
                     " encode-avg=%.1fms jpeg-avg=%" PRIu64
                     "B ack-ewma=%.1fms source-wait=%" PRIu64
                     "ms source-starved=%d latest-only=1",
                     WEB_LEGACY_CAPTURE_MAX_FPS,
                     telemetry_target_fps,
                     telemetry_frames,
                     telemetry_frames ?
                         (double)telemetry_encode_ms /
                             (double)telemetry_frames : 0.0,
                     telemetry_frames ?
                         telemetry_jpeg_bytes / telemetry_frames : 0,
                     ack_ewma_ms,
                     latest_source_wait_ms,
                     source_starved);

            telemetry_started_ms = sent_ms;
            telemetry_frames = 0;
            telemetry_jpeg_bytes = 0;
            telemetry_encode_ms = 0;
        }

        if (frames_sent == 1) {
            LOG_INFO("Legacy browser media sent first JPEG frame: %dx%d quality=%d; browser-ACK pacing active",
                     sender->width,
                     sender->height,
                     WEB_LEGACY_JPEG_QUALITY);
        }
    }

    free(pixels);
    return NULL;
}

static int
web_media_sender_start(WebMediaSender *sender,
                       pthread_t *thread,
                       int width,
                       int height)
{
    if (!sender || !thread)
        return -1;

    pthread_mutex_lock(&sender->mutex);
    sender->stop = 0;
    sender->frame_in_flight = 0;
    sender->frames_acked = 0;
    sender->ack_last_ms = 0;
    sender->ack_ewma_ms = 0.0;
    sender->adapt_slow_since_ms = 0;
    sender->source_starved_until_ms = 0;
    sender->target_fps = WEB_LEGACY_MAX_FPS;
    sender->width = width;
    sender->height = height;
    pthread_mutex_unlock(&sender->mutex);

    int rc = pthread_create(thread, NULL, web_media_sender_worker, sender);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    return 0;
}

static void
web_media_sender_stop_join(WebMediaSender *sender,
                           pthread_t thread,
                           int *started)
{
    if (!sender || !started || !*started)
        return;

    web_media_sender_stop(sender);
    (void)pthread_join(thread, NULL);
    *started = 0;
}

static int
web_device_layout_prepare(MonitorLayoutCache *cache,
                          const RuntimeConfig *cfg,
                          const DeviceProfile *profile,
                          VncBrokerDisplayMode mode,
                          VncBrokerDisplayOrientation orientation)
{
    if (!cache || !cfg || !profile)
        return -1;

    char scope[160];
    if (device_profile_layout_scope(
            profile,
            (DeviceDisplayMode)mode,
            (DeviceOrientation)orientation,
            scope,
            sizeof(scope)) < 0)
        return -1;

    return monitor_layout_cache_prepare_scoped(cache, cfg, scope);
}

static void
web_device_layout_seed_legacy(MonitorLayoutCache *target,
                              const RuntimeConfig *cfg)
{
    if (!target || !cfg || target->cache_existed)
        return;

    MonitorLayoutCache legacy = {0};
    if (monitor_layout_cache_prepare(&legacy, cfg) < 0)
        return;

    if (legacy.cache_existed &&
        monitor_layout_cache_file_has_virtual(&legacy) &&
        monitor_layout_cache_seed_from(target, legacy.cache_path) == 0) {
        LOG_INFO("Browser device layout inherited existing VNC layout: %dx%d",
                 cfg->width,
                 cfg->height);
    }

    monitor_layout_cache_clear(&legacy);
}

static int
web_send_display_rejected(int control_fd,
                          uint32_t generation,
                          VncBrokerDisplayMode mode,
                          VncBrokerDisplayOrientation orientation,
                          const RuntimeConfig *cfg)
{
    if (!cfg || generation == 0)
        return -1;

    VncBrokerDisplayState rejected = {
        .generation = generation,
        .width = (uint32_t)cfg->width,
        .height = (uint32_t)cfg->height,
        .mode = mode,
        .orientation = orientation
    };

    return vnc_broker_send_display_state(
        control_fd,
        VNC_BROKER_CONTROL_DISPLAY_SIZE_REJECTED,
        &rejected);
}

static int
web_send_display_applied(int control_fd,
                         const VncBrokerDisplayState *state,
                         const RuntimeConfig *cfg)
{
    if (!state || !cfg)
        return -1;

    VncBrokerDisplayState applied = *state;
    applied.width = (uint32_t)cfg->width;
    applied.height = (uint32_t)cfg->height;
    return vnc_broker_send_display_state(
        control_fd,
        VNC_BROKER_CONTROL_DISPLAY_SIZE_APPLIED,
        &applied);
}

static int
serve_web_media_lifetime(int control_fd,
                         const RuntimeConfig *cfg,
                         FrameBridge *frames,
                         PipelineStats *pipeline_stats)
{
    if (!cfg || cfg->width < (int)VNC_BROKER_VIDEO_DIMENSION_MIN ||
        cfg->height < (int)VNC_BROKER_VIDEO_DIMENSION_MIN ||
        (uint32_t)cfg->width > VNC_BROKER_VIDEO_DIMENSION_MAX ||
        (uint32_t)cfg->height > VNC_BROKER_VIDEO_DIMENSION_MAX) {
        LOG_ERROR("Legacy browser media size exceeds safe display limits");
        errno = EINVAL;
        return -1;
    }

    RuntimeConfig session_cfg = *cfg;

    RealMonitor real;
    memset(&real, 0, sizeof(real));

    MonitorLayoutCache layout_cache;
    memset(&layout_cache, 0, sizeof(layout_cache));

    DeviceProfile device_profile;
    memset(&device_profile, 0, sizeof(device_profile));
    int device_bound = 0;
    VncBrokerDisplayMode active_mode = VNC_BROKER_DISPLAY_WINDOW;
    VncBrokerDisplayOrientation active_orientation =
        session_cfg.width >= session_cfg.height ?
            VNC_BROKER_ORIENTATION_LANDSCAPE :
            VNC_BROKER_ORIENTATION_PORTRAIT;
    uint32_t applied_generation = 0;

    WebMediaSender sender;
    memset(&sender, 0, sizeof(sender));
    sender.control_fd = control_fd;
    sender.width = session_cfg.width;
    sender.height = session_cfg.height;
    sender.frames = frames;

    WebHlsStream hls;
    memset(&hls, 0, sizeof(hls));

    int mutex_rc = pthread_mutex_init(&sender.mutex, NULL);
    if (mutex_rc != 0) {
        errno = mutex_rc;
        return -1;
    }

    int cond_rc = pthread_cond_init(&sender.ack_cond, NULL);
    if (cond_rc != 0) {
        pthread_mutex_destroy(&sender.mutex);
        errno = cond_rc;
        return -1;
    }

    pthread_t sender_thread;
    memset(&sender_thread, 0, sizeof(sender_thread));
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

        if (type == VNC_BROKER_CONTROL_DEVICE_BIND) {
            if (device_bound || media_started) {
                LOG_ERROR("Duplicate/late browser device binding");
                result = -1;
                break;
            }

            char device_id[VNC_BROKER_DEVICE_ID_HEX_LEN + 1];
            if (vnc_broker_parse_device_bind(payload,
                                             payload_len,
                                             device_id) < 0 ||
                device_profile_load(&device_profile,
                                    device_id,
                                    session_cfg.width,
                                    session_cfg.height) < 0) {
                LOG_ERROR("Could not load browser device profile");
                result = -1;
                break;
            }

            device_bound = 1;
            active_mode = (VncBrokerDisplayMode)device_profile.last_mode;
            active_orientation =
                (VncBrokerDisplayOrientation)device_profile.last_orientation;

            int remembered_width = 0;
            int remembered_height = 0;
            if (device_profile_get_size(
                    &device_profile,
                    (DeviceDisplayMode)active_mode,
                    (DeviceOrientation)active_orientation,
                    &remembered_width,
                    &remembered_height) == 0) {
                session_cfg.width = remembered_width;
                session_cfg.height = remembered_height;
            }

            LOG_INFO("Browser device profile bound: device=%.8s state=%s/%s size=%dx%d",
                     device_profile.id,
                     device_display_mode_name((DeviceDisplayMode)active_mode),
                     device_orientation_name((DeviceOrientation)active_orientation),
                     session_cfg.width,
                     session_cfg.height);
            continue;
        }

        if (type == VNC_BROKER_CONTROL_MEDIA_START && payload_len == 0) {
            if (media_started || !device_bound) {
                LOG_ERROR("Invalid browser media start: media=%d device-bound=%d",
                          media_started, device_bound);
                result = -1;
                break;
            }

            if (frame_bridge_resize(frames,
                                    session_cfg.width,
                                    session_cfg.height) < 0) {
                LOG_ERROR("Could not prepare browser FrameBridge at %dx%d",
                          session_cfg.width, session_cfg.height);
                result = -1;
                break;
            }

            frame_bridge_clear(frames);

            if (web_device_layout_prepare(&layout_cache,
                                          &session_cfg,
                                          &device_profile,
                                          active_mode,
                                          active_orientation) < 0) {
                LOG_DEBUG("Browser device layout cache preparation failed; continuing without cached layout");
            }
            else {
                web_device_layout_seed_legacy(&layout_cache, &session_cfg);
            }

            /*
             * WEB sessions get their own capture ceiling. session_cfg is a
             * private copy, so the VNC transport and its capture policy are
             * deliberately untouched.
             */
            if (session_cfg.max_fps > WEB_LEGACY_CAPTURE_MAX_FPS)
                session_cfg.max_fps = WEB_LEGACY_CAPTURE_MAX_FPS;
            LOG_INFO("[WEB][CAPTURE] upper-bound=%d fps transport=wss-jpeg vnc-policy=unchanged",
                     session_cfg.max_fps);

            if (real_monitor_start(&real,
                                   &session_cfg,
                                   frames,
                                   pipeline_stats) < 0) {
                LOG_ERROR("Could not start legacy browser virtual monitor/capture");
                monitor_layout_cache_clear(&layout_cache);
                result = -1;
                break;
            }

            media_started = 1;

            if (monitor_layout_cache_apply(&layout_cache,
                                           &session_cfg,
                                           session_cfg.capture_timeout_ms) < 0) {
                LOG_DEBUG("Cached browser device layout could not be applied; using Mutter's current layout");
            }

            if (vnc_log_enabled(VNC_LOG_DEBUG))
                (void)monitor_layout_log_matching_modes(&layout_cache,
                                                        &session_cfg);

            if (web_media_sender_start(&sender,
                                       &sender_thread,
                                       session_cfg.width,
                                       session_cfg.height) == 0) {
                sender_started = 1;
                LOG_INFO("Legacy browser media selected low-latency WSS/JPEG: %dx%d max-fps=%d quality=%d queue-depth=1",
                         session_cfg.width,
                         session_cfg.height,
                         WEB_LEGACY_MAX_FPS,
                         WEB_LEGACY_JPEG_QUALITY);
                continue;
            }

            LOG_INFO("Legacy browser WSS/JPEG sender unavailable (%s); falling back to HLS/H.264",
                     strerror(errno));

            if (web_hls_start(&hls,
                              control_fd,
                              frames,
                              session_cfg.width,
                              session_cfg.height,
                              WEB_HLS_TEST_FPS) == 0) {
                hls_started = 1;
                LOG_INFO("Legacy browser media selected HLS/H.264 fallback transport");
                continue;
            }

            LOG_ERROR("Could not start either legacy browser media transport");
            result = -1;
            break;
        }

        if (type == VNC_BROKER_CONTROL_VIDEO_FRAME_ACK && payload_len == 0) {
            if (sender_started)
                web_media_sender_ack(&sender);
            continue;
        }

        if (type == VNC_BROKER_CONTROL_DISPLAY_SIZE) {
            VncBrokerDisplayState requested;
            if (!device_bound ||
                vnc_broker_parse_display_state(payload,
                                               payload_len,
                                               &requested) < 0) {
                LOG_ERROR("Invalid browser display-state control packet");
                result = -1;
                break;
            }

            if (requested.generation <= applied_generation)
                continue;

            VncBrokerDisplayMode old_mode = active_mode;
            VncBrokerDisplayOrientation old_orientation = active_orientation;
            int old_width = session_cfg.width;
            int old_height = session_cfg.height;

            /*
             * Fullscreen/standalone transitions often change only presentation
             * mode while retaining the same pixel size. Do not tear down the
             * encoder or virtual monitor in that case; just switch the profile
             * scope and acknowledge the stable browser state.
             */
            if ((int)requested.width == session_cfg.width &&
                (int)requested.height == session_cfg.height) {
                int state_changed =
                    requested.mode != active_mode ||
                    requested.orientation != active_orientation;

                if (state_changed && media_started) {
                    if (monitor_layout_cache_save(&layout_cache,
                                                  &session_cfg) < 0)
                        LOG_DEBUG("Could not save pre-mode-switch browser layout");

                    char *previous_layout_path =
                        layout_cache.cache_path ?
                            g_strdup(layout_cache.cache_path) : NULL;
                    monitor_layout_cache_clear(&layout_cache);

                    active_mode = requested.mode;
                    active_orientation = requested.orientation;

                    if (web_device_layout_prepare(&layout_cache,
                                                  &session_cfg,
                                                  &device_profile,
                                                  active_mode,
                                                  active_orientation) == 0) {
                        if (previous_layout_path &&
                            !layout_cache.cache_existed)
                            (void)monitor_layout_cache_seed_from(
                                &layout_cache,
                                previous_layout_path);
                        (void)monitor_layout_cache_apply(
                            &layout_cache,
                            &session_cfg,
                            session_cfg.capture_timeout_ms);
                    }
                    g_free(previous_layout_path);
                }
                else {
                    active_mode = requested.mode;
                    active_orientation = requested.orientation;
                }

                if (device_profile_update_state(
                        &device_profile,
                        (DeviceDisplayMode)active_mode,
                        (DeviceOrientation)active_orientation,
                        session_cfg.width,
                        session_cfg.height) < 0 ||
                    device_profile_save(&device_profile) < 0)
                    LOG_DEBUG("Browser device display profile was not persisted");

                applied_generation = requested.generation;
                if (web_send_display_applied(control_fd,
                                             &requested,
                                             &session_cfg) < 0) {
                    LOG_INFO("Could not acknowledge browser display state: %s",
                             strerror(errno));
                    result = -1;
                    break;
                }

                LOG_INFO("Browser display state applied without monitor rebuild: device=%.8s generation=%u size=%dx%d state=%s/%s",
                         device_profile.id,
                         requested.generation,
                         session_cfg.width,
                         session_cfg.height,
                         device_display_mode_name((DeviceDisplayMode)active_mode),
                         device_orientation_name((DeviceOrientation)active_orientation));
                continue;
            }

            int used_jpeg = sender_started;
            int used_hls = hls_started;

            if (sender_started)
                web_media_sender_stop_join(&sender,
                                           sender_thread,
                                           &sender_started);
            if (hls_started) {
                web_hls_stop(&hls);
                hls_started = 0;
            }

            if (media_started &&
                monitor_layout_cache_save(&layout_cache,
                                          &session_cfg) < 0) {
                LOG_DEBUG("Could not save pre-resize browser device layout");
            }
            char *previous_layout_path =
                layout_cache.cache_path ?
                    g_strdup(layout_cache.cache_path) : NULL;
            monitor_layout_cache_clear(&layout_cache);

            int resize_ok = 1;
            if (media_started &&
                real_monitor_resize(&real,
                                    &session_cfg,
                                    frames,
                                    pipeline_stats,
                                    (int)requested.width,
                                    (int)requested.height) < 0) {
                resize_ok = 0;
                LOG_INFO("Browser display resize rejected by runtime; keeping %dx%d",
                         old_width, old_height);
            }
            else if (!media_started) {
                session_cfg.width = (int)requested.width;
                session_cfg.height = (int)requested.height;
            }

            if (!resize_ok) {
                active_mode = old_mode;
                active_orientation = old_orientation;
                if (web_device_layout_prepare(&layout_cache,
                                              &session_cfg,
                                              &device_profile,
                                              active_mode,
                                              active_orientation) == 0 &&
                    media_started) {
                    (void)monitor_layout_cache_apply(
                        &layout_cache,
                        &session_cfg,
                        session_cfg.capture_timeout_ms);
                }
            }
            else {
                active_mode = requested.mode;
                active_orientation = requested.orientation;

                if (web_device_layout_prepare(&layout_cache,
                                              &session_cfg,
                                              &device_profile,
                                              active_mode,
                                              active_orientation) < 0) {
                    LOG_DEBUG("Could not prepare resized browser layout scope");
                }
                else {
                    if (media_started &&
                        previous_layout_path &&
                        !layout_cache.cache_existed &&
                        monitor_layout_cache_seed_from(
                            &layout_cache,
                            previous_layout_path) < 0) {
                        LOG_DEBUG("Could not seed first layout for new browser display state");
                    }
                    if (media_started &&
                        monitor_layout_cache_apply(
                            &layout_cache,
                            &session_cfg,
                            session_cfg.capture_timeout_ms) < 0) {
                        LOG_DEBUG("No cached layout for resized browser display state");
                    }
                }

                if (device_profile_update_state(
                        &device_profile,
                        (DeviceDisplayMode)active_mode,
                        (DeviceOrientation)active_orientation,
                        session_cfg.width,
                        session_cfg.height) < 0 ||
                    device_profile_save(&device_profile) < 0) {
                    LOG_DEBUG("Browser device display profile was not persisted");
                }

                applied_generation = requested.generation;
            }

            g_free(previous_layout_path);
            previous_layout_path = NULL;

            if (media_started && used_jpeg) {
                if (web_media_sender_start(&sender,
                                           &sender_thread,
                                           session_cfg.width,
                                           session_cfg.height) == 0) {
                    sender_started = 1;
                }
                else {
                    LOG_INFO("JPEG sender restart failed after display resize; trying HLS");
                    used_hls = 1;
                }
            }

            if (media_started && used_hls && !sender_started) {
                if (web_hls_start(&hls,
                                  control_fd,
                                  frames,
                                  session_cfg.width,
                                  session_cfg.height,
                                  WEB_HLS_TEST_FPS) == 0) {
                    hls_started = 1;
                }
                else {
                    LOG_ERROR("Could not restore browser media after display resize");
                    result = -1;
                    break;
                }
            }

            if (!resize_ok) {
                if (web_send_display_rejected(control_fd,
                                              requested.generation,
                                              old_mode,
                                              old_orientation,
                                              &session_cfg) < 0) {
                    LOG_INFO("Could not report rejected browser display state: %s",
                             strerror(errno));
                    result = -1;
                    break;
                }
                continue;
            }

            if (web_send_display_applied(control_fd,
                                         &requested,
                                         &session_cfg) < 0) {
                LOG_INFO("Could not acknowledge browser display state: %s",
                         strerror(errno));
                result = -1;
                break;
            }

            LOG_INFO("Browser display state applied: device=%.8s generation=%u size=%dx%d state=%s/%s",
                     device_profile.id,
                     requested.generation,
                     session_cfg.width,
                     session_cfg.height,
                     device_display_mode_name((DeviceDisplayMode)active_mode),
                     device_orientation_name((DeviceOrientation)active_orientation));
            continue;
        }

        /* SDP/ICE will be handled here when the modern WebRTC backend lands. */
        LOG_DEBUG("Ignoring unsupported browser control message type=%u payload=%zu",
                  (unsigned)type, payload_len);
    }

    if (hls_started)
        web_hls_stop(&hls);

    if (sender_started)
        web_media_sender_stop_join(&sender, sender_thread, &sender_started);

    if (media_started) {
        if (monitor_layout_cache_save(&layout_cache,
                                      &session_cfg) < 0)
            LOG_DEBUG("Browser device layout was not saved");

        real_monitor_stop(&real);
        frame_bridge_clear(frames);
    }

    monitor_layout_cache_clear(&layout_cache);
    device_profile_clear(&device_profile);

    pthread_cond_destroy(&sender.ack_cond);
    pthread_mutex_destroy(&sender.mutex);
    return result;
}

static VncBrokerWebAuthResult
authenticate_parsed_control_request(VncBrokerWebAuthRequest *request,
                                    const RuntimeConfig *cfg,
                                    const char *purpose)
{
    VncBrokerWebAuthResult result = VNC_BROKER_WEB_AUTH_ERROR;

    if (!request || !request->username || !request->password)
        return result;

    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_name) {
        LOG_ERROR("Cannot resolve local Unix account for %s authentication",
                  purpose);
        return result;
    }

    if (strcmp(request->username, pw->pw_name) != 0) {
        LOG_INFO("Rejected %s authentication for user '%s': agent belongs to '%s'",
                 purpose,
                 request->username,
                 pw->pw_name);
        return VNC_BROKER_WEB_AUTH_DENIED;
    }

    if (io_deadline_set_ms(cfg->client_handshake_timeout_ms) < 0) {
        LOG_ERROR("Could not start %s PAM deadline: %s",
                  purpose,
                  strerror(errno));
        return result;
    }

    int auth_rc = auth_client_check(cfg->auth_socket,
                                    request->username,
                                    request->password);
    io_deadline_clear();

    if (auth_rc == 1)
        result = VNC_BROKER_WEB_AUTH_OK;
    else if (auth_rc == 0)
        result = VNC_BROKER_WEB_AUTH_DENIED;

    return result;
}

static VncBrokerWebAuthResult
authenticate_control_request(int control_fd,
                             const RuntimeConfig *cfg,
                             const char *purpose)
{
    VncBrokerWebAuthRequest request;
    memset(&request, 0, sizeof(request));

    if (vnc_broker_recv_web_auth_request(control_fd, &request) < 0) {
        LOG_ERROR("Invalid %s authentication request from broker: %s",
                  purpose,
                  strerror(errno));
        return VNC_BROKER_WEB_AUTH_ERROR;
    }

    VncBrokerWebAuthResult result =
        authenticate_parsed_control_request(&request, cfg, purpose);
    vnc_broker_web_auth_request_clear(&request);
    return result;
}

static VncBrokerWebAuthResult
authenticate_or_reuse_web_control_request(int control_fd,
                                          const RuntimeConfig *cfg)
{
    uint8_t payload[4 + VNC_BROKER_AUTH_USERNAME_MAX +
                    VNC_BROKER_AUTH_PASSWORD_MAX];
    VncBrokerControlType type;
    size_t payload_len = 0;

    if (vnc_broker_recv_control(control_fd,
                                &type,
                                payload,
                                sizeof(payload),
                                &payload_len) < 0) {
        LOG_ERROR("Invalid WebRTC authentication control packet: %s",
                  strerror(errno));
        return VNC_BROKER_WEB_AUTH_ERROR;
    }

    if (type == VNC_BROKER_CONTROL_WEB_AUTH_REUSE) {
        if (payload_len != 0) {
            errno = EPROTO;
            return VNC_BROKER_WEB_AUTH_ERROR;
        }

        /*
         * handle_broker_handoff() already verified SO_PEERCRED uid=0 and
         * handoff.uid == getuid(). Reuse is therefore delegated only by the
         * local root broker after it validates its browser session token.
         */
        LOG_INFO("WebRTC browser reused previously authenticated browser session");
        return VNC_BROKER_WEB_AUTH_OK;
    }

    if (type != VNC_BROKER_CONTROL_WEB_AUTH_REQUEST) {
        explicit_bzero(payload, sizeof(payload));
        errno = EPROTO;
        return VNC_BROKER_WEB_AUTH_ERROR;
    }

    VncBrokerWebAuthRequest request;
    memset(&request, 0, sizeof(request));
    int parse_rc =
        vnc_broker_parse_web_auth_request(payload, payload_len, &request);
    explicit_bzero(payload, sizeof(payload));
    if (parse_rc < 0) {
        LOG_ERROR("Invalid WebRTC authentication request payload");
        return VNC_BROKER_WEB_AUTH_ERROR;
    }

    VncBrokerWebAuthResult result =
        authenticate_parsed_control_request(&request, cfg, "WebRTC");
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
        authenticate_or_reuse_web_control_request(control_fd, cfg);

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
