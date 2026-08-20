#define _GNU_SOURCE

#include "broker_protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
} LegacyBrokerHandoff;

_Static_assert(sizeof(LegacyBrokerHandoff) == sizeof(VncBrokerHandoff),
               "broker handoff wire size changed");

static int failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s (errno=%d: %s)\n",          \
                    __FILE__, __LINE__, #expr, errno, strerror(errno));     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static int
make_seqpacket_pair(int fds[2])
{
    return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds);
}

static int
make_stream_pair(int fds[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
}

static int
send_legacy_handoff(int control_fd,
                    int client_fd,
                    uid_t uid,
                    const char *session_id,
                    const char *peer_addr)
{
    LegacyBrokerHandoff handoff;
    memset(&handoff, 0, sizeof(handoff));

    handoff.magic = VNC_BROKER_PROTOCOL_MAGIC;
    handoff.version = VNC_BROKER_PROTOCOL_VERSION;
    handoff.reserved = 0; /* beta.3 */
    handoff.uid = (uint32_t)uid;
    snprintf(handoff.session_id, sizeof(handoff.session_id), "%s", session_id);
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

    ssize_t n = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    return n == (ssize_t)sizeof(handoff) ? 0 : -1;
}

/*
 * Deliberately models the beta.3 receiver: the former reserved field is read
 * but never interpreted. A new VNC broker may therefore place transport=1 in
 * those same two bytes without breaking an old agent.
 */
static int
recv_legacy_handoff(int control_fd,
                    int *client_fd,
                    LegacyBrokerHandoff *handoff)
{
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

    ssize_t n = recvmsg(control_fd, &msg, MSG_CMSG_CLOEXEC);
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
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
        }
    }

    /* Exactly the beta.3 semantic checks: reserved is intentionally ignored. */
    if (received_fd < 0 ||
        handoff->magic != VNC_BROKER_PROTOCOL_MAGIC ||
        handoff->version != VNC_BROKER_PROTOCOL_VERSION ||
        handoff->session_id[0] == '\0') {
        if (received_fd >= 0)
            close(received_fd);
        errno = EPROTO;
        return -1;
    }

    *client_fd = received_fd;
    return 0;
}

static void
verify_fd_roundtrip(int received_fd, int peer_fd)
{
    const unsigned char sent = 0x5a;
    unsigned char got = 0;

    CHECK(write(peer_fd, &sent, 1) == 1);
    CHECK(read(received_fd, &got, 1) == 1);
    CHECK(got == sent);
}

static void
test_legacy_broker_to_new_agent(void)
{
    int control[2] = {-1, -1};
    int data[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);
    CHECK(make_stream_pair(data) == 0);

    CHECK(send_legacy_handoff(control[0], data[0], 1000, "c2", "192.0.2.10") == 0);

    VncBrokerHandoff handoff;
    int received_fd = -1;
    CHECK(vnc_broker_recv_handoff(control[1], &received_fd, &handoff) == 0);
    CHECK(received_fd >= 0);
    CHECK(handoff.transport == VNC_BROKER_TRANSPORT_LEGACY_VNC);
    CHECK(handoff.uid == 1000);
    CHECK(strcmp(handoff.session_id, "c2") == 0);
    CHECK(strcmp(handoff.peer_addr, "192.0.2.10") == 0);

    if (received_fd >= 0) {
        verify_fd_roundtrip(received_fd, data[1]);
        close(received_fd);
    }

    close(data[0]);
    close(data[1]);
    close(control[0]);
    close(control[1]);
}

static void
test_new_broker_to_legacy_agent(void)
{
    int control[2] = {-1, -1};
    int data[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);
    CHECK(make_stream_pair(data) == 0);

    CHECK(vnc_broker_send_handoff(control[0],
                                  data[0],
                                  1001,
                                  "c7",
                                  "198.51.100.20") == 0);

    LegacyBrokerHandoff handoff;
    int received_fd = -1;
    CHECK(recv_legacy_handoff(control[1], &received_fd, &handoff) == 0);
    CHECK(received_fd >= 0);

    /* Old code ignores this field; observing 1 here proves same wire location. */
    CHECK(handoff.reserved == VNC_BROKER_TRANSPORT_VNC);
    CHECK(handoff.uid == 1001);
    CHECK(strcmp(handoff.session_id, "c7") == 0);
    CHECK(strcmp(handoff.peer_addr, "198.51.100.20") == 0);

    if (received_fd >= 0) {
        verify_fd_roundtrip(received_fd, data[1]);
        close(received_fd);
    }

    close(data[0]);
    close(data[1]);
    close(control[0]);
    close(control[1]);
}

static void
test_webrtc_handoff_has_no_fd(void)
{
    int control[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);

    CHECK(vnc_broker_send_handoff_transport(control[0],
                                             VNC_BROKER_TRANSPORT_WEBRTC,
                                             -1,
                                             1000,
                                             "c9",
                                             "203.0.113.30") == 0);

    VncBrokerHandoff handoff;
    int received_fd = 123;
    CHECK(vnc_broker_recv_handoff(control[1], &received_fd, &handoff) == 0);
    CHECK(received_fd == -1);
    CHECK(handoff.transport == VNC_BROKER_TRANSPORT_WEBRTC);

    close(control[0]);
    close(control[1]);
}

static void
test_transport_fd_contract(void)
{
    int control[2] = {-1, -1};
    int data[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);
    CHECK(make_stream_pair(data) == 0);

    errno = 0;
    CHECK(vnc_broker_send_handoff_transport(control[0],
                                             VNC_BROKER_TRANSPORT_VNC,
                                             -1,
                                             1000,
                                             "c1",
                                             "peer") < 0);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(vnc_broker_send_handoff_transport(control[0],
                                             VNC_BROKER_TRANSPORT_WEBRTC,
                                             data[0],
                                             1000,
                                             "c1",
                                             "peer") < 0);
    CHECK(errno == EINVAL);

    close(data[0]);
    close(data[1]);
    close(control[0]);
    close(control[1]);
}

static void
test_web_auth_control_roundtrip(void)
{
    int control[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);

    CHECK(vnc_broker_send_web_auth_request(control[0], "test-user", "s3cret") == 0);

    VncBrokerWebAuthRequest request;
    CHECK(vnc_broker_recv_web_auth_request(control[1], &request) == 0);
    CHECK(request.username != NULL);
    CHECK(request.password != NULL);
    if (request.username)
        CHECK(strcmp(request.username, "test-user") == 0);
    if (request.password)
        CHECK(strcmp(request.password, "s3cret") == 0);
    CHECK(request.username_len == strlen("test-user"));
    CHECK(request.password_len == strlen("s3cret"));
    vnc_broker_web_auth_request_clear(&request);

    CHECK(vnc_broker_send_web_auth_result(control[1], VNC_BROKER_WEB_AUTH_OK) == 0);
    VncBrokerWebAuthResult result = VNC_BROKER_WEB_AUTH_ERROR;
    CHECK(vnc_broker_recv_web_auth_result(control[0], &result) == 0);
    CHECK(result == VNC_BROKER_WEB_AUTH_OK);

    close(control[0]);
    close(control[1]);
}

static void
test_web_auth_limits(void)
{
    int control[2] = {-1, -1};
    CHECK(make_seqpacket_pair(control) == 0);

    char valid_username[VNC_BROKER_AUTH_USERNAME_MAX + 1];
    memset(valid_username, 'u', sizeof(valid_username) - 1);
    valid_username[sizeof(valid_username) - 1] = '\0';

    char *valid_password = malloc(VNC_BROKER_AUTH_PASSWORD_MAX + 1);
    CHECK(valid_password != NULL);
    if (valid_password) {
        memset(valid_password, 'p', VNC_BROKER_AUTH_PASSWORD_MAX);
        valid_password[VNC_BROKER_AUTH_PASSWORD_MAX] = '\0';
        CHECK(vnc_broker_send_web_auth_request(control[0],
                                                valid_username,
                                                valid_password) == 0);

        VncBrokerWebAuthRequest request;
        CHECK(vnc_broker_recv_web_auth_request(control[1], &request) == 0);
        CHECK(request.username_len == VNC_BROKER_AUTH_USERNAME_MAX);
        CHECK(request.password_len == VNC_BROKER_AUTH_PASSWORD_MAX);
        vnc_broker_web_auth_request_clear(&request);
        memset(valid_password, 0, VNC_BROKER_AUTH_PASSWORD_MAX + 1);
        free(valid_password);
    }

    char invalid_username[VNC_BROKER_AUTH_USERNAME_MAX + 2];
    memset(invalid_username, 'x', sizeof(invalid_username) - 1);
    invalid_username[sizeof(invalid_username) - 1] = '\0';

    errno = 0;
    CHECK(vnc_broker_send_web_auth_request(control[0], invalid_username, "x") < 0);
    CHECK(errno == EINVAL);

    close(control[0]);
    close(control[1]);
}

int
main(void)
{
    test_legacy_broker_to_new_agent();
    test_new_broker_to_legacy_agent();
    test_webrtc_handoff_has_no_fd();
    test_transport_fd_contract();
    test_web_auth_control_roundtrip();
    test_web_auth_limits();

    if (failures != 0) {
        fprintf(stderr, "broker_protocol_test: %d failure(s)\n", failures);
        return 1;
    }

    printf("broker_protocol_test: OK\n");
    return 0;
}
