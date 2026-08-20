#ifndef VNC_MONITOR_BROKER_PROTOCOL_H
#define VNC_MONITOR_BROKER_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

#define VNC_BROKER_PROTOCOL_MAGIC       0x564d4231u /* VMB1 */
#define VNC_BROKER_PROTOCOL_VERSION     1u
#define VNC_BROKER_SESSION_ID_MAX       64
#define VNC_BROKER_PEER_ADDR_MAX        64

typedef enum {
    /* Protocol-v1 handoffs from beta.3 used zero in this former reserved field. */
    VNC_BROKER_TRANSPORT_LEGACY_VNC = 0,
    VNC_BROKER_TRANSPORT_VNC = 1,
    VNC_BROKER_TRANSPORT_WEBRTC = 2
} VncBrokerTransport;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t transport;
    uint32_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
} VncBrokerHandoff;

enum {
    VNC_BROKER_STATUS_BUSY = 2,
    VNC_BROKER_STATUS_REJECT = 3,
    VNC_BROKER_STATUS_DONE = 4
};

/*
 * Backward-compatible VNC handoff helper. Existing callers keep using this
 * function while the broker grows a second WebRTC transport.
 */
int vnc_broker_send_handoff(int control_fd,
                            int client_fd,
                            uid_t uid,
                            const char *session_id,
                            const char *peer_addr);

/*
 * Generic transport-aware handoff.
 *
 * VNC transports one accepted TCP fd with SCM_RIGHTS.
 * WebRTC is broker-terminated HTTPS/WSS and therefore carries no browser fd;
 * its signalling/control messages will use the broker-agent control channel.
 *
 * The current fixed-size protocol remains wire-version 1. The old reserved
 * uint16_t is reused as this discriminator; value 0 remains legacy VNC so
 * beta.3 broker/agent processes can coexist briefly during an upgrade.
 */
int vnc_broker_send_handoff_transport(int control_fd,
                                      VncBrokerTransport transport,
                                      int client_fd,
                                      uid_t uid,
                                      const char *session_id,
                                      const char *peer_addr);

int vnc_broker_recv_handoff(int control_fd,
                            int *client_fd,
                            VncBrokerHandoff *handoff);

const char *vnc_broker_transport_name(uint16_t transport);

int vnc_broker_send_status(int control_fd, uint8_t status);
int vnc_broker_recv_status(int control_fd, uint8_t *status);

#endif
