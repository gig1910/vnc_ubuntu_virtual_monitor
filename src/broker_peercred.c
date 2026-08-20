#define _GNU_SOURCE

#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BROKER_CGROUP_PATH "/system.slice/vnc-monitor-broker.service"

static int
pid_is_system_broker(pid_t pid)
{
    if (pid <= 0)
        return 0;

    char proc_path[64];
    int n = snprintf(proc_path,
                     sizeof(proc_path),
                     "/proc/%ld/cgroup",
                     (long)pid);
    if (n < 0 || (size_t)n >= sizeof(proc_path))
        return 0;

    FILE *fp = fopen(proc_path, "re");
    if (!fp)
        return 0;

    char line[512];
    int trusted = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *first = strchr(line, ':');
        if (!first)
            continue;

        char *second = strchr(first + 1, ':');
        if (!second)
            continue;

        char *cgroup_path = second + 1;
        cgroup_path[strcspn(cgroup_path, "\r\n")] = '\0';

        if (strcmp(cgroup_path, BROKER_CGROUP_PATH) == 0) {
            trusted = 1;
            break;
        }
    }

    fclose(fp);
    return trusted;
}

int
vnc_broker_peercred_getsockopt(int fd,
                               int level,
                               int option_name,
                               void *option_value,
                               socklen_t *option_len)
{
    int rc = getsockopt(fd,
                        level,
                        option_name,
                        option_value,
                        option_len);
    if (rc < 0)
        return rc;

    if (level != SOL_SOCKET ||
        option_name != SO_PEERCRED ||
        !option_value ||
        !option_len ||
        *option_len < sizeof(struct ucred)) {
        return 0;
    }

    struct ucred *peer = option_value;

    /* Host-root is directly visible in namespaces that map UID 0. */
    if (peer->uid == 0)
        return 0;

    /*
     * A systemd user service can have a user namespace containing only its
     * own UID. Host-root then appears as overflowuid (normally 65534), exactly
     * like every other unmapped host UID. Never trust that UID by itself.
     *
     * SO_PEERCRED also gives us the immutable peer PID for this connection.
     * Only a process in the root-managed broker service cgroup is accepted as
     * the system broker; arbitrary desktop users cannot move themselves into
     * /system.slice/vnc-monitor-broker.service.
     */
    if (pid_is_system_broker(peer->pid)) {
        LOG_DEBUG("Trusted namespace-mapped broker peer: pid=%ld mapped-uid=%lu cgroup=%s",
                  (long)peer->pid,
                  (unsigned long)peer->uid,
                  BROKER_CGROUP_PATH);
        peer->uid = 0;
    }
    else {
        LOG_DEBUG("Untrusted SO_PEERCRED peer: pid=%ld uid=%lu is not in %s",
                  (long)peer->pid,
                  (unsigned long)peer->uid,
                  BROKER_CGROUP_PATH);
    }

    return 0;
}
