#define _GNU_SOURCE

#include "mutter_environment.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int
is_numeric_name(const char *name)
{
    if (!name || !*name)
        return 0;

    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isdigit(*p))
            return 0;
    }

    return 1;
}

static int
read_small_file(
    const char *path,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    if (!path || !buffer || capacity < 2)
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    size_t used = 0;

    while (used + 1 < capacity) {
        ssize_t n = read(fd, buffer + used, capacity - used - 1);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            close(fd);
            return -1;
        }

        if (n == 0)
            break;

        used += (size_t)n;
    }

    close(fd);
    buffer[used] = '\0';

    if (length_out)
        *length_out = used;

    return 0;
}

static int
value_is_true(const char *value)
{
    if (!value)
        return 0;

    return
        strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0;
}

static int
probe_gnome_shell(
    pid_t *pid_out,
    int *variable_present_out,
    int *hw_cursor_disabled_out,
    char *raw_value,
    size_t raw_value_capacity)
{
    DIR *proc = opendir("/proc");

    if (!proc)
        return -1;

    uid_t uid = geteuid();
    int result = -1;
    struct dirent *entry;

    while ((entry = readdir(proc)) != NULL) {
        if (!is_numeric_name(entry->d_name))
            continue;

        char path[256];
        struct stat st;
        long pid_value = strtol(entry->d_name, NULL, 10);

        snprintf(path, sizeof(path), "/proc/%ld", pid_value);

        if (stat(path, &st) < 0 || st.st_uid != uid)
            continue;

        char comm[64];
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid_value);

        if (read_small_file(path, comm, sizeof(comm), NULL) < 0)
            continue;

        comm[strcspn(comm, "\r\n")] = '\0';

        if (strcmp(comm, "gnome-shell") != 0)
            continue;

        char environ[65536];
        size_t environ_len = 0;
        snprintf(path, sizeof(path), "/proc/%ld/environ", pid_value);

        if (
            read_small_file(
                path,
                environ,
                sizeof(environ),
                &environ_len
            ) < 0
        )
            continue;

        const char key[] = "MUTTER_DEBUG_DISABLE_HW_CURSORS=";
        const size_t key_len = sizeof(key) - 1;
        const char *found = NULL;

        size_t pos = 0;
        while (pos < environ_len) {
            const char *item = environ + pos;
            size_t remaining = environ_len - pos;
            size_t item_len = strnlen(item, remaining);

            if (
                item_len >= key_len &&
                memcmp(item, key, key_len) == 0
            ) {
                found = item + key_len;
                break;
            }

            if (item_len == remaining)
                break;

            pos += item_len + 1;
        }

        if (pid_out)
            *pid_out = (pid_t)pid_value;

        if (variable_present_out)
            *variable_present_out = found != NULL;

        if (hw_cursor_disabled_out)
            *hw_cursor_disabled_out = found && value_is_true(found);

        if (raw_value && raw_value_capacity > 0) {
            snprintf(
                raw_value,
                raw_value_capacity,
                "%s",
                found ? found : "<unset>"
            );
        }

        result = 0;
        break;
    }

    closedir(proc);
    return result;
}

int
mutter_environment_log_hardware_cursor(
    MutterHardwareCursorMode requested_mode)
{
    pid_t pid = 0;
    int variable_present = 0;
    int disabled = 0;
    char raw_value[64];

    if (
        probe_gnome_shell(
            &pid,
            &variable_present,
            &disabled,
            raw_value,
            sizeof(raw_value)
        ) < 0
    ) {
        fprintf(
            stderr,
            "Mutter HW cursor env: unable to inspect running gnome-shell\n"
        );
        return -1;
    }

    const char *effective =
        disabled
            ? "disabled"
            : variable_present
                ? "nonstandard/unknown"
                : "enabled/default";

    printf(
        "Mutter HW cursor env: %s (gnome-shell pid=%ld, "
        "MUTTER_DEBUG_DISABLE_HW_CURSORS=%s)\n",
        effective,
        (long)pid,
        raw_value
    );

    if (
        requested_mode == MUTTER_HW_CURSOR_DISABLED &&
        !disabled
    ) {
        fprintf(
            stderr,
            "WARNING: --mutter-hardware-cursor disabled was requested, "
            "but the running gnome-shell was not started with "
            "MUTTER_DEBUG_DISABLE_HW_CURSORS=1. Restart the GNOME session "
            "after exporting that variable.\n"
        );
    }
    else if (
        requested_mode == MUTTER_HW_CURSOR_ENABLED &&
        variable_present
    ) {
        fprintf(
            stderr,
            "WARNING: --mutter-hardware-cursor enabled was requested, "
            "but the running gnome-shell has hardware cursors disabled. "
            "Remove MUTTER_DEBUG_DISABLE_HW_CURSORS and restart the GNOME "
            "session to test enabled hardware cursors.\n"
        );
    }
    else if (requested_mode != MUTTER_HW_CURSOR_AUTO) {
        printf("Mutter HW cursor request: confirmed\n");
    }

    return 0;
}
