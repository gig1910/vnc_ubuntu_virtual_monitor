#include "rfb_proxy.h"
#include "config.h"
#include "io.h"
#include "shutdown_signal.h"
#include "ra2_stream_coalescer.h"
#include "mutter_virtual_monitor.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int
connect_backend(const RuntimeConfig *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVBUF,
                   &cfg->backend_receive_buffer,
                   sizeof(cfg->backend_receive_buffer)) < 0) {
        LOG_DEBUG("Could not set backend SO_RCVBUF: %s", strerror(errno));
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->backend_port),
        .sin_addr.s_addr = inet_addr(cfg->backend_bind)
    };

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Could not connect internal RFB backend: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int
backend_handshake(int fd, uint8_t client_init)
{
    char server_version[12];
    if (io_read_exact(fd, server_version, sizeof(server_version)) < 0)
        return -1;

    static const char version[] = "RFB 003.008\n";
    if (io_write_exact(fd, version, 12) < 0)
        return -1;

    uint8_t count = 0;
    if (io_read_exact(fd, &count, 1) < 0 || count == 0)
        return -1;

    uint8_t *types = malloc(count);
    if (!types)
        return -1;

    if (io_read_exact(fd, types, count) < 0) {
        free(types);
        return -1;
    }

    int supports_none = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (types[i] == 1) {
            supports_none = 1;
            break;
        }
    }
    free(types);

    if (!supports_none) {
        LOG_ERROR("Internal backend did not offer security type None");
        return -1;
    }

    const uint8_t none = 1;
    if (io_write_exact(fd, &none, 1) < 0)
        return -1;

    uint8_t result[4];
    if (io_read_exact(fd, result, sizeof(result)) < 0 ||
        io_get_u32_be(result) != 0) {
        LOG_ERROR("Internal backend security handshake failed");
        return -1;
    }

    return io_write_exact(fd, &client_init, 1);
}

static int
forward_server_init(int backend_fd,
                    int external_fd,
                    Ra2Direction *server_to_client)
{
    uint8_t header[24];
    if (io_read_exact(backend_fd, header, sizeof(header)) < 0)
        return -1;

    uint32_t name_len = io_get_u32_be(header + 20);
    if (name_len > 1024 * 1024) {
        LOG_ERROR("Backend ServerInit name is absurdly large");
        return -1;
    }

    size_t total = sizeof(header) + (size_t)name_len;
    uint8_t *msg = malloc(total);
    if (!msg)
        return -1;

    memcpy(msg, header, sizeof(header));
    if (name_len > 0 &&
        io_read_exact(backend_fd,
                      msg + sizeof(header),
                      name_len) < 0) {
        free(msg);
        return -1;
    }

    if (total > RA2_MAX_RECORD) {
        LOG_ERROR("Backend ServerInit exceeds one RA2 record");
        free(msg);
        return -1;
    }

    int rc = ra2_send_record(external_fd,
                             server_to_client,
                             msg,
                             total);
    free(msg);
    return rc;
}

static int
relay_loop(int external_fd,
           int backend_fd,
           Ra2Session *session,
           const RuntimeConfig *cfg,
           PipelineStats *stats)
{
    unsigned long long c2s_records = 0;
    unsigned long long s2c_records = 0;
    unsigned long long c2s_bytes = 0;
    unsigned long long s2c_bytes = 0;

    const int mutter_lifecycle_fd =
        mutter_virtual_monitor_lifecycle_fd();

    LOG_DEBUG("RA2 stream relay: record-cap=%d coalesce=%s wait=%dus",
              cfg->ra2_stream_record_max,
              cfg->ra2_coalesce ? "on" : "off",
              cfg->ra2_coalesce_us);

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(external_fd, &readfds);
        FD_SET(backend_fd, &readfds);

        int stop_fd = shutdown_signal_fd();
        if (stop_fd >= 0)
            FD_SET(stop_fd, &readfds);
        if (mutter_lifecycle_fd >= 0)
            FD_SET(mutter_lifecycle_fd, &readfds);

        int maxfd = external_fd > backend_fd ? external_fd : backend_fd;
        if (stop_fd > maxfd)
            maxfd = stop_fd;
        if (mutter_lifecycle_fd > maxfd)
            maxfd = mutter_lifecycle_fd;

        /* Periodic wake-up keeps backpressure telemetry fresh. */
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 20000};
        int rc = select(maxfd + 1,
                        &readfds,
                        NULL,
                        NULL,
                        &timeout);

        if (rc < 0) {
            if (errno == EINTR) {
                if (shutdown_signal_requested())
                    return 0;
                continue;
            }

            LOG_ERROR("RA2 relay select failed: %s", strerror(errno));
            return -1;
        }

        pipeline_stats_transport_queues(stats, external_fd, backend_fd);
        if (rc == 0)
            continue;

        if (stop_fd >= 0 && FD_ISSET(stop_fd, &readfds)) {
            shutdown_signal_drain();
            return 0;
        }

        if (mutter_lifecycle_fd >= 0 &&
            FD_ISSET(mutter_lifecycle_fd, &readfds)) {
            mutter_virtual_monitor_lifecycle_drain();
            LOG_INFO("Mutter stopped screen sharing; closing VNC session");
            return 0;
        }

        if (FD_ISSET(external_fd, &readfds)) {
            uint8_t *plain = NULL;
            size_t plain_len = 0;

            if (ra2_recv_record(external_fd,
                                &session->client_to_server,
                                &plain,
                                &plain_len) < 0) {
                free(plain);
                LOG_DEBUG("RA2 client stream ended (c2s=%llu/%lluB s2c=%llu/%lluB)",
                          c2s_records,
                          c2s_bytes,
                          s2c_records,
                          s2c_bytes);
                return 0;
            }

            c2s_records++;
            c2s_bytes += plain_len;

            int wr = io_write_exact(backend_fd, plain, plain_len);
            free(plain);

            if (wr < 0) {
                LOG_DEBUG("Internal RFB backend stopped accepting client data");
                return 0;
            }
        }

        if (FD_ISSET(backend_fd, &readfds)) {
            Ra2CoalesceResult coalesce_rc =
                ra2_stream_coalesce_and_send(backend_fd,
                                             external_fd,
                                             &session->server_to_client,
                                             cfg,
                                             stats,
                                             &s2c_records,
                                             &s2c_bytes);

            if (coalesce_rc == RA2_COALESCE_BACKEND_EOF) {
                LOG_DEBUG("Internal RFB backend closed relay (c2s=%llu/%lluB s2c=%llu/%lluB)",
                          c2s_records,
                          c2s_bytes,
                          s2c_records,
                          s2c_bytes);
                return 0;
            }

            if (coalesce_rc == RA2_COALESCE_SHUTDOWN)
                return 0;

            if (coalesce_rc == RA2_COALESCE_ERROR) {
                LOG_ERROR("RA2 relay failed while coalescing/sending");
                return 0;
            }
        }
    }
}

int
rfb_proxy_run(int external_fd,
              Ra2Session *session,
              const RuntimeConfig *cfg,
              PipelineStats *stats)
{
    uint8_t *client_init_msg = NULL;
    size_t client_init_len = 0;

    if (ra2_recv_record(external_fd,
                        &session->client_to_server,
                        &client_init_msg,
                        &client_init_len) < 0)
        return -1;

    if (client_init_len != 1) {
        LOG_ERROR("Unexpected ClientInit length: %zu", client_init_len);
        free(client_init_msg);
        return -1;
    }

    uint8_t client_init = client_init_msg[0];
    free(client_init_msg);
    LOG_DEBUG("Encrypted ClientInit received (shared=%u)", client_init);

    int backend_fd = connect_backend(cfg);
    if (backend_fd < 0)
        return -1;

    if (shutdown_signal_register_fd(backend_fd) < 0)
        LOG_DEBUG("Could not register backend socket for shutdown");

    int rc = -1;

    if (backend_handshake(backend_fd, client_init) < 0) {
        LOG_ERROR("Internal LibVNCServer handshake failed");
        goto out;
    }

    if (forward_server_init(backend_fd,
                            external_fd,
                            &session->server_to_client) < 0) {
        LOG_ERROR("Failed forwarding encrypted ServerInit");
        goto out;
    }

    LOG_DEBUG("RA2r <-> LibVNCServer stream relay started");
    rc = relay_loop(external_fd,
                    backend_fd,
                    session,
                    cfg,
                    stats);

out:
    shutdown_signal_unregister_fd(backend_fd);
    close(backend_fd);
    return rc;
}
