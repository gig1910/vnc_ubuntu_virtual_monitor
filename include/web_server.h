#ifndef VNC_MONITOR_WEB_SERVER_H
#define VNC_MONITOR_WEB_SERVER_H

#include <glib.h>
#include "broker_protocol.h"

#define VNC_WEB_PROTOCOL_VERSION              7u
#define VNC_WEB_PROTOCOL_VERSION_TEXT         "7"
#define VNC_WEB_JPEG_DECODE_FAILURE_LIMIT     3u

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
                                      const char *session_token,
                                      gpointer completion_data);

typedef struct {
    gboolean viewer_active;
    gboolean websocket_attached;
    gboolean web_protocol_ready;
    gboolean web_frame_in_flight;
    guint64 web_frames_forwarded;
    guint64 web_frames_acked;
    guint64 web_frames_nacked;
    char viewer_state[32];
    char viewer_transport[16];
    char viewer_peer[VNC_BROKER_PEER_ADDR_MAX];
    char viewer_user[128];
    char viewer_session_id[VNC_BROKER_SESSION_ID_MAX];

    gboolean active_user_available;
    guint active_uid;
    char active_user[128];
    char active_session_id[VNC_BROKER_SESSION_ID_MAX];

    int vnc_port;
} WebServerManagementInfo;

typedef struct {
    guint32 generation;
    guint32 width;
    guint32 height;
    VncBrokerDisplayMode mode;
    VncBrokerDisplayOrientation orientation;
} WebServerDisplayState;

typedef struct {
    const char *kind;
    const char *level;
    const char *event;
    guint64 seq;
    guint32 bytes;
    guint32 observed;
    guint32 checksum;
    guint32 elapsed_ms;
    const char *mime;
} WebServerClientDiagnostic;

typedef struct {
    gboolean (*slot_busy)(gpointer user_data);
    const char *(*slot_state)(gpointer user_data);

    /*
     * Start PAM-backed authentication for the exact browser peer. Password
     * ownership remains with the caller and is valid only for this call.
     *
     * Return WEB_SERVER_AUTH_STARTED when completion will be called later.
     * Any other result is an immediate terminal result.
     *
     * A successful asynchronous completion carries a short-lived opaque token
     * that web_server exposes only as an HttpOnly cookie.
     */
    WebServerAuthResult (*begin_auth)(const char *username,
                                      const char *password,
                                      const char *peer_addr,
                                      const char *device_id,
                                      WebServerAuthComplete completion,
                                      gpointer completion_data,
                                      gpointer user_data);

    /*
     * Validate the viewer token for WSS attach. The broker retains the token
     * only while the exact viewer session is active so same-origin HLS GETs
     * can authenticate with the same Secure HttpOnly cookie.
     */
    gboolean (*validate_websocket_token)(const char *token,
                                         gpointer user_data);
    gboolean (*bind_websocket)(const char *token,
                               gpointer user_data);
    gboolean (*validate_media_token)(const char *token,
                                     gpointer user_data);

    /*
     * Browser and broker must agree on the explicit browser wire protocol
     * before media starts. This version is independent of broker-agent IPC.
     */
    gboolean (*websocket_protocol_ready)(guint protocol,
                                         gpointer user_data);

    /*
     * Browser reports whether the in-flight WSS/JPEG reached the image
     * decoder. ACK resets the consecutive-failure counter. NACK drops one
     * independent frame and releases queue-depth=1 pacing unless the broker
     * reaches the consecutive decode-failure limit.
     */
    gboolean (*websocket_frame_ack)(gpointer user_data);
    gboolean (*websocket_frame_nack)(gpointer user_data);

    gboolean (*websocket_display_state)(
        const WebServerDisplayState *state,
        gpointer user_data);

    /*
     * Structured diagnostics from the authenticated browser. web_server
     * validates a fixed schema, allowlisted event name and rate limit before
     * invoking this hook; strings are temporary and valid only for the call.
     */
    void (*websocket_client_diagnostic)(
        const WebServerClientDiagnostic *diagnostic,
        gpointer user_data);

    /* Called only for the currently bound authenticated WebSocket. */
    void (*websocket_closed)(gpointer user_data);

    /*
     * Management authentication is independent of the viewer slot. It still
     * authenticates the exact active seat0 Unix user through the user agent.
     */
    WebServerAuthResult (*begin_management_auth)(const char *username,
                                                 const char *password,
                                                 const char *peer_addr,
                                                 WebServerAuthComplete completion,
                                                 gpointer completion_data,
                                                 gpointer user_data);
    gboolean (*validate_management_token)(const char *token,
                                          gpointer user_data);
    void (*management_logout)(gpointer user_data);
    int (*get_management_info)(WebServerManagementInfo *info,
                               gpointer user_data);
    gboolean (*disconnect_viewer)(gpointer user_data);
    void (*request_restart)(gpointer user_data);
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

/* Send signalling/status messages over the authenticated browser WSS. */
gboolean web_server_send_text(WebServer *server, const char *text);

/* Send one complete encoded video frame over the authenticated browser WSS. */
gboolean web_server_send_binary(WebServer *server,
                                const guint8 *data,
                                gsize length);

/* Set/clear the broker-readable per-user HLS directory for the active viewer. */
void web_server_set_hls_root(WebServer *server, const char *root);

/* Close the bound browser signalling socket, if one exists. */
void web_server_close_websocket(WebServer *server);

void web_server_stop(WebServer *server);

#endif
