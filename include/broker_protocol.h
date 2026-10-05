#ifndef VNC_MONITOR_BROKER_PROTOCOL_H
#define VNC_MONITOR_BROKER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VNC_BROKER_PROTOCOL_MAGIC       0x564d4231u /* VMB1 */
#define VNC_BROKER_PROTOCOL_VERSION     1u
#define VNC_BROKER_SESSION_ID_MAX       64
#define VNC_BROKER_PEER_ADDR_MAX        64

#define VNC_BROKER_CONTROL_MAGIC        0x564d4331u /* VMC1 */
#define VNC_BROKER_CONTROL_VERSION      1u
#define VNC_BROKER_AUTH_USERNAME_MAX    255u
#define VNC_BROKER_AUTH_PASSWORD_MAX    4096u
#define VNC_BROKER_CONTROL_PAYLOAD_MAX  8192u
#define VNC_BROKER_VIDEO_FRAME_MAX      (4u * 1024u * 1024u)
#define VNC_BROKER_VIDEO_DIMENSION_MAX  4096u
#define VNC_BROKER_VIDEO_DIMENSION_MIN  64u
#define VNC_BROKER_DEVICE_ID_HEX_LEN    64u

typedef enum {
    /* Protocol-v1 handoffs from beta.3 used zero in this former reserved field. */
    VNC_BROKER_TRANSPORT_LEGACY_VNC = 0,
    VNC_BROKER_TRANSPORT_VNC = 1,
    VNC_BROKER_TRANSPORT_WEBRTC = 2,
    VNC_BROKER_TRANSPORT_MANAGEMENT = 3
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

typedef enum {
    VNC_BROKER_CONTROL_WEB_AUTH_REQUEST = 1,
    VNC_BROKER_CONTROL_WEB_AUTH_RESULT = 2,
    VNC_BROKER_CONTROL_REVOKE = 3,
    VNC_BROKER_CONTROL_SDP_OFFER = 10,
    VNC_BROKER_CONTROL_SDP_ANSWER = 11,
    VNC_BROKER_CONTROL_ICE_CANDIDATE = 12,
    VNC_BROKER_CONTROL_DISPLAY_SIZE = 13,
    VNC_BROKER_CONTROL_DEVICE_BIND = 14,
    VNC_BROKER_CONTROL_DISPLAY_SIZE_APPLIED = 15,

    VNC_BROKER_CONTROL_MEDIA_START = 20,
    VNC_BROKER_CONTROL_HLS_READY = 21,
    VNC_BROKER_CONTROL_VIDEO_FRAME_BEGIN = 30,
    VNC_BROKER_CONTROL_VIDEO_FRAME_CHUNK = 31,
    VNC_BROKER_CONTROL_VIDEO_FRAME_END = 32,
    VNC_BROKER_CONTROL_VIDEO_FRAME_ACK = 33
} VncBrokerControlType;

typedef enum {
    VNC_BROKER_WEB_AUTH_DENIED = 0,
    VNC_BROKER_WEB_AUTH_OK = 1,
    VNC_BROKER_WEB_AUTH_ERROR = 2
} VncBrokerWebAuthResult;

typedef struct {
    char *username;
    char *password;
    size_t username_len;
    size_t password_len;
} VncBrokerWebAuthRequest;

typedef enum {
    VNC_BROKER_DISPLAY_WINDOW = 0,
    VNC_BROKER_DISPLAY_FULLSCREEN = 1
} VncBrokerDisplayMode;

typedef enum {
    VNC_BROKER_ORIENTATION_PORTRAIT = 0,
    VNC_BROKER_ORIENTATION_LANDSCAPE = 1
} VncBrokerDisplayOrientation;

typedef struct {
    uint32_t generation;
    uint32_t width;
    uint32_t height;
    VncBrokerDisplayMode mode;
    VncBrokerDisplayOrientation orientation;
} VncBrokerDisplayState;


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
 * its signalling/control messages use the broker-agent control channel.
 * Management authentication is a short-lived control-plane handoff with no
 * browser fd and never claims the machine-wide viewer slot.
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

/*
 * Typed control packets are used only after a transport-aware handoff. They
 * are length-delimited SOCK_SEQPACKET records, distinct from the legacy
 * one-byte VNC DONE/BUSY/REJECT status protocol.
 */
int vnc_broker_send_control(int control_fd,
                            VncBrokerControlType type,
                            const void *payload,
                            size_t payload_len);

int vnc_broker_recv_control(int control_fd,
                            VncBrokerControlType *type,
                            void *payload,
                            size_t payload_capacity,
                            size_t *payload_len);

int vnc_broker_send_web_auth_request(int control_fd,
                                     const char *username,
                                     const char *password);

int vnc_broker_recv_web_auth_request(int control_fd,
                                     VncBrokerWebAuthRequest *request);

void vnc_broker_web_auth_request_clear(VncBrokerWebAuthRequest *request);

int vnc_broker_send_web_auth_result(int control_fd,
                                    VncBrokerWebAuthResult result);

int vnc_broker_recv_web_auth_result(int control_fd,
                                    VncBrokerWebAuthResult *result);

int vnc_broker_send_device_bind(int control_fd, const char *device_id);
int vnc_broker_parse_device_bind(const void *payload,
                                 size_t payload_len,
                                 char device_id[VNC_BROKER_DEVICE_ID_HEX_LEN + 1]);

int vnc_broker_send_display_state(int control_fd,
                                  VncBrokerControlType type,
                                  const VncBrokerDisplayState *state);
int vnc_broker_parse_display_state(const void *payload,
                                   size_t payload_len,
                                   VncBrokerDisplayState *state);

int vnc_broker_send_video_frame_begin(int control_fd,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t jpeg_size);
int vnc_broker_parse_video_frame_begin(const void *payload,
                                       size_t payload_len,
                                       uint32_t *width,
                                       uint32_t *height,
                                       uint32_t *jpeg_size);
int vnc_broker_send_video_frame(int control_fd,
                                uint32_t width,
                                uint32_t height,
                                const void *jpeg,
                                size_t jpeg_size);

#endif
