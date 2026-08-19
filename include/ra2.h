#ifndef VNC_MONITOR_RA2_H
#define VNC_MONITOR_RA2_H

#include <stddef.h>
#include <stdint.h>

#include "runtime_config.h"

typedef struct {
    uint8_t key[16];
    uint64_t counter;
} Ra2Direction;

typedef struct {
    Ra2Direction client_to_server;
    Ra2Direction server_to_client;
    char username[256];
} Ra2Session;

/*
 * Performs RFB 3.8 + RA2r (security type 13), auth-helper validation,
 * second RA2r re-key and encrypted SecurityResult=OK.
 *
 * On success, the next client message is encrypted ClientInit and can be
 * consumed with ra2_recv_record().
 */
int ra2_server_handshake(
    int fd,
    Ra2Session *session,
    const RuntimeConfig *cfg);

int ra2_send_record(
    int fd,
    Ra2Direction *dir,
    const uint8_t *plain,
    size_t plain_len);

int ra2_recv_record(
    int fd,
    Ra2Direction *dir,
    uint8_t **plain_out,
    size_t *plain_len_out);

void ra2_session_clear(Ra2Session *session);

#endif
