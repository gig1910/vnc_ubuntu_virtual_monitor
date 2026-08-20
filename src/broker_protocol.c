#define _GNU_SOURCE

#include "broker_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_len;
} VncBrokerControlHeader;

static void
secure_clear(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;

#if defined(__GLIBC__)
    explicit_bzero(buf, len);
#else
    volatile unsigned char *p = buf;
    while (len--)
        *p++ = 0;
#endif
}

static uint16_t
get_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void
put_u16_be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static int
transport_is_vnc(uint16_t transport)
{
    return transport == VNC_BROKER_TRANSPORT_LEGACY_VNC ||
           transport == VNC_BROKER_TRANSPORT_VNC;
}

const char *
vnc_broker_transport_name(uint16_t transport)
{
    switch (transport) {
        case VNC_BROKER_TRANSPORT_LEGACY_VNC:
        case VNC_BROKER_TRANSPORT_VNC:
            return "vnc";
        case VNC_BROKER_TRANSPORT_WEBRTC:
            return "webrtc";
        default:
            return "unknown";
    }
}

int
vnc_broker_send_handoff_transport(int control_fd,
                                  VncBrokerTransport transport,
                                  int client_fd,
                                  uid_t uid,
                                  const char *session_id,
                                  const char *peer_addr)
{
    if (transport != VNC_BROKER_TRANSPORT_VNC &&
        transport != VNC_BROKER_TRANSPORT_WEBRTC) {
        errno = EINVAL;
        return -1;
    }

    if ((transport == VNC_BROKER_TRANSPORT_VNC && client_fd < 0) ||
        (transport == VNC_BROKER_TRANSPORT_WEBRTC && client_fd >= 0)) {
        errno = EINVAL;
        return -1;
    }

    VncBrokerHandoff handoff;
    memset(&handoff, 0, sizeof(handoff));

    handoff.magic = VNC_BROKER_PROTOCOL_MAGIC;
    handoff.version = VNC_BROKER_PROTOCOL_VERSION;
    handoff.transport = (uint16_t)transport;
    handoff.uid = (uint32_t)uid;

    if (session_id)
        snprintf(handoff.session_id, sizeof(handoff.session_id), "%s", session_id);
    if (peer_addr)
        snprintf(handoff.peer_addr, sizeof(handoff.peer_addr), "%s", peer_addr);

    struct iovec iov = {
        .iov_base = &handoff,
        .iov_len = sizeof(handoff)
    };

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (client_fd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(client_fd));
    }

    ssize_t n;
    do {
        n = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof(handoff) ? 0 : -1;
}

int
vnc_broker_send_handoff(int control_fd,
                        int client_fd,
                        uid_t uid,
                        const char *session_id,
                        const char *peer_addr)
{
    return vnc_broker_send_handoff_transport(control_fd,
                                             VNC_BROKER_TRANSPORT_VNC,
                                             client_fd,
                                             uid,
                                             session_id,
                                             peer_addr);
}

int
vnc_broker_recv_handoff(int control_fd,
                        int *client_fd,
                        VncBrokerHandoff *handoff)
{
    if (!client_fd || !handoff) {
        errno = EINVAL;
        return -1;
    }

    *client_fd = -1;
    memset(handoff, 0, sizeof(*handoff));

    struct iovec iov = {
        .iov_base = handoff,
        .iov_len = sizeof(*handoff)
    };

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(control_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof(*handoff) ||
        (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        errno = EPROTO;
        return -1;
    }

    int received_fd = -1;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS) {
            if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) || received_fd >= 0) {
                errno = EPROTO;
                goto fail;
            }

            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
        }
    }

    if (handoff->magic != VNC_BROKER_PROTOCOL_MAGIC ||
        handoff->version != VNC_BROKER_PROTOCOL_VERSION ||
        handoff->session_id[0] == '\0' ||
        (!transport_is_vnc(handoff->transport) &&
         handoff->transport != VNC_BROKER_TRANSPORT_WEBRTC)) {
        errno = EPROTO;
        goto fail;
    }

    if ((transport_is_vnc(handoff->transport) && received_fd < 0) ||
        (handoff->transport == VNC_BROKER_TRANSPORT_WEBRTC && received_fd >= 0)) {
        errno = EPROTO;
        goto fail;
    }

    *client_fd = received_fd;
    return 0;

fail:
    if (received_fd >= 0)
        close(received_fd);
    return -1;
}

int
vnc_broker_send_status(int control_fd, uint8_t status)
{
    ssize_t n;
    do {
        n = send(control_fd, &status, sizeof(status), MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof(status) ? 0 : -1;
}

int
vnc_broker_recv_status(int control_fd, uint8_t *status)
{
    if (!status) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do {
        n = recv(control_fd, status, sizeof(*status), 0);
    } while (n < 0 && errno == EINTR);

    if (n == 0) {
        errno = ECONNRESET;
        return -1;
    }

    return n == (ssize_t)sizeof(*status) ? 0 : -1;
}

int
vnc_broker_send_control(int control_fd,
                        VncBrokerControlType type,
                        const void *payload,
                        size_t payload_len)
{
    if (control_fd < 0 || type <= 0 ||
        payload_len > VNC_BROKER_CONTROL_PAYLOAD_MAX ||
        (payload_len > 0 && !payload)) {
        errno = EINVAL;
        return -1;
    }

    VncBrokerControlHeader header = {
        .magic = VNC_BROKER_CONTROL_MAGIC,
        .version = VNC_BROKER_CONTROL_VERSION,
        .type = (uint16_t)type,
        .payload_len = (uint32_t)payload_len
    };

    struct iovec iov[2] = {
        {
            .iov_base = &header,
            .iov_len = sizeof(header)
        },
        {
            .iov_base = (void *)payload,
            .iov_len = payload_len
        }
    };

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = payload_len > 0 ? 2 : 1;

    ssize_t expected = (ssize_t)(sizeof(header) + payload_len);
    ssize_t n;
    do {
        n = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == expected ? 0 : -1;
}

int
vnc_broker_recv_control(int control_fd,
                        VncBrokerControlType *type,
                        void *payload,
                        size_t payload_capacity,
                        size_t *payload_len)
{
    if (control_fd < 0 || !type || !payload_len ||
        payload_capacity > VNC_BROKER_CONTROL_PAYLOAD_MAX ||
        (payload_capacity > 0 && !payload)) {
        errno = EINVAL;
        return -1;
    }

    uint8_t packet[sizeof(VncBrokerControlHeader) + VNC_BROKER_CONTROL_PAYLOAD_MAX];
    struct iovec iov = {
        .iov_base = packet,
        .iov_len = sizeof(packet)
    };

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t n;
    do {
        n = recvmsg(control_fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n <= 0) {
        if (n == 0)
            errno = ECONNRESET;
        return -1;
    }

    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        (size_t)n < sizeof(VncBrokerControlHeader)) {
        errno = EPROTO;
        return -1;
    }

    VncBrokerControlHeader header;
    memcpy(&header, packet, sizeof(header));

    if (header.magic != VNC_BROKER_CONTROL_MAGIC ||
        header.version != VNC_BROKER_CONTROL_VERSION ||
        header.type == 0 ||
        header.payload_len > VNC_BROKER_CONTROL_PAYLOAD_MAX ||
        (size_t)n != sizeof(header) + (size_t)header.payload_len) {
        errno = EPROTO;
        return -1;
    }

    if ((size_t)header.payload_len > payload_capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    if (header.payload_len > 0)
        memcpy(payload, packet + sizeof(header), header.payload_len);

    *type = (VncBrokerControlType)header.type;
    *payload_len = header.payload_len;
    return 0;
}

int
vnc_broker_send_web_auth_request(int control_fd,
                                 const char *username,
                                 const char *password)
{
    if (!username || !password) {
        errno = EINVAL;
        return -1;
    }

    size_t username_len = strlen(username);
    size_t password_len = strlen(password);

    if (username_len == 0 || username_len > VNC_BROKER_AUTH_USERNAME_MAX ||
        password_len > VNC_BROKER_AUTH_PASSWORD_MAX) {
        errno = EINVAL;
        return -1;
    }

    uint8_t payload[4 + VNC_BROKER_AUTH_USERNAME_MAX + VNC_BROKER_AUTH_PASSWORD_MAX];
    size_t payload_len = 4 + username_len + password_len;

    put_u16_be(payload, (uint16_t)username_len);
    put_u16_be(payload + 2, (uint16_t)password_len);
    memcpy(payload + 4, username, username_len);
    if (password_len > 0)
        memcpy(payload + 4 + username_len, password, password_len);

    int rc = vnc_broker_send_control(control_fd,
                                     VNC_BROKER_CONTROL_WEB_AUTH_REQUEST,
                                     payload,
                                     payload_len);

    secure_clear(payload, payload_len);
    return rc;
}

int
vnc_broker_recv_web_auth_request(int control_fd,
                                 VncBrokerWebAuthRequest *request)
{
    if (!request) {
        errno = EINVAL;
        return -1;
    }

    memset(request, 0, sizeof(*request));

    uint8_t payload[4 + VNC_BROKER_AUTH_USERNAME_MAX + VNC_BROKER_AUTH_PASSWORD_MAX];
    VncBrokerControlType type;
    size_t payload_len = 0;

    int rc = vnc_broker_recv_control(control_fd,
                                     &type,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (rc < 0)
        return -1;

    if (type != VNC_BROKER_CONTROL_WEB_AUTH_REQUEST || payload_len < 4) {
        secure_clear(payload, payload_len);
        errno = EPROTO;
        return -1;
    }

    size_t username_len = get_u16_be(payload);
    size_t password_len = get_u16_be(payload + 2);

    if (username_len == 0 || username_len > VNC_BROKER_AUTH_USERNAME_MAX ||
        password_len > VNC_BROKER_AUTH_PASSWORD_MAX ||
        payload_len != 4 + username_len + password_len) {
        secure_clear(payload, payload_len);
        errno = EPROTO;
        return -1;
    }

    request->username = calloc(username_len + 1, 1);
    request->password = calloc(password_len + 1, 1);
    if (!request->username || !request->password) {
        secure_clear(payload, payload_len);
        vnc_broker_web_auth_request_clear(request);
        errno = ENOMEM;
        return -1;
    }

    memcpy(request->username, payload + 4, username_len);
    if (password_len > 0)
        memcpy(request->password, payload + 4 + username_len, password_len);

    request->username_len = username_len;
    request->password_len = password_len;
    secure_clear(payload, payload_len);
    return 0;
}

void
vnc_broker_web_auth_request_clear(VncBrokerWebAuthRequest *request)
{
    if (!request)
        return;

    if (request->password) {
        secure_clear(request->password, request->password_len + 1);
        free(request->password);
    }

    if (request->username) {
        secure_clear(request->username, request->username_len + 1);
        free(request->username);
    }

    memset(request, 0, sizeof(*request));
}

int
vnc_broker_send_web_auth_result(int control_fd,
                                VncBrokerWebAuthResult result)
{
    if (result != VNC_BROKER_WEB_AUTH_DENIED &&
        result != VNC_BROKER_WEB_AUTH_OK &&
        result != VNC_BROKER_WEB_AUTH_ERROR) {
        errno = EINVAL;
        return -1;
    }

    uint8_t payload = (uint8_t)result;
    return vnc_broker_send_control(control_fd,
                                   VNC_BROKER_CONTROL_WEB_AUTH_RESULT,
                                   &payload,
                                   sizeof(payload));
}

int
vnc_broker_recv_web_auth_result(int control_fd,
                                VncBrokerWebAuthResult *result)
{
    if (!result) {
        errno = EINVAL;
        return -1;
    }

    uint8_t payload = 0xff;
    VncBrokerControlType type;
    size_t payload_len = 0;

    if (vnc_broker_recv_control(control_fd,
                                &type,
                                &payload,
                                sizeof(payload),
                                &payload_len) < 0) {
        return -1;
    }

    if (type != VNC_BROKER_CONTROL_WEB_AUTH_RESULT || payload_len != 1 ||
        (payload != VNC_BROKER_WEB_AUTH_DENIED &&
         payload != VNC_BROKER_WEB_AUTH_OK &&
         payload != VNC_BROKER_WEB_AUTH_ERROR)) {
        errno = EPROTO;
        return -1;
    }

    *result = (VncBrokerWebAuthResult)payload;
    return 0;
}
