#define _GNU_SOURCE

#include <security/pam_appl.h>

#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PAM_SERVICE_NAME "vnc-monitor"

#define AUTH_MAGIC "VMA1"
#define MAX_USERNAME 255
#define MAX_PASSWORD 4096

enum {
    AUTH_DENIED = 0,
    AUTH_OK = 1,
    AUTH_ERROR = 2
};

typedef struct {
    const char *username;
    const char *password;
} PamCredentials;

static void
secure_clear(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;

#if defined(__GLIBC__)
    explicit_bzero(buf, len);
#else
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--)
        *p++ = 0;
#endif
}

static int
read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static int
write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static uint16_t
get_u16_be(const uint8_t p[2])
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static int
pam_conversation(
    int num_msg,
    const struct pam_message **msg,
    struct pam_response **resp,
    void *appdata_ptr)
{
    if (num_msg <= 0 || !msg || !resp || !appdata_ptr)
        return PAM_CONV_ERR;

    PamCredentials *credentials = appdata_ptr;

    struct pam_response *responses =
        calloc((size_t)num_msg, sizeof(*responses));

    if (!responses)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        if (!msg[i])
            goto fail;

        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                responses[i].resp = strdup(credentials->password);
                if (!responses[i].resp)
                    goto fail;
                break;

            case PAM_PROMPT_ECHO_ON:
                responses[i].resp = strdup(credentials->username);
                if (!responses[i].resp)
                    goto fail;
                break;

            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                responses[i].resp = NULL;
                break;

            default:
                goto fail;
        }

        responses[i].resp_retcode = 0;
    }

    *resp = responses;
    return PAM_SUCCESS;

fail:
    for (int i = 0; i < num_msg; i++) {
        if (responses[i].resp) {
            secure_clear(responses[i].resp, strlen(responses[i].resp));
            free(responses[i].resp);
        }
    }

    free(responses);
    return PAM_CONV_ERR;
}

static int
authenticate_with_pam(
    const char *username,
    const char *password)
{
    PamCredentials credentials = {
        .username = username,
        .password = password
    };

    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = &credentials
    };

    pam_handle_t *pamh = NULL;

    int rc = pam_start(
        PAM_SERVICE_NAME,
        username,
        &conv,
        &pamh
    );

    if (rc != PAM_SUCCESS) {
        fprintf(stderr, "pam_start failed: code %d\n", rc);
        if (pamh)
            pam_end(pamh, rc);
        return AUTH_ERROR;
    }

    rc = pam_authenticate(pamh, 0);

    if (rc == PAM_SUCCESS)
        rc = pam_acct_mgmt(pamh, 0);

    if (rc != PAM_SUCCESS) {
        fprintf(
            stderr,
            "Authentication denied for user \"%s\": %s\n",
            username,
            pam_strerror(pamh, rc)
        );

        pam_end(pamh, rc);
        return AUTH_DENIED;
    }

    pam_end(pamh, PAM_SUCCESS);

    fprintf(
        stderr,
        "Authentication successful for user \"%s\"\n",
        username
    );

    return AUTH_OK;
}

int
main(void)
{
    const int fd = STDIN_FILENO;

    struct ucred peer = {0};
    socklen_t peer_len = sizeof(peer);

    if (
        getsockopt(
            fd,
            SOL_SOCKET,
            SO_PEERCRED,
            &peer,
            &peer_len
        ) < 0
    ) {
        perror("SO_PEERCRED");
        return 1;
    }

    if (peer.uid == 0) {
        fprintf(stderr, "Refusing root peer\n");
        return 1;
    }

    struct passwd *peer_pw = getpwuid(peer.uid);

    if (!peer_pw || !peer_pw->pw_name) {
        fprintf(
            stderr,
            "Cannot resolve peer uid %lu\n",
            (unsigned long)peer.uid
        );

        return 1;
    }

    uint8_t header[8];

    if (read_exact(fd, header, sizeof(header)) < 0) {
        fprintf(stderr, "Short authentication request\n");
        return 1;
    }

    if (memcmp(header, AUTH_MAGIC, 4) != 0) {
        fprintf(stderr, "Invalid authentication request magic\n");
        return 1;
    }

    uint16_t username_len = get_u16_be(header + 4);
    uint16_t password_len = get_u16_be(header + 6);

    if (
        username_len == 0 ||
        username_len > MAX_USERNAME ||
        password_len > MAX_PASSWORD
    ) {
        fprintf(stderr, "Invalid credential lengths\n");
        return 1;
    }

    char *username = calloc((size_t)username_len + 1, 1);
    char *password = calloc((size_t)password_len + 1, 1);

    if (!username || !password) {
        free(username);
        free(password);
        return 1;
    }

    int result = AUTH_ERROR;

    if (
        read_exact(fd, username, username_len) < 0 ||
        read_exact(fd, password, password_len) < 0
    ) {
        fprintf(stderr, "Short credential payload\n");
        goto out;
    }

    /*
     * Critical authorization boundary:
     * the root helper may authenticate only the Unix account
     * that owns the connecting local process.
     */
    if (strcmp(username, peer_pw->pw_name) != 0) {
        fprintf(
            stderr,
            "Rejected peer uid=%lu: requested user \"%s\" "
            "does not match peer account \"%s\"\n",
            (unsigned long)peer.uid,
            username,
            peer_pw->pw_name
        );

        result = AUTH_DENIED;
        goto out;
    }

    result = authenticate_with_pam(username, password);

out:
    /*
     * One-byte response:
     * 0 = denied, 1 = success, 2 = internal/protocol error.
     */
    uint8_t response = (uint8_t)result;
    (void)write_exact(fd, &response, 1);

    secure_clear(password, (size_t)password_len + 1);
    secure_clear(username, (size_t)username_len + 1);

    free(password);
    free(username);

    return result == AUTH_ERROR ? 1 : 0;
}
