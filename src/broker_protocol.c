#define _GNU_SOURCE

#include "broker_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int
vnc_broker_send_handoff(int control_fd,
                        int client_fd,
                        uid_t uid,
                        const char *session_id,
                        const char *peer_addr)
{
    VncBrokerHandoff handoff;
    memset(&handoff, 0, sizeof(handoff));

    handoff.magic = VNC_BROKER_PROTOCOL_MAGIC;
    handoff.version = VNC_BROKER_PROTOCOL_VERSION;
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
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(client_fd));

    ssize_t n;
    do {
        n = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof(handoff) ? 0 : -1;
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

    if (received_fd < 0 ||
        handoff->magic != VNC_BROKER_PROTOCOL_MAGIC ||
        handoff->version != VNC_BROKER_PROTOCOL_VERSION ||
        handoff->session_id[0] == '\0') {
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
