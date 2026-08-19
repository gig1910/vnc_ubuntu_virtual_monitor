#include "auth_client.h"
#include "io.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define AUTH_MAGIC "VMA1"
#define AUTH_MAX_USERNAME 255
#define AUTH_MAX_PASSWORD 4096

int
auth_client_check(
    const char *socket_path,
    const char *username,
    const char *password)
{
    if (!socket_path || !username || !password)
        return -1;

    size_t username_len = strlen(username);
    size_t password_len = strlen(password);

    if (
        username_len == 0 ||
        username_len > AUTH_MAX_USERNAME ||
        password_len > AUTH_MAX_PASSWORD
    ) {
        fprintf(stderr, "Invalid credential length for auth helper\n");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        perror("auth helper socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (
        snprintf(
            addr.sun_path,
            sizeof(addr.sun_path),
            "%s",
            socket_path
        ) >= (int)sizeof(addr.sun_path)
    ) {
        fprintf(stderr, "Auth helper socket path too long\n");
        close(fd);
        return -1;
    }

    if (
        connect(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0
    ) {
        perror("connect auth helper");
        close(fd);
        return -1;
    }

    uint8_t header[8] = {
        'V', 'M', 'A', '1',
        (uint8_t)(username_len >> 8),
        (uint8_t)username_len,
        (uint8_t)(password_len >> 8),
        (uint8_t)password_len
    };

    int result = -1;

    if (
        io_write_exact(fd, header, sizeof(header)) < 0 ||
        io_write_exact(fd, username, username_len) < 0 ||
        io_write_exact(fd, password, password_len) < 0
    ) {
        fprintf(stderr, "Failed sending credentials to auth helper\n");
        goto out;
    }

    uint8_t response = 2;

    if (io_read_exact(fd, &response, 1) < 0) {
        fprintf(stderr, "Auth helper closed without a result\n");
        goto out;
    }

    switch (response) {
        case 0:
            result = 0;
            break;
        case 1:
            result = 1;
            break;
        default:
            fprintf(stderr, "Auth helper internal error\n");
            result = -1;
            break;
    }

out:
    close(fd);
    return result;
}
