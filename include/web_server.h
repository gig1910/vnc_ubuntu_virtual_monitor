#ifndef VNC_MONITOR_WEB_SERVER_H
#define VNC_MONITOR_WEB_SERVER_H

#include <glib.h>

typedef struct WebServer WebServer;

typedef struct {
    gboolean (*slot_busy)(gpointer user_data);
    const char *(*slot_state)(gpointer user_data);
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
