#define _GNU_SOURCE

#include "config.h"
#include "ra2.h"
#include "rfb_backend.h"
#include "rfb_proxy.h"
#include "runtime_config.h"
#include "shutdown_signal.h"
#include "frame_bridge.h"
#include "real_monitor.h"
#include "monitor_layout_cache.h"
#include "pipeline_stats.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int running;
    int client_fd;
    char peer_addr[INET_ADDRSTRLEN];

    const RuntimeConfig *cfg;
    FrameBridge *frames;
    PipelineStats *pipeline_stats;
} ClientSlot;

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
set_client_io_timeout(int client_fd, int timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    if (setsockopt(client_fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        LOG_ERROR("Could not set required RA2 handshake SO_RCVTIMEO=%dms: %s",
                  timeout_ms,
                  strerror(errno));
        return -1;
    }

    if (setsockopt(client_fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        LOG_ERROR("Could not set required RA2 handshake SO_SNDTIMEO=%dms: %s",
                  timeout_ms,
                  strerror(errno));
        return -1;
    }

    return 0;
}

static void
clear_client_io_timeout(int client_fd)
{
    struct timeval timeout = {0};

    if (setsockopt(client_fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        LOG_DEBUG("Could not clear client SO_RCVTIMEO: %s", strerror(errno));
    }

    if (setsockopt(client_fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        LOG_DEBUG("Could not clear client SO_SNDTIMEO: %s", strerror(errno));
    }
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

        LOG_DEBUG("Client liveness: keepalive idle=%ds interval=%ds probes=%d user-timeout=%dms handshake-timeout=%dms",
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
     * A connected but non-negotiating peer must not monopolize the only client
     * slot forever. The timeout applies only to the unauthenticated RA2/PAM
     * handshake and is removed before the long-lived RFB session begins.
     */
    if (set_client_io_timeout(client_fd, cfg.client_handshake_timeout_ms) < 0)
        return;

    if (ra2_server_handshake(client_fd, &session, &cfg) < 0) {
        clear_client_io_timeout(client_fd);
        LOG_ERROR("RA2r handshake failed or timed out");
        return;
    }

    clear_client_io_timeout(client_fd);

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

static void *
client_worker(void *arg)
{
    ClientSlot *slot = arg;

    pthread_mutex_lock(&slot->mutex);
    int client_fd = slot->client_fd;
    char peer_addr[INET_ADDRSTRLEN];
    snprintf(peer_addr, sizeof(peer_addr), "%s", slot->peer_addr);
    const RuntimeConfig *cfg = slot->cfg;
    FrameBridge *frames = slot->frames;
    PipelineStats *pipeline_stats = slot->pipeline_stats;
    pthread_mutex_unlock(&slot->mutex);

    LOG_INFO("Client connected: %s", peer_addr);
    serve_client(client_fd, cfg, frames, pipeline_stats);

    shutdown_signal_unregister_fd(client_fd);
    close(client_fd);
    LOG_INFO("Client disconnected: %s", peer_addr);

    pthread_mutex_lock(&slot->mutex);
    slot->client_fd = -1;
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
    pthread_cond_broadcast(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
}

static int
start_client_worker(ClientSlot *slot,
                    int client_fd,
                    const char *peer_addr,
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
    slot->cfg = cfg;
    slot->frames = frames;
    slot->pipeline_stats = pipeline_stats;
    snprintf(slot->peer_addr,
             sizeof(slot->peer_addr),
             "%s",
             peer_addr ? peer_addr : "unknown");

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

static void
wait_for_client_slot(ClientSlot *slot)
{
    pthread_mutex_lock(&slot->mutex);

    if (slot->running && slot->client_fd >= 0)
        (void)shutdown(slot->client_fd, SHUT_RDWR);

    while (slot->running)
        pthread_cond_wait(&slot->cond, &slot->mutex);

    pthread_mutex_unlock(&slot->mutex);
}

int
main(int argc, char **argv)
{
    RuntimeConfig cfg;
    runtime_config_defaults(&cfg);

    if (runtime_config_parse(&cfg, argc, argv) < 0) {
        runtime_config_usage(argv[0]);
        return 2;
    }

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

    int listen_fd = create_public_listener(&cfg);
    if (listen_fd < 0) {
        LOG_ERROR("Could not create public listener on TCP/%d: %s",
                  cfg.public_port,
                  strerror(errno));
        pthread_cond_destroy(&client_slot.cond);
        pthread_mutex_destroy(&client_slot.mutex);
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    if (shutdown_signal_register_fd(listen_fd) < 0)
        LOG_DEBUG("Could not register public listener for shutdown");

    LOG_INFO("VNC Monitor %s ready on TCP/%d (strong single-connect; screen=%s fallback=%dx%d; virtual monitor is created on client connection)",
             VNC_MONITOR_VERSION,
             cfg.public_port,
             runtime_config_screen_size_mode_name(cfg.screen_size_mode),
             cfg.width,
             cfg.height);

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

        char peer_addr[INET_ADDRSTRLEN] = "unknown";
        (void)inet_ntop(AF_INET,
                        &peer.sin_addr,
                        peer_addr,
                        sizeof(peer_addr));

        int start_rc = start_client_worker(&client_slot,
                                           client_fd,
                                           peer_addr,
                                           &cfg,
                                           &frames,
                                           &pipeline_stats);

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

    LOG_INFO("Stopping VNC Monitor");

    shutdown_signal_unregister_fd(listen_fd);
    close(listen_fd);

    /* The shared framebuffer/stats outlive the only permitted client worker. */
    wait_for_client_slot(&client_slot);

    pthread_cond_destroy(&client_slot.cond);
    pthread_mutex_destroy(&client_slot.mutex);
    frame_bridge_destroy(&frames);
    shutdown_signal_cleanup();
    pipeline_stats_destroy(&pipeline_stats);

    LOG_INFO("VNC Monitor stopped");
    return 0;
}
