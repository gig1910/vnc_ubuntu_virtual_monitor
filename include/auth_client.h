#ifndef VNC_MONITOR_AUTH_CLIENT_H
#define VNC_MONITOR_AUTH_CLIENT_H

/*
 * Returns:
 *   1 = authenticated
 *   0 = denied
 *  -1 = helper/protocol error
 */
int auth_client_check(
    const char *socket_path,
    const char *username,
    const char *password);

#endif
