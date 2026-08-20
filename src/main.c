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
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void
configure_external_socket(int client_fd, const RuntimeConfig *cfg)
{
    if (setsockopt(client_fd,
                   SOL_SOCKET,
                   SO_SNDBUF,
                   &cfg->external_send_buffer,
                   sizeof(cfg->external_send_buffer)) < 0) {
        LOG_DEBUG("Could not set external SO_SNDBUF: %s", strerror(errno));
        return;
    }

    if (!vnc_log_enabled(VNC_LOG_DEBUG))
        return;

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

    if (ra2_server_handshake(client_fd, &session, &cfg) < 0) {
        LOG_ERROR("RA2r handshake failed");
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

    int listen_fd = create_public_listener(&cfg);
    if (listen_fd < 0) {
        LOG_ERROR("Could not create public listener on TCP/%d: %s",
                  cfg.public_port,
                  strerror(errno));
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        pipeline_stats_destroy(&pipeline_stats);
        return 1;
    }

    if (shutdown_signal_register_fd(listen_fd) < 0)
        LOG_DEBUG("Could not register public listener for shutdown");

    LOG_INFO("VNC Monitor %s ready on TCP/%d (screen=%s fallback=%dx%d; virtual monitor is created on client connection)",
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

        LOG_INFO("Client connected: %s", peer_addr);

        if (shutdown_signal_register_fd(client_fd) < 0)
            LOG_DEBUG("Could not register client socket for shutdown");

        configure_external_socket(client_fd, &cfg);
        serve_client(client_fd, &cfg, &frames, &pipeline_stats);

        shutdown_signal_unregister_fd(client_fd);
        close(client_fd);
        LOG_INFO("Client disconnected: %s", peer_addr);
    }

    LOG_INFO("Stopping VNC Monitor");

    shutdown_signal_unregister_fd(listen_fd);
    close(listen_fd);
    frame_bridge_destroy(&frames);
    shutdown_signal_cleanup();
    pipeline_stats_destroy(&pipeline_stats);

    LOG_INFO("VNC Monitor stopped");
    return 0;
}
