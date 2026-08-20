#ifndef VNC_MONITOR_BROKER_PROTOCOL_H
#define VNC_MONITOR_BROKER_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

#define VNC_BROKER_PROTOCOL_MAGIC       0x564d4231u /* VMB1 */
#define VNC_BROKER_PROTOCOL_VERSION     1u
#define VNC_BROKER_SESSION_ID_MAX       64
#define VNC_BROKER_PEER_ADDR_MAX        64

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
} VncBrokerHandoff;

enum {
    VNC_BROKER_STATUS_BUSY = 2,
    VNC_BROKER_STATUS_REJECT = 3,
    VNC_BROKER_STATUS_DONE = 4
};

int vnc_broker_send_handoff(int control_fd,
                            int client_fd,
                            uid_t uid,
                            const char *session_id,
                            const char *peer_addr);

int vnc_broker_recv_handoff(int control_fd,
                            int *client_fd,
                            VncBrokerHandoff *handoff);

int vnc_broker_send_status(int control_fd, uint8_t status);
int vnc_broker_recv_status(int control_fd, uint8_t *status);

#endif
