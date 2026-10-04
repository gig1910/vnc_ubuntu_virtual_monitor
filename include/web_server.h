#ifndef VNC_MONITOR_WEB_SERVER_H
#define VNC_MONITOR_WEB_SERVER_H

#include <glib.h>

typedef struct WebServer WebServer;

typedef enum {
    /* begin_auth accepted the request and will complete it asynchronously. */
    WEB_SERVER_AUTH_STARTED = 0,
    WEB_SERVER_AUTH_OK,
    WEB_SERVER_AUTH_DENIED,
    WEB_SERVER_AUTH_BUSY,
    WEB_SERVER_AUTH_UNAVAILABLE,
    WEB_SERVER_AUTH_ERROR
} WebServerAuthResult;

typedef void (*WebServerAuthComplete)(WebServerAuthResult result,
                                      gpointer completion_data);

typedef struct {
    gboolean (*slot_busy)(gpointer user_data);
    const char *(*slot_state)(gpointer user_data);

    /*
     * Start PAM-backed authentication for the exact browser peer. Password
     * ownership remains with the caller and is valid only for this call.
     *
     * Return WEB_SERVER_AUTH_STARTED when completion will be called later.
     * Any other result is an immediate terminal result.
     */
    WebServerAuthResult (*begin_auth)(const char *username,
                                      const char *password,
                                      const char *peer_addr,
                                      WebServerAuthComplete completion,
                                      gpointer completion_data,
                                      gpointer user_data);
} WebServerHooks;

/*
 * Start the optional broker-owned HTTPS endpoint configured by [web] in the
 * machine-wide config file.
 *
 * Return values:
 *   1  HTTPS server started
 *   0  web endpoint disabled
 *  -1  endpoint was enabled but could not be configured/started
 */
int web_server_start(WebServer **out,
                     const char *config_file,
                     const WebServerHooks *hooks,
                     gpointer user_data);

void web_server_stop(WebServer *server);

#endif
