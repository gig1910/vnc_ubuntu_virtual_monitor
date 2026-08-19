#ifndef VNC_MONITOR_SHUTDOWN_SIGNAL_H
#define VNC_MONITOR_SHUTDOWN_SIGNAL_H

/*
 * Installs SIGINT/SIGTERM handlers backed by self-pipes.
 *
 * First signal requests graceful shutdown and wakes select()/poll() users.
 * A supervisor thread also shutdown(SHUT_RDWR)s all registered sockets so
 * blocking socket I/O wakes even when it is not using the helper functions.
 * A second SIGINT/SIGTERM uses _exit(128+signal) as an emergency escape hatch.
 */
int shutdown_signal_init(void);
void shutdown_signal_cleanup(void);

int shutdown_signal_fd(void);
int shutdown_signal_requested(void);
void shutdown_signal_drain(void);

int shutdown_signal_register_fd(int fd);
void shutdown_signal_unregister_fd(int fd);

#endif
