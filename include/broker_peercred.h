#ifndef VNC_MONITOR_BROKER_PEERCRED_H
#define VNC_MONITOR_BROKER_PEERCRED_H

/*
 * systemd user managers may run services in a user namespace that maps only
 * the desktop user's UID. In that namespace, host root from the system broker
 * is reported by SO_PEERCRED as the overflow UID rather than 0.
 *
 * main.c is compiled with this header force-included so only its getsockopt()
 * calls pass through the compatibility wrapper. The wrapper delegates every
 * option unchanged except SO_PEERCRED, where a namespace-mapped UID is
 * normalized to root only after the peer PID is verified to belong exactly to
 * /system.slice/vnc-monitor-broker.service.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/socket.h>

int vnc_broker_peercred_getsockopt(int fd,
                                   int level,
                                   int option_name,
                                   void *option_value,
                                   socklen_t *option_len);

#define getsockopt vnc_broker_peercred_getsockopt

#endif
