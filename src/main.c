#include "config.h"
#include "ra2.h"
#include "rfb_backend.h"
#include "rfb_proxy.h"
#include "runtime_config.h"
#include "shutdown_signal.h"
#include "benchmark.h"
#include "frame_bridge.h"
#include "real_monitor.h"
#include "monitor_layout_cache.h"
#include "mutter_environment.h"
#include "pipeline_stats.h"

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
    int fd =
        socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            0
        );

    if (fd < 0)
        return -1;

    int one = 1;

    setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &one,
        sizeof(one)
    );

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->public_port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (
        bind(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0 ||
        listen(fd, 8) < 0
    ) {
        close(fd);
        return -1;
    }

    return fd;
}

int
main(int argc, char **argv)
{
    RuntimeConfig cfg;
    runtime_config_defaults(&cfg);

    if (
        runtime_config_parse(
            &cfg,
            argc,
            argv
        ) < 0
    ) {
        runtime_config_usage(argv[0]);
        return 2;
    }

    runtime_config_print(&cfg);

    if (cfg.source_mode == FRAME_SOURCE_MUTTER) {
        (void)mutter_environment_log_hardware_cursor(
            cfg.mutter_hardware_cursor_mode
        );
    }

    if (benchmark_init(&cfg) < 0) {
        fprintf(
            stderr,
            "Failed to initialize benchmark subsystem\n"
        );

        return 1;
    }

    PipelineStats pipeline_stats;

    if (
        pipeline_stats_init(
            &pipeline_stats,
            &cfg
        ) < 0
    ) {
        fprintf(
            stderr,
            "Failed to initialize pipeline statistics\n"
        );

        benchmark_shutdown();
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (shutdown_signal_init() < 0) {
        benchmark_shutdown();
        return 1;
    }

    FrameBridge frames;

    if (
        frame_bridge_init(
            &frames,
            cfg.width,
            cfg.height
        ) < 0
    ) {
        fprintf(
            stderr,
            "Failed to initialize framebuffer bridge\n"
        );

        shutdown_signal_cleanup();
        benchmark_shutdown();
        return 1;
    }

    RfbBackend backend;

    if (
        rfb_backend_start(
            &backend,
            &cfg,
            &frames,
            &pipeline_stats
        ) < 0
    ) {
        fprintf(
            stderr,
            "Failed to start internal LibVNCServer backend\n"
        );
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        benchmark_shutdown();
        return 1;
    }

    int listen_fd =
        create_public_listener(&cfg);

    if (listen_fd < 0) {
        perror("public listener");
        rfb_backend_stop(&backend);
        frame_bridge_destroy(&frames);
        shutdown_signal_cleanup();
        benchmark_shutdown();
        return 1;
    }

    printf(
        "RA2r VNC front-end listening on TCP port %d\n",
        cfg.public_port
    );

    if (shutdown_signal_register_fd(listen_fd) < 0)
        perror("register public listener for shutdown");

    while (!shutdown_signal_requested()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        int stop_fd =
            shutdown_signal_fd();

        if (stop_fd >= 0)
            FD_SET(stop_fd, &readfds);

        int maxfd =
            listen_fd > stop_fd
                ? listen_fd
                : stop_fd;

        int sel =
            select(
                maxfd + 1,
                &readfds,
                NULL,
                NULL,
                NULL
            );

        if (sel < 0) {
            if (errno == EINTR) {
                if (shutdown_signal_requested())
                    break;

                continue;
            }

            perror("select listener");
            break;
        }

        if (
            stop_fd >= 0 &&
            FD_ISSET(stop_fd, &readfds)
        ) {
            shutdown_signal_drain();
            break;
        }

        if (!FD_ISSET(listen_fd, &readfds))
            continue;

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int client_fd =
            accept(
                listen_fd,
                (struct sockaddr *)&peer,
                &peer_len
            );

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        printf(
            "External client connected: %s\n",
            inet_ntoa(peer.sin_addr)
        );

        if (shutdown_signal_register_fd(client_fd) < 0)
            perror("register external client for shutdown");

        if (
            setsockopt(
                client_fd,
                SOL_SOCKET,
                SO_SNDBUF,
                &cfg.external_send_buffer,
                sizeof(cfg.external_send_buffer)
            ) < 0
        ) {
            perror("setsockopt external SO_SNDBUF");
        }
        else {
            int actual = 0;
            socklen_t actual_len =
                sizeof(actual);

            if (
                getsockopt(
                    client_fd,
                    SOL_SOCKET,
                    SO_SNDBUF,
                    &actual,
                    &actual_len
                ) == 0
            ) {
                printf(
                    "External TCP send buffer: requested=%d actual=%d bytes\n",
                    cfg.external_send_buffer,
                    actual
                );
            }
        }

        Ra2Session session;

        if (
            ra2_server_handshake(
                client_fd,
                &session,
                &cfg
            ) == 0
        ) {
            RealMonitor real = {0};
            int source_ready = 1;

            MonitorLayoutCache layout_cache = {0};

            if (
                cfg.source_mode ==
                FRAME_SOURCE_MUTTER
            ) {
                frame_bridge_clear(&frames);

                if (
                    monitor_layout_cache_prepare(
                        &layout_cache,
                        &cfg
                    ) < 0
                ) {
                    fprintf(
                        stderr,
                        "Monitor-layout cache preparation failed; "
                        "continuing without cached layout\n"
                    );
                }

                source_ready =
                    real_monitor_start(
                        &real,
                        &cfg,
                        &frames,
                        &pipeline_stats
                    ) == 0;

                if (
                    source_ready &&
                    monitor_layout_cache_apply(
                        &layout_cache,
                        &cfg,
                        cfg.capture_timeout_ms
                    ) < 0
                ) {
                    fprintf(
                        stderr,
                        "Cached monitor layout could not be applied; "
                        "continuing with Mutter's current layout\n"
                    );
                }

                if (source_ready) {
                    (void)monitor_layout_log_matching_modes(
                        &layout_cache,
                        &cfg
                    );
                }
            }

            if (source_ready) {
                (void)rfb_proxy_run(
                    client_fd,
                    &session,
                    &cfg,
                    &pipeline_stats
                );
            }
            else {
                fprintf(
                    stderr,
                    "Real monitor source failed; closing VNC client\n"
                );
            }

            if (
                cfg.source_mode ==
                FRAME_SOURCE_MUTTER
            ) {
                /*
                 * Save while the virtual monitor still exists, then stop it.
                 * On the first successful session this teaches the program
                 * the layout that the user arranged in GNOME Settings.
                 */
                if (source_ready) {
                    (void)monitor_layout_cache_save(
                        &layout_cache,
                        &cfg
                    );
                }
                else {
                    fprintf(
                        stderr,
                        "Layout cache not saved because "
                        "the real monitor source did not start successfully\n"
                    );
                }

                real_monitor_stop(&real);
                frame_bridge_clear(&frames);

                monitor_layout_cache_clear(
                    &layout_cache
                );
            }

            ra2_session_clear(
                &session
            );
        }
        else {
            fprintf(
                stderr,
                "RA2r handshake failed\n"
            );
        }

        shutdown_signal_unregister_fd(client_fd);
        close(client_fd);

        printf(
            "External client disconnected\n"
        );
    }

    printf("Stopping VNC monitor test...\n");

    shutdown_signal_unregister_fd(listen_fd);
    close(listen_fd);
    rfb_backend_stop(&backend);
    frame_bridge_destroy(&frames);
    shutdown_signal_cleanup();
    pipeline_stats_destroy(&pipeline_stats);
    benchmark_shutdown();

    printf("Stopped.\n");
    return 0;
}
