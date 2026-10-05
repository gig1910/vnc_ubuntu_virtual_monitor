#define _GNU_SOURCE

#include "broker_protocol.h"
#include "config.h"
#include "log.h"
#include "web_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define LOGIN1_NAME              "org.freedesktop.login1"
#define LOGIN1_MANAGER_PATH      "/org/freedesktop/login1"
#define LOGIN1_MANAGER_IFACE     "org.freedesktop.login1.Manager"
#define LOGIN1_SEAT_IFACE        "org.freedesktop.login1.Seat"
#define LOGIN1_SESSION_IFACE     "org.freedesktop.login1.Session"
#define DBUS_PROPERTIES_IFACE    "org.freedesktop.DBus.Properties"
#define SYSTEM_CONFIG_FILE       "/etc/vnc-monitor/config.ini"
#define WEB_CONFIG_FILE          "/etc/vnc-monitor/web.ini"
#define BROKER_DEFAULT_PORT      5901
#define BROKER_AGENT_DIR         "vnc-monitor"
#define BROKER_AGENT_SOCKET      "agent.sock"
#define WEB_ATTACH_TIMEOUT_MS      15000
#define WEB_SESSION_TOKEN_BYTES        32
#define WEB_SESSION_TOKEN_HEX_LEN      (WEB_SESSION_TOKEN_BYTES * 2)
#define MANAGEMENT_SESSION_TIMEOUT_MS  (10 * 60 * 1000)

typedef struct {
    int valid;
    uid_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char object_path[256];
    char user_name[128];
} ActiveSession;

typedef struct ManagementAuth ManagementAuth;

typedef enum {
    BROKER_SESSION_IDLE = 0,
    BROKER_SESSION_AUTH_VNC,
    BROKER_SESSION_AUTH_WEB,
    BROKER_SESSION_ACTIVE_VNC,
    BROKER_SESSION_ACTIVE_WEBRTC,
    BROKER_SESSION_REVOKING
} BrokerSessionState;

typedef struct {
    GMainLoop *loop;
    GDBusConnection *bus;
    char seat_path[256];

    int listener_fd;
    guint listener_source;
    guint seat_subscription;
    guint session_removed_subscription;
    guint periodic_source;
    guint sigterm_source;
    guint sigint_source;
    guint restart_source;
    gboolean restart_requested;

    WebServer *web_server;

    BrokerSessionState state;
    int client_fd;
    int control_fd;
    guint control_source;
    guint web_attach_timeout_source;
    WebServerAuthComplete web_auth_complete;
    gpointer web_auth_complete_data;
    gboolean web_token_valid;
    uid_t web_token_uid;
    char web_token_session_id[VNC_BROKER_SESSION_ID_MAX];
    gboolean web_auth_reuse;
    gboolean websocket_attached;
    char web_token[WEB_SESSION_TOKEN_HEX_LEN + 1];

    GByteArray *web_frame_buffer;
    guint32 web_frame_expected;
    guint32 web_frame_width;
    guint32 web_frame_height;
    gboolean web_frame_active;
    gboolean web_frame_in_flight;
    gboolean web_protocol_ready;
    guint64 web_frames_forwarded;
    guint64 web_frames_acked;
    guint64 web_frames_nacked;
    guint64 web_in_flight_seq;
    guint32 web_in_flight_bytes;
    char web_in_flight_sha256[65];
    guint32 web_in_flight_adler32;
    guint web_decode_failures;
    char web_device_id[VNC_BROKER_DEVICE_ID_HEX_LEN + 1];

    ManagementAuth *management_auth;
    gboolean management_token_valid;
    char management_token[WEB_SESSION_TOKEN_HEX_LEN + 1];
    gint64 management_token_expires_us;
    uid_t management_uid;
    char management_session_id[VNC_BROKER_SESSION_ID_MAX];

    int vnc_port;
    uid_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
} Broker;

struct ManagementAuth {
    Broker *broker;
    int control_fd;
    guint source;
    uid_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
    WebServerAuthComplete completion;
    gpointer completion_data;
};

static const char *
broker_session_state_name(BrokerSessionState state)
{
    switch (state) {
        case BROKER_SESSION_IDLE:
            return "idle";
        case BROKER_SESSION_AUTH_VNC:
            return "auth-vnc";
        case BROKER_SESSION_AUTH_WEB:
            return "auth-web";
        case BROKER_SESSION_ACTIVE_VNC:
            return "active-vnc";
        case BROKER_SESSION_ACTIVE_WEBRTC:
            return "active-webrtc";
        case BROKER_SESSION_REVOKING:
            return "revoking";
        default:
            return "unknown";
    }
}

static int
broker_session_owns_slot(const Broker *broker)
{
    return broker && broker->state != BROKER_SESSION_IDLE;
}

static int
broker_session_has_binding(const Broker *broker)
{
    return broker_session_owns_slot(broker) &&
           broker->uid != (uid_t)-1 &&
           broker->session_id[0] != '\0';
}

static void
broker_session_set_state(Broker *broker, BrokerSessionState state)
{
    if (!broker || broker->state == state)
        return;

    LOG_DEBUG("Broker session state: %s -> %s",
              broker_session_state_name(broker->state),
              broker_session_state_name(state));
    broker->state = state;
}

static gboolean
broker_web_slot_busy(gpointer user_data)
{
    return broker_session_owns_slot((const Broker *)user_data) ? TRUE : FALSE;
}

static const char *
broker_web_slot_state(gpointer user_data)
{
    const Broker *broker = user_data;
    return broker ? broker_session_state_name(broker->state) : "unknown";
}

static void
secure_clear(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;
#if defined(__GLIBC__)
    explicit_bzero(buf, len);
#else
    volatile unsigned char *p = buf;
    while (len--)
        *p++ = 0;
#endif
}

static void
broker_invalidate_web_token(Broker *broker)
{
    if (!broker)
        return;

    secure_clear(broker->web_token, sizeof(broker->web_token));
    broker->web_token_valid = FALSE;
    broker->web_token_uid = (uid_t)-1;
    broker->web_token_session_id[0] = '\0';
}

static void
broker_invalidate_management_token(Broker *broker)
{
    if (!broker)
        return;

    secure_clear(broker->management_token, sizeof(broker->management_token));
    broker->management_token_valid = FALSE;
    broker->management_token_expires_us = 0;
    broker->management_uid = (uid_t)-1;
    broker->management_session_id[0] = '\0';
}

static int
generate_random_hex_token(char out[WEB_SESSION_TOKEN_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random_bytes[WEB_SESSION_TOKEN_BYTES];
    size_t offset = 0;

    while (offset < sizeof(random_bytes)) {
        ssize_t n = getrandom(random_bytes + offset,
                              sizeof(random_bytes) - offset,
                              0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            secure_clear(random_bytes, sizeof(random_bytes));
            return -1;
        }
        if (n == 0) {
            secure_clear(random_bytes, sizeof(random_bytes));
            errno = EIO;
            return -1;
        }
        offset += (size_t)n;
    }

    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        out[i * 2] = hex[random_bytes[i] >> 4];
        out[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    out[WEB_SESSION_TOKEN_HEX_LEN] = '\0';
    secure_clear(random_bytes, sizeof(random_bytes));
    return 0;
}

static int
set_web_token_binding(Broker *broker,
                      uid_t uid,
                      const char *session_id)
{
    if (!broker || uid == (uid_t)-1 || !session_id || !*session_id) {
        errno = EINVAL;
        return -1;
    }

    if (broker->web_token_valid &&
        broker->web_token_uid == uid &&
        strcmp(broker->web_token_session_id, session_id) == 0)
        return 0;

    broker_invalidate_web_token(broker);
    if (generate_random_hex_token(broker->web_token) < 0)
        return -1;

    broker->web_token_valid = TRUE;
    broker->web_token_uid = uid;
    g_strlcpy(broker->web_token_session_id,
              session_id,
              sizeof(broker->web_token_session_id));
    return 0;
}

static gboolean
constant_time_token_equal(const char *expected, const char *candidate)
{
    if (!expected || !candidate ||
        strlen(candidate) != WEB_SESSION_TOKEN_HEX_LEN)
        return FALSE;

    unsigned int diff = 0;
    for (size_t i = 0; i < WEB_SESSION_TOKEN_HEX_LEN; i++)
        diff |= (unsigned char)expected[i] ^ (unsigned char)candidate[i];

    return diff == 0 ? TRUE : FALSE;
}

static gboolean
web_token_valid_for_active_session(Broker *broker, const char *candidate)
{
    if (!broker || !broker->web_token_valid || !candidate ||
        !constant_time_token_equal(broker->web_token, candidate))
        return FALSE;

    ActiveSession active;
    if (query_active_session(broker, &active) != 1 ||
        active.uid != broker->web_token_uid ||
        strcmp(active.session_id, broker->web_token_session_id) != 0) {
        broker_invalidate_web_token(broker);
        return FALSE;
    }

    return TRUE;
}


static void
broker_complete_web_auth(Broker *broker, WebServerAuthResult result)
{
    if (!broker || !broker->web_auth_complete)
        return;

    WebServerAuthComplete completion = broker->web_auth_complete;
    gpointer completion_data = broker->web_auth_complete_data;
    const char *token =
        result == WEB_SERVER_AUTH_OK && broker->web_token_valid ?
        broker->web_token : NULL;

    broker->web_auth_complete = NULL;
    broker->web_auth_complete_data = NULL;
    completion(result, token, completion_data);
}

static int
load_public_port(void)
{
    int port = BROKER_DEFAULT_PORT;

    if (access(SYSTEM_CONFIG_FILE, R_OK) != 0)
        return port;

    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile,
                                   SYSTEM_CONFIG_FILE,
                                   G_KEY_FILE_NONE,
                                   &error)) {
        LOG_ERROR("Broker cannot read %s: %s",
                  SYSTEM_CONFIG_FILE,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        g_key_file_unref(keyfile);
        return -1;
    }

    error = NULL;
    gint configured = g_key_file_get_integer(keyfile,
                                              "network",
                                              "port",
                                              &error);

    if (error) {
        if (error->domain == G_KEY_FILE_ERROR &&
            (error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND ||
             error->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND)) {
            g_clear_error(&error);
            g_key_file_unref(keyfile);
            return port;
        }

        LOG_ERROR("Broker invalid [network] port in %s: %s",
                  SYSTEM_CONFIG_FILE,
                  error->message);
        g_clear_error(&error);
        g_key_file_unref(keyfile);
        return -1;
    }

    g_key_file_unref(keyfile);

    if (configured < 1 || configured > 65535) {
        LOG_ERROR("Broker invalid [network] port=%d in %s",
                  configured,
                  SYSTEM_CONFIG_FILE);
        return -1;
    }

    return configured;
}

static int
create_public_listener(int port)
{
    int fd = socket(AF_INET,
                    SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    0);
    if (fd < 0)
        return -1;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

static void
reset_client(int client_fd)
{
    if (client_fd < 0)
        return;

    struct linger reset = {
        .l_onoff = 1,
        .l_linger = 0
    };
    (void)setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    close(client_fd);
}

static GVariant *
get_all_properties(GDBusConnection *bus,
                   const char *object_path,
                   const char *interface_name)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bus,
        LOGIN1_NAME,
        object_path,
        DBUS_PROPERTIES_IFACE,
        "GetAll",
        g_variant_new("(s)", interface_name),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    if (!reply) {
        LOG_ERROR("Broker logind GetAll(%s) failed for %s: %s",
                  interface_name,
                  object_path,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }

    GVariant *properties = NULL;
    g_variant_get(reply, "(@a{sv})", &properties);
    g_variant_unref(reply);
    return properties;
}

static int
dict_get_string(GVariant *dict,
                const char *key,
                char *out,
                size_t out_size)
{
    GVariant *value = g_variant_lookup_value(dict,
                                             key,
                                             G_VARIANT_TYPE_STRING);
    if (!value)
        return -1;

    const char *text = g_variant_get_string(value, NULL);
    g_strlcpy(out, text, out_size);
    g_variant_unref(value);
    return 0;
}

static int
dict_get_boolean(GVariant *dict, const char *key, gboolean *out)
{
    GVariant *value = g_variant_lookup_value(dict,
                                             key,
                                             G_VARIANT_TYPE_BOOLEAN);
    if (!value)
        return -1;

    *out = g_variant_get_boolean(value);
    g_variant_unref(value);
    return 0;
}

static int
dict_get_uid(GVariant *dict, uid_t *uid)
{
    GVariant *value = g_variant_lookup_value(dict,
                                             "User",
                                             G_VARIANT_TYPE("(uo)"));
    if (!value)
        return -1;

    guint32 value_uid = 0;
    const char *user_path = NULL;
    g_variant_get(value, "(u&o)", &value_uid, &user_path);
    (void)user_path;
    g_variant_unref(value);

    *uid = (uid_t)value_uid;
    return 0;
}

static int
resolve_seat_path(Broker *broker)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        broker->bus,
        LOGIN1_NAME,
        LOGIN1_MANAGER_PATH,
        LOGIN1_MANAGER_IFACE,
        "GetSeat",
        g_variant_new("(s)", "seat0"),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    if (!reply) {
        LOG_ERROR("Broker cannot resolve logind seat0: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    const char *path = NULL;
    g_variant_get(reply, "(&o)", &path);
    g_strlcpy(broker->seat_path, path, sizeof(broker->seat_path));
    g_variant_unref(reply);
    return 0;
}

/*
 * Return 1 for an eligible active local Wayland user session, 0 when seat0
 * currently points at GDM/greeter/no-user, and -1 for a logind query error.
 */
static int
query_active_session(Broker *broker, ActiveSession *out)
{
    memset(out, 0, sizeof(*out));

    GVariant *seat = get_all_properties(broker->bus,
                                        broker->seat_path,
                                        LOGIN1_SEAT_IFACE);
    if (!seat)
        return -1;

    GVariant *active = g_variant_lookup_value(seat,
                                              "ActiveSession",
                                              G_VARIANT_TYPE("(so)"));
    g_variant_unref(seat);

    if (!active)
        return 0;

    const char *session_id = NULL;
    const char *session_path = NULL;
    g_variant_get(active, "(&s&o)", &session_id, &session_path);

    if (!session_id || !*session_id ||
        !session_path || strcmp(session_path, "/") == 0) {
        g_variant_unref(active);
        return 0;
    }

    g_strlcpy(out->session_id, session_id, sizeof(out->session_id));
    g_strlcpy(out->object_path, session_path, sizeof(out->object_path));
    g_variant_unref(active);

    GVariant *session = get_all_properties(broker->bus,
                                           out->object_path,
                                           LOGIN1_SESSION_IFACE);
    if (!session)
        return -1;

    char class_name[64] = "";
    char type_name[64] = "";
    gboolean remote = TRUE;
    gboolean is_active = FALSE;
    uid_t uid = (uid_t)-1;

    int ok =
        dict_get_string(session, "Class", class_name, sizeof(class_name)) == 0 &&
        dict_get_string(session, "Type", type_name, sizeof(type_name)) == 0 &&
        dict_get_boolean(session, "Remote", &remote) == 0 &&
        dict_get_boolean(session, "Active", &is_active) == 0 &&
        dict_get_uid(session, &uid) == 0;

    (void)dict_get_string(session,
                          "Name",
                          out->user_name,
                          sizeof(out->user_name));

    g_variant_unref(session);

    if (!ok)
        return -1;

    if (!is_active || remote ||
        strcmp(class_name, "user") != 0 ||
        strcmp(type_name, "wayland") != 0) {
        LOG_INFO("Broker seat0 target not attachable: session=%s class=%s type=%s remote=%s",
                 out->session_id,
                 class_name[0] ? class_name : "unknown",
                 type_name[0] ? type_name : "unknown",
                 remote ? "yes" : "no");
        return 0;
    }

    out->uid = uid;
    out->valid = 1;
    return 1;
}

static void
broker_reset_web_frame(Broker *broker)
{
    if (!broker)
        return;

    if (broker->web_frame_buffer) {
        g_byte_array_unref(broker->web_frame_buffer);
        broker->web_frame_buffer = NULL;
    }

    broker->web_frame_expected = 0;
    broker->web_frame_width = 0;
    broker->web_frame_height = 0;
    broker->web_frame_active = FALSE;
}

static guint32
broker_adler32(const guint8 *data, gsize length)
{
    guint32 a = 1, b = 0;
    if (!data) return 0;
    for (gsize i = 0; i < length; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void
broker_web_send_frame_telemetry(Broker *broker,
                                const char *event,
                                guint64 seq,
                                guint32 bytes,
                                const char *sha256,
                                guint32 adler32,
                                guint failures)
{
    if (!broker || !event || !broker->web_server ||
        !broker->websocket_attached || !broker->web_protocol_ready) return;
    char *message = g_strdup_printf(
        "{\"type\":\"telemetry\",\"event\":\"%s\",\"seq\":%" G_GUINT64_FORMAT
        ",\"bytes\":%u,\"sha256\":\"%s\",\"adler32\":%u,\"failures\":%u}",
        event, seq, bytes, sha256 && sha256[0] ? sha256 : "unavailable", adler32, failures);
    if (!message) return;
    (void)web_server_send_text(broker->web_server, message);
    g_free(message);
}

static gboolean
broker_handle_web_media_packet(Broker *broker,
                               VncBrokerControlType type,
                               const guint8 *payload,
                               size_t payload_len)
{
    if (!broker)
        return FALSE;

    if (type == VNC_BROKER_CONTROL_DISPLAY_SIZE_REJECTED) {
        VncBrokerDisplayState state;
        if (!broker->websocket_attached ||
            !broker->web_protocol_ready ||
            vnc_broker_parse_display_state(payload,
                                           payload_len,
                                           &state) < 0)
            return FALSE;

        char *message = g_strdup_printf(
            "{\"type\":\"display-state-rejected\","
            "\"generation\":%u,\"reason\":\"runtime\","
            "\"width\":%u,\"height\":%u,"
            "\"mode\":\"%s\",\"orientation\":\"%s\"}",
            state.generation,
            state.width,
            state.height,
            state.mode == VNC_BROKER_DISPLAY_FULLSCREEN ?
                "fullscreen" : "window",
            state.orientation == VNC_BROKER_ORIENTATION_LANDSCAPE ?
                "landscape" : "portrait");
        gboolean sent = web_server_send_text(broker->web_server, message);
        g_free(message);
        return sent;
    }

    if (type == VNC_BROKER_CONTROL_DISPLAY_SIZE_APPLIED) {
        VncBrokerDisplayState state;
        if (!broker->websocket_attached ||
            !broker->web_protocol_ready ||
            vnc_broker_parse_display_state(payload,
                                           payload_len,
                                           &state) < 0)
            return FALSE;

        char *message = g_strdup_printf(
            "{\"type\":\"display-state-applied\","
            "\"generation\":%u,\"width\":%u,\"height\":%u,"
            "\"mode\":\"%s\",\"orientation\":\"%s\"}",
            state.generation,
            state.width,
            state.height,
            state.mode == VNC_BROKER_DISPLAY_FULLSCREEN ?
                "fullscreen" : "window",
            state.orientation == VNC_BROKER_ORIENTATION_LANDSCAPE ?
                "landscape" : "portrait");
        gboolean sent = web_server_send_text(broker->web_server, message);
        g_free(message);
        return sent;
    }

    if (type == VNC_BROKER_CONTROL_HLS_READY) {
        if (payload_len != 0 ||
            !broker->websocket_attached ||
            !broker->web_server)
            return FALSE;

        LOG_INFO("Broker received legacy HLS ready signal for session %s",
                 broker->session_id);
        return web_server_send_text(
            broker->web_server,
            "{\"type\":\"media-ready\",\"media\":\"hls\","
            "\"url\":\"/live/index.m3u8\"}");
    }

    if (type == VNC_BROKER_CONTROL_VIDEO_FRAME_BEGIN) {
        uint32_t width = 0, height = 0, jpeg_size = 0;
        if (broker->web_frame_active ||
            broker->web_frame_in_flight ||
            vnc_broker_parse_video_frame_begin(payload, payload_len,
                                               &width, &height,
                                               &jpeg_size) < 0 ||
            width > VNC_BROKER_VIDEO_DIMENSION_MAX ||
            height > VNC_BROKER_VIDEO_DIMENSION_MAX)
            return FALSE;

        broker->web_frame_buffer = g_byte_array_sized_new(jpeg_size);
        broker->web_frame_expected = jpeg_size;
        broker->web_frame_width = width;
        broker->web_frame_height = height;
        broker->web_frame_active = TRUE;
        return TRUE;
    }

    if (type == VNC_BROKER_CONTROL_VIDEO_FRAME_CHUNK) {
        if (!broker->web_frame_active || !broker->web_frame_buffer ||
            payload_len == 0 ||
            payload_len > broker->web_frame_expected ||
            broker->web_frame_buffer->len >
                broker->web_frame_expected - payload_len)
            return FALSE;

        g_byte_array_append(broker->web_frame_buffer,
                            payload, (guint)payload_len);
        return TRUE;
    }

    if (type == VNC_BROKER_CONTROL_VIDEO_FRAME_END) {
        if (!broker->web_frame_active || !broker->web_frame_buffer ||
            payload_len != 0 ||
            broker->web_frame_buffer->len != broker->web_frame_expected)
            return FALSE;

        if (broker->web_frames_forwarded == 0) {
            guint actual = broker->web_frame_buffer->len;
            const guint8 *data = broker->web_frame_buffer->data;
            gboolean soi_ok = actual >= 2 &&
                              data[0] == 0xffu &&
                              data[1] == 0xd8u;
            gboolean eoi_ok = actual >= 2 &&
                              data[actual - 2] == 0xffu &&
                              data[actual - 1] == 0xd9u;
            gchar *sha256 =
                g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            data,
                                            (gsize)actual);

            LOG_INFO("Broker first JPEG integrity: declared=%u actual=%u soi=%s eoi=%s sha256=%s",
                     broker->web_frame_expected,
                     actual,
                     soi_ok ? "ok" : "bad",
                     eoi_ok ? "ok" : "bad",
                     sha256 ? sha256 : "unavailable");
            g_free(sha256);

            if (!soi_ok || !eoi_ok) {
                LOG_ERROR("Broker rejected malformed legacy JPEG before WebSocket forwarding");
                broker_reset_web_frame(broker);
                return FALSE;
            }
        }

        guint actual = broker->web_frame_buffer->len;
        gchar *frame_sha256 =
            g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                        broker->web_frame_buffer->data,
                                        (gsize)actual);
        guint32 frame_adler32 = broker_adler32(broker->web_frame_buffer->data, (gsize)actual);

        gboolean sent = broker->websocket_attached &&
                        broker->web_protocol_ready &&
                        broker->web_server &&
                        web_server_send_binary(
                            broker->web_server,
                            broker->web_frame_buffer->data,
                            broker->web_frame_buffer->len);
        if (!sent) {
            g_free(frame_sha256);
            return FALSE;
        }

        broker->web_frame_in_flight = TRUE;
        broker->web_frames_forwarded++;
        broker->web_in_flight_seq = broker->web_frames_forwarded;
        broker->web_in_flight_bytes = actual;
        g_strlcpy(broker->web_in_flight_sha256,
                  frame_sha256 ? frame_sha256 : "unavailable",
                  sizeof(broker->web_in_flight_sha256));
        broker->web_in_flight_adler32 = frame_adler32;
        if (broker->web_in_flight_seq <= 3) {
            broker_web_send_frame_telemetry(broker, "frame-forwarded",
                                            broker->web_in_flight_seq,
                                            broker->web_in_flight_bytes,
                                            frame_sha256 ? frame_sha256 : "unavailable",
                                            frame_adler32,
                                            broker->web_decode_failures);
        }
        g_free(frame_sha256);

        if (broker->web_frames_forwarded == 1) {
            LOG_INFO("Broker forwarded first legacy browser video frame: %ux%u JPEG=%u bytes seq=%" G_GUINT64_FORMAT,
                     broker->web_frame_width,
                     broker->web_frame_height,
                     broker->web_frame_expected,
                     broker->web_in_flight_seq);
        }

        broker_reset_web_frame(broker);
        return TRUE;
    }

    return FALSE;
}

static void
clear_session(Broker *broker, int reset)
{
    if (!broker)
        return;

    if (broker->websocket_attached) {
        broker->websocket_attached = FALSE;
        if (broker->web_server)
            web_server_close_websocket(broker->web_server);
    }

    secure_clear(broker->web_device_id, sizeof(broker->web_device_id));
    broker->web_auth_reuse = FALSE;
    if (broker->web_server)
        web_server_set_hls_root(broker->web_server, NULL);
    broker_reset_web_frame(broker);
    broker->web_frame_in_flight = FALSE;
    broker->web_protocol_ready = FALSE;
    broker->web_frames_forwarded = 0;
    broker->web_frames_acked = 0;
    broker->web_frames_nacked = 0;
    broker->web_in_flight_seq = 0;
    broker->web_in_flight_bytes = 0;
    broker->web_in_flight_sha256[0] = '\0';
    broker->web_in_flight_adler32 = 0;
    broker->web_decode_failures = 0;

    if (broker->web_attach_timeout_source) {
        guint source = broker->web_attach_timeout_source;
        broker->web_attach_timeout_source = 0;
        g_source_remove(source);
    }

    if (broker->control_source) {
        guint source = broker->control_source;
        broker->control_source = 0;
        g_source_remove(source);
    }

    if (broker->client_fd >= 0) {
        if (reset) {
            struct linger linger = {
                .l_onoff = 1,
                .l_linger = 0
            };
            (void)setsockopt(broker->client_fd,
                             SOL_SOCKET,
                             SO_LINGER,
                             &linger,
                             sizeof(linger));
        }
        close(broker->client_fd);
    }

    if (broker->control_fd >= 0)
        close(broker->control_fd);

    broker->client_fd = -1;
    broker->control_fd = -1;
    broker->uid = (uid_t)-1;
    broker->session_id[0] = '\0';
    broker->peer_addr[0] = '\0';
    broker_session_set_state(broker, BROKER_SESSION_IDLE);

    /* A lost control channel during AUTH_WEB must finish the paused HTTP request. */
    broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
}

static void
revoke_session(Broker *broker, const char *reason)
{
    if (!broker_session_owns_slot(broker) ||
        broker->state == BROKER_SESSION_REVOKING) {
        return;
    }

    BrokerSessionState previous = broker->state;
    broker_session_set_state(broker, BROKER_SESSION_REVOKING);
    broker_invalidate_web_token(broker);

    if (broker->websocket_attached) {
        broker->websocket_attached = FALSE;
        if (broker->web_server)
            web_server_close_websocket(broker->web_server);
    }

    LOG_INFO("Broker revoking %s session for %s: bound session %s (%s)",
             broker_session_state_name(previous),
             broker->peer_addr[0] ? broker->peer_addr : "client",
             broker->session_id[0] ? broker->session_id : "unbound",
             reason ? reason : "session changed");

    if (broker->client_fd >= 0) {
        /* VNC: broker holds a duplicate of the exact accepted TCP socket. */
        (void)shutdown(broker->client_fd, SHUT_RDWR);
    }
    else if (broker->control_fd >= 0) {
        /*
         * WebRTC has no browser fd in the agent. The bound control channel is
         * its authoritative lifetime guard. REVOKE is best-effort; shutdown()
         * is the fail-safe even if the peer cannot parse another message.
         */
        (void)vnc_broker_send_control(broker->control_fd,
                                      VNC_BROKER_CONTROL_REVOKE,
                                      NULL,
                                      0);
        (void)shutdown(broker->control_fd, SHUT_RDWR);
    }
}

static void
enforce_session_binding(Broker *broker)
{
    if (!broker_session_has_binding(broker) ||
        broker->state == BROKER_SESSION_REVOKING) {
        return;
    }

    ActiveSession active;
    int rc = query_active_session(broker, &active);

    if (rc != 1 ||
        strcmp(active.session_id, broker->session_id) != 0 ||
        active.uid != broker->uid) {
        revoke_session(broker,
                       rc == 1 ? "seat0 switched session" : "no active user session");
    }
}

static gboolean
vnc_control_ready_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker_session_owns_slot(broker) || fd != broker->control_fd)
        return G_SOURCE_REMOVE;

    /* This callback is terminal for the current v1 status protocol. */
    broker->control_source = 0;

    uint8_t status = 0;
    int got_status = 0;

    if ((condition & G_IO_IN) != 0 &&
        vnc_broker_recv_status(fd, &status) == 0) {
        got_status = 1;
    }

    if (got_status && status == VNC_BROKER_STATUS_DONE) {
        LOG_INFO("Broker session finished: transport=%s peer=%s session=%s",
                 broker_session_state_name(broker->state),
                 broker->peer_addr,
                 broker->session_id);
        clear_session(broker, 0);
    }
    else if (got_status && status == VNC_BROKER_STATUS_BUSY) {
        LOG_INFO("Broker handoff rejected: active user agent is busy");
        clear_session(broker, 1);
    }
    else if (got_status && status == VNC_BROKER_STATUS_REJECT) {
        LOG_INFO("Broker handoff rejected by active user agent");
        clear_session(broker, 1);
    }
    else {
        LOG_INFO("Broker lost agent control channel for session %s",
                 broker->session_id);
        if (broker->client_fd >= 0)
            (void)shutdown(broker->client_fd, SHUT_RDWR);
        clear_session(broker, 1);
    }

    return G_SOURCE_REMOVE;
}

static int connect_agent(const ActiveSession *session);

static gboolean
web_attach_timeout_cb(gpointer user_data)
{
    Broker *broker = user_data;
    broker->web_attach_timeout_source = 0;

    if (broker->state == BROKER_SESSION_ACTIVE_WEBRTC &&
        !broker->websocket_attached) {
        LOG_INFO("Broker WebRTC attach window expired for %s session=%s",
                 broker->peer_addr,
                 broker->session_id);
        if (broker->control_fd >= 0) {
            (void)vnc_broker_send_control(broker->control_fd,
                                          VNC_BROKER_CONTROL_REVOKE,
                                          NULL,
                                          0);
            (void)shutdown(broker->control_fd, SHUT_RDWR);
        }
        clear_session(broker, 0);
    }

    return G_SOURCE_REMOVE;
}

static gboolean
web_control_ready_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker_session_owns_slot(broker) || fd != broker->control_fd)
        return G_SOURCE_REMOVE;

    if (broker->state == BROKER_SESSION_AUTH_WEB &&
        (condition & G_IO_IN) != 0) {
        VncBrokerWebAuthResult auth_result = VNC_BROKER_WEB_AUTH_ERROR;

        if (vnc_broker_recv_web_auth_result(fd, &auth_result) < 0) {
            LOG_INFO("Broker could not read browser authentication result for session %s",
                     broker->session_id);
            broker->control_source = 0;
            broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
            clear_session(broker, 1);
            return G_SOURCE_REMOVE;
        }

        if (auth_result == VNC_BROKER_WEB_AUTH_OK) {
            if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
                LOG_INFO("Broker lost browser agent channel after successful authentication for session %s",
                         broker->session_id);
                broker->control_source = 0;
                broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
                clear_session(broker, 1);
                return G_SOURCE_REMOVE;
            }

            if (vnc_broker_send_device_bind(fd,
                                            broker->web_device_id) < 0) {
                LOG_INFO("Broker could not bind browser device identity for session %s: %s",
                         broker->session_id,
                         strerror(errno));
                broker->control_source = 0;
                broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
                clear_session(broker, 1);
                return G_SOURCE_REMOVE;
            }

            if (set_web_token_binding(broker,
                                      broker->uid,
                                      broker->session_id) < 0) {
                LOG_ERROR("Broker could not establish browser authentication token: %s",
                          strerror(errno));
                broker->control_source = 0;
                broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
                clear_session(broker, 1);
                return G_SOURCE_REMOVE;
            }

            broker_session_set_state(broker, BROKER_SESSION_ACTIVE_WEBRTC);
            broker->web_attach_timeout_source =
                g_timeout_add(WEB_ATTACH_TIMEOUT_MS,
                              web_attach_timeout_cb, broker);

            LOG_INFO("Broker authenticated browser peer=%s uid=%lu session=%s; attach window=%dms",
                     broker->peer_addr, (unsigned long)broker->uid,
                     broker->session_id, WEB_ATTACH_TIMEOUT_MS);
            broker_complete_web_auth(broker, WEB_SERVER_AUTH_OK);
            return G_SOURCE_CONTINUE;
        }

        broker->control_source = 0;
        if (auth_result == VNC_BROKER_WEB_AUTH_DENIED)
            broker_complete_web_auth(broker, WEB_SERVER_AUTH_DENIED);
        else
            broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
        clear_session(broker, 1);
        return G_SOURCE_REMOVE;
    }

    if (broker->state == BROKER_SESSION_ACTIVE_WEBRTC &&
        (condition & G_IO_IN) != 0) {
        guint8 payload[VNC_BROKER_CONTROL_PAYLOAD_MAX];
        VncBrokerControlType type;
        size_t payload_len = 0;

        int recv_rc =
            vnc_broker_recv_control(fd, &type, payload,
                                    sizeof(payload), &payload_len);

        if (recv_rc < 0) {
            int saved_errno = errno;
            if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0 ||
                saved_errno == ECONNRESET) {
                LOG_INFO("Broker lost browser agent media channel for session %s",
                         broker->session_id);
            }
            else {
                LOG_INFO("Broker could not read browser media control packet for session %s: %s",
                         broker->session_id,
                         strerror(saved_errno));
            }

            broker->control_source = 0;
            clear_session(broker, 1);
            return G_SOURCE_REMOVE;
        }

        if (broker_handle_web_media_packet(broker, type,
                                           payload, payload_len))
            return G_SOURCE_CONTINUE;

        LOG_INFO("Broker rejected malformed browser media control packet type=%u payload=%zu for session %s",
                 (unsigned)type,
                 payload_len,
                 broker->session_id);
        broker->control_source = 0;
        clear_session(broker, 1);
        return G_SOURCE_REMOVE;
    }

    LOG_INFO("Broker lost browser agent control channel for session %s",
             broker->session_id);
    broker->control_source = 0;
    if (broker->state == BROKER_SESSION_AUTH_WEB)
        broker_complete_web_auth(broker, WEB_SERVER_AUTH_ERROR);
    clear_session(broker, 1);
    return G_SOURCE_REMOVE;
}

static gboolean
broker_web_validate_websocket_token(const char *token, gpointer user_data)
{
    Broker *broker = user_data;

    return broker &&
           broker->state == BROKER_SESSION_ACTIVE_WEBRTC &&
           !broker->websocket_attached &&
           web_token_valid_for_active_session(broker, token);
}

static gboolean
broker_web_validate_media_token(const char *token, gpointer user_data)
{
    Broker *broker = user_data;
    return broker &&
           broker->state == BROKER_SESSION_ACTIVE_WEBRTC &&
           broker->websocket_attached &&
           web_token_valid_for_active_session(broker, token);
}

static gboolean
broker_web_bind_websocket(const char *token, gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker_web_validate_websocket_token(token, user_data))
        return FALSE;

    char *hls_root =
        g_strdup_printf("/run/user/%lu/vnc-monitor/hls",
                        (unsigned long)broker->uid);
    if (broker->web_server)
        web_server_set_hls_root(broker->web_server, hls_root);
    g_free(hls_root);

    if (broker->control_fd < 0) {
        if (broker->web_server)
            web_server_set_hls_root(broker->web_server, NULL);
        return FALSE;
    }

    broker->websocket_attached = TRUE;
    /*
     * Retain the token only for the live viewer lifetime. WSS replay is still
     * rejected by websocket_attached; same-origin HLS GETs use this token.
     */
    broker_reset_web_frame(broker);
    broker->web_frame_in_flight = FALSE;
    broker->web_protocol_ready = FALSE;
    broker->web_frames_forwarded = 0;
    broker->web_frames_acked = 0;
    broker->web_frames_nacked = 0;
    broker->web_in_flight_seq = 0;
    broker->web_in_flight_bytes = 0;
    broker->web_in_flight_sha256[0] = '\0';
    broker->web_in_flight_adler32 = 0;
    broker->web_decode_failures = 0;

    if (broker->web_attach_timeout_source) {
        guint source = broker->web_attach_timeout_source;
        broker->web_attach_timeout_source = 0;
        g_source_remove(source);
    }

    LOG_INFO("Broker bound authenticated WebSocket to peer=%s uid=%lu session=%s",
             broker->peer_addr,
             (unsigned long)broker->uid,
             broker->session_id);
    return TRUE;
}

static gboolean
broker_websocket_protocol_ready(guint protocol, gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker ||
        protocol != VNC_WEB_PROTOCOL_VERSION ||
        broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached ||
        broker->web_protocol_ready ||
        broker->control_fd < 0)
        return FALSE;

    if (vnc_broker_send_control(broker->control_fd,
                                VNC_BROKER_CONTROL_MEDIA_START,
                                NULL, 0) < 0) {
        LOG_INFO("Broker could not start browser media after protocol handshake for session %s: %s",
                 broker->session_id,
                 strerror(errno));
        return FALSE;
    }

    broker->web_protocol_ready = TRUE;
    LOG_INFO("Broker browser protocol verified: version=%u session=%s; media start released",
             protocol,
             broker->session_id);
    return TRUE;
}

static gboolean
broker_websocket_frame_ack(gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached || !broker->web_protocol_ready || broker->control_fd < 0) return FALSE;
    if (!broker->web_frame_in_flight) return TRUE;

    guint64 seq = broker->web_in_flight_seq;
    guint32 bytes = broker->web_in_flight_bytes;
    guint32 adler32 = broker->web_in_flight_adler32;
    char sha256[65];
    g_strlcpy(sha256, broker->web_in_flight_sha256[0] ? broker->web_in_flight_sha256 : "unavailable", sizeof(sha256));

    broker->web_frame_in_flight = FALSE;
    broker->web_frames_acked++;
    broker->web_decode_failures = 0;
    if (seq <= 3) broker_web_send_frame_telemetry(broker, "frame-ack", seq, bytes, sha256, adler32, 0);

    broker->web_in_flight_seq = 0;
    broker->web_in_flight_bytes = 0;
    broker->web_in_flight_sha256[0] = '\0';
    broker->web_in_flight_adler32 = 0;

    if (vnc_broker_send_control(broker->control_fd, VNC_BROKER_CONTROL_VIDEO_FRAME_ACK, NULL, 0) < 0) {
        LOG_INFO("Broker could not forward browser frame ACK for session %s: %s", broker->session_id, strerror(errno));
        (void)shutdown(broker->control_fd, SHUT_RDWR);
        return FALSE;
    }
    if (broker->web_frames_acked == 1)
        LOG_INFO("Broker received first browser JPEG frame ACK; queue-depth=1 decode pacing active");
    return TRUE;
}

static gboolean
broker_websocket_frame_nack(gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached || !broker->web_protocol_ready || broker->control_fd < 0) return FALSE;
    if (!broker->web_frame_in_flight) return TRUE;

    broker->web_decode_failures++;
    broker->web_frames_nacked++;
    LOG_INFO("Broker browser JPEG decode failure: seq=%" G_GUINT64_FORMAT
             " bytes=%u sha256=%s adler32=%u consecutive=%u/%u",
             broker->web_in_flight_seq, broker->web_in_flight_bytes,
             broker->web_in_flight_sha256[0] ? broker->web_in_flight_sha256 : "unavailable",
             broker->web_in_flight_adler32, broker->web_decode_failures,
             VNC_WEB_JPEG_DECODE_FAILURE_LIMIT);

    broker_web_send_frame_telemetry(broker, "frame-nack",
                                    broker->web_in_flight_seq,
                                    broker->web_in_flight_bytes,
                                    broker->web_in_flight_sha256,
                                    broker->web_in_flight_adler32,
                                    broker->web_decode_failures);

    broker->web_frame_in_flight = FALSE;
    broker->web_in_flight_seq = 0;
    broker->web_in_flight_bytes = 0;
    broker->web_in_flight_sha256[0] = '\0';
    broker->web_in_flight_adler32 = 0;
    if (broker->web_decode_failures >= VNC_WEB_JPEG_DECODE_FAILURE_LIMIT) return FALSE;

    if (vnc_broker_send_control(broker->control_fd, VNC_BROKER_CONTROL_VIDEO_FRAME_ACK, NULL, 0) < 0) {
        LOG_INFO("Broker could not release browser frame after JPEG NACK for session %s: %s", broker->session_id, strerror(errno));
        (void)shutdown(broker->control_fd, SHUT_RDWR);
        return FALSE;
    }
    return TRUE;
}

static gboolean
broker_websocket_display_state(const WebServerDisplayState *state,
                               gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || !state ||
        broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached ||
        !broker->web_protocol_ready ||
        broker->control_fd < 0 ||
        broker->web_frame_in_flight)
        return FALSE;

    VncBrokerDisplayState wire = {
        .generation = state->generation,
        .width = state->width,
        .height = state->height,
        .mode = state->mode,
        .orientation = state->orientation
    };

    if (vnc_broker_send_display_state(broker->control_fd,
                                      VNC_BROKER_CONTROL_DISPLAY_SIZE,
                                      &wire) < 0) {
        LOG_INFO("Broker could not forward browser display state generation=%u: %s",
                 state->generation,
                 strerror(errno));
        return FALSE;
    }

    LOG_INFO("Broker forwarded browser display state: device=%.8s generation=%u size=%ux%u mode=%s orientation=%s",
             broker->web_device_id,
             state->generation,
             state->width,
             state->height,
             state->mode == VNC_BROKER_DISPLAY_FULLSCREEN ? "fullscreen" : "window",
             state->orientation == VNC_BROKER_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
    return TRUE;
}

static void
broker_websocket_client_diagnostic(const WebServerClientDiagnostic *diagnostic, gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || !diagnostic || broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached || !broker->web_protocol_ready) return;

    LOG_INFO("Broker browser client diagnostic: kind=%s level=%s event=%s peer=%s session=%s seq=%"
             G_GUINT64_FORMAT " bytes=%u observed=%u checksum=%u elapsed_ms=%u mime=%s",
             diagnostic->kind, diagnostic->level, diagnostic->event,
             broker->peer_addr[0] ? broker->peer_addr : "unknown",
             broker->session_id[0] ? broker->session_id : "unknown",
             diagnostic->seq, diagnostic->bytes, diagnostic->observed,
             diagnostic->checksum, diagnostic->elapsed_ms, diagnostic->mime);
}

static void
broker_websocket_closed(gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker ||
        broker->state != BROKER_SESSION_ACTIVE_WEBRTC ||
        !broker->websocket_attached) {
        return;
    }

    broker->websocket_attached = FALSE;
    broker_session_set_state(broker, BROKER_SESSION_REVOKING);

    LOG_INFO("Broker authenticated WebSocket closed for peer=%s session=%s",
             broker->peer_addr,
             broker->session_id);

    if (broker->control_fd >= 0) {
        (void)vnc_broker_send_control(broker->control_fd,
                                      VNC_BROKER_CONTROL_REVOKE,
                                      NULL,
                                      0);
        (void)shutdown(broker->control_fd, SHUT_RDWR);
    }

    clear_session(broker, 0);
}

static void
management_auth_finish(ManagementAuth *auth,
                       WebServerAuthResult result,
                       const char *token)
{
    if (!auth)
        return;

    Broker *broker = auth->broker;
    if (auth->source) {
        guint source = auth->source;
        auth->source = 0;
        g_source_remove(source);
    }

    if (auth->control_fd >= 0) {
        close(auth->control_fd);
        auth->control_fd = -1;
    }

    if (broker && broker->management_auth == auth)
        broker->management_auth = NULL;

    WebServerAuthComplete completion = auth->completion;
    gpointer completion_data = auth->completion_data;
    g_free(auth);

    if (completion)
        completion(result, token, completion_data);
}

static gboolean
management_control_ready_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    ManagementAuth *auth = user_data;
    Broker *broker = auth ? auth->broker : NULL;

    if (!auth || !broker || fd != auth->control_fd)
        return G_SOURCE_REMOVE;

    auth->source = 0;

    VncBrokerWebAuthResult auth_result = VNC_BROKER_WEB_AUTH_ERROR;
    int got_result =
        (condition & G_IO_IN) != 0 &&
        vnc_broker_recv_web_auth_result(fd, &auth_result) == 0;

    if (!got_result) {
        LOG_INFO("Broker lost management authentication channel for session %s",
                 auth->session_id);
        management_auth_finish(auth, WEB_SERVER_AUTH_ERROR, NULL);
        return G_SOURCE_REMOVE;
    }

    if (auth_result == VNC_BROKER_WEB_AUTH_DENIED) {
        management_auth_finish(auth, WEB_SERVER_AUTH_DENIED, NULL);
        return G_SOURCE_REMOVE;
    }

    if (auth_result != VNC_BROKER_WEB_AUTH_OK) {
        management_auth_finish(auth, WEB_SERVER_AUTH_ERROR, NULL);
        return G_SOURCE_REMOVE;
    }

    ActiveSession active;
    if (query_active_session(broker, &active) != 1 ||
        active.uid != auth->uid ||
        strcmp(active.session_id, auth->session_id) != 0) {
        LOG_INFO("Broker discarded management authentication because seat0 changed");
        management_auth_finish(auth, WEB_SERVER_AUTH_UNAVAILABLE, NULL);
        return G_SOURCE_REMOVE;
    }

    broker_invalidate_management_token(broker);
    if (set_web_token_binding(broker,
                              auth->uid,
                              auth->session_id) < 0) {
        LOG_ERROR("Broker could not establish shared browser authentication token: %s",
                  strerror(errno));
        management_auth_finish(auth, WEB_SERVER_AUTH_ERROR, NULL);
        return G_SOURCE_REMOVE;
    }

    LOG_INFO("Broker management session authenticated for peer=%s uid=%lu session=%s using shared browser token",
             auth->peer_addr,
             (unsigned long)auth->uid,
             auth->session_id);

    management_auth_finish(auth,
                           WEB_SERVER_AUTH_OK,
                           broker->web_token);
    return G_SOURCE_REMOVE;
}

static WebServerAuthResult
broker_web_begin_resume(const char *token,
                        const char *peer_addr,
                        const char *device_id,
                        WebServerAuthComplete completion,
                        gpointer completion_data,
                        gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker || !token || !device_id || !completion ||
        strlen(device_id) != VNC_BROKER_DEVICE_ID_HEX_LEN)
        return WEB_SERVER_AUTH_ERROR;

    if (broker_session_owns_slot(broker))
        return WEB_SERVER_AUTH_BUSY;

    if (!web_token_valid_for_active_session(broker, token))
        return WEB_SERVER_AUTH_DENIED;

    ActiveSession active;
    if (query_active_session(broker, &active) != 1 ||
        active.uid != broker->web_token_uid ||
        strcmp(active.session_id, broker->web_token_session_id) != 0)
        return WEB_SERVER_AUTH_DENIED;

    int control_fd = connect_agent(&active);
    if (control_fd < 0)
        return WEB_SERVER_AUTH_UNAVAILABLE;

    broker_session_set_state(broker, BROKER_SESSION_AUTH_WEB);
    broker->client_fd = -1;
    broker->control_fd = control_fd;
    broker->uid = active.uid;
    g_strlcpy(broker->session_id, active.session_id, sizeof(broker->session_id));
    g_strlcpy(broker->peer_addr,
              peer_addr && *peer_addr ? peer_addr : "unknown",
              sizeof(broker->peer_addr));
    broker->web_auth_complete = completion;
    broker->web_auth_complete_data = completion_data;
    broker->web_auth_reuse = TRUE;
    g_strlcpy(broker->web_device_id,
              device_id,
              sizeof(broker->web_device_id));

    if (vnc_broker_send_handoff_transport(control_fd,
                                          VNC_BROKER_TRANSPORT_WEBRTC,
                                          -1,
                                          active.uid,
                                          active.session_id,
                                          broker->peer_addr) < 0 ||
        vnc_broker_send_web_auth_reuse(control_fd) < 0) {
        broker->web_auth_complete = NULL;
        broker->web_auth_complete_data = NULL;
        clear_session(broker, 1);
        return WEB_SERVER_AUTH_UNAVAILABLE;
    }

    broker->control_source =
        g_unix_fd_add(control_fd,
                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                      web_control_ready_cb,
                      broker);
    if (!broker->control_source) {
        broker->web_auth_complete = NULL;
        broker->web_auth_complete_data = NULL;
        clear_session(broker, 1);
        return WEB_SERVER_AUTH_ERROR;
    }

    LOG_INFO("Broker resumed authenticated browser peer=%s uid=%lu session=%s without PAM",
             broker->peer_addr,
             (unsigned long)broker->uid,
             broker->session_id);
    enforce_session_binding(broker);
    return WEB_SERVER_AUTH_STARTED;
}

static WebServerAuthResult
broker_management_begin_auth(const char *username,
                             const char *password,
                             const char *peer_addr,
                             WebServerAuthComplete completion,
                             gpointer completion_data,
                             gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || !username || !password || !completion)
        return WEB_SERVER_AUTH_ERROR;

    if (broker->management_auth)
        return WEB_SERVER_AUTH_BUSY;

    ActiveSession active;
    if (query_active_session(broker, &active) != 1)
        return WEB_SERVER_AUTH_UNAVAILABLE;

    const char *active_user = active.user_name;
    struct passwd *pw = NULL;
    if (!active_user[0]) {
        pw = getpwuid(active.uid);
        active_user = pw && pw->pw_name ? pw->pw_name : "";
    }

    if (!active_user[0] || strcmp(username, active_user) != 0)
        return WEB_SERVER_AUTH_DENIED;

    int control_fd = connect_agent(&active);
    if (control_fd < 0)
        return WEB_SERVER_AUTH_UNAVAILABLE;

    ManagementAuth *auth = g_new0(ManagementAuth, 1);
    auth->broker = broker;
    auth->control_fd = control_fd;
    auth->uid = active.uid;
    auth->completion = completion;
    auth->completion_data = completion_data;
    g_strlcpy(auth->session_id, active.session_id, sizeof(auth->session_id));
    g_strlcpy(auth->peer_addr,
              peer_addr && *peer_addr ? peer_addr : "unknown",
              sizeof(auth->peer_addr));

    if (vnc_broker_send_handoff_transport(control_fd,
                                          VNC_BROKER_TRANSPORT_MANAGEMENT,
                                          -1,
                                          active.uid,
                                          active.session_id,
                                          auth->peer_addr) < 0 ||
        vnc_broker_send_web_auth_request(control_fd, username, password) < 0) {
        close(control_fd);
        g_free(auth);
        return WEB_SERVER_AUTH_UNAVAILABLE;
    }

    broker->management_auth = auth;
    auth->source = g_unix_fd_add(control_fd,
                                 G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                 management_control_ready_cb,
                                 auth);
    if (!auth->source) {
        broker->management_auth = NULL;
        close(control_fd);
        g_free(auth);
        return WEB_SERVER_AUTH_ERROR;
    }

    LOG_INFO("Broker started management authentication for peer=%s uid=%lu session=%s",
             auth->peer_addr,
             (unsigned long)auth->uid,
             auth->session_id);
    return WEB_SERVER_AUTH_STARTED;
}

static gboolean
broker_validate_management_token(const char *token, gpointer user_data)
{
    return web_token_valid_for_active_session((Broker *)user_data, token);
}


static void
broker_browser_logout(gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker)
        return;

    broker_invalidate_web_token(broker);
    broker_invalidate_management_token(broker);

    if (broker->state == BROKER_SESSION_AUTH_WEB ||
        broker->state == BROKER_SESSION_ACTIVE_WEBRTC ||
        broker->state == BROKER_SESSION_REVOKING)
        revoke_session(broker, "explicit browser logout");
}

static void
broker_management_logout(gpointer user_data)
{
    broker_browser_logout(user_data);
}

static int
broker_get_management_info(WebServerManagementInfo *info, gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || !info)
        return -1;

    memset(info, 0, sizeof(*info));
    info->vnc_port = broker->vnc_port;
    info->viewer_active = broker_session_owns_slot(broker) ? TRUE : FALSE;
    info->websocket_attached = broker->websocket_attached;
    info->web_protocol_ready = broker->web_protocol_ready;
    info->web_frame_in_flight = broker->web_frame_in_flight;
    info->web_frames_forwarded = broker->web_frames_forwarded;
    info->web_frames_acked = broker->web_frames_acked;
    info->web_frames_nacked = broker->web_frames_nacked;
    g_strlcpy(info->viewer_state,
              broker_session_state_name(broker->state),
              sizeof(info->viewer_state));

    if (broker->state == BROKER_SESSION_ACTIVE_VNC ||
        broker->state == BROKER_SESSION_AUTH_VNC) {
        g_strlcpy(info->viewer_transport, "vnc", sizeof(info->viewer_transport));
    }
    else if (broker->state == BROKER_SESSION_ACTIVE_WEBRTC ||
             broker->state == BROKER_SESSION_AUTH_WEB ||
             broker->state == BROKER_SESSION_REVOKING) {
        g_strlcpy(info->viewer_transport, "webrtc", sizeof(info->viewer_transport));
    }
    else {
        g_strlcpy(info->viewer_transport, "none", sizeof(info->viewer_transport));
    }

    g_strlcpy(info->viewer_peer, broker->peer_addr, sizeof(info->viewer_peer));
    g_strlcpy(info->viewer_session_id,
              broker->session_id,
              sizeof(info->viewer_session_id));

    if (broker->uid != (uid_t)-1) {
        struct passwd *pw = getpwuid(broker->uid);
        if (pw && pw->pw_name)
            g_strlcpy(info->viewer_user, pw->pw_name, sizeof(info->viewer_user));
    }

    ActiveSession active;
    if (query_active_session(broker, &active) == 1) {
        info->active_user_available = TRUE;
        info->active_uid = (guint)active.uid;
        g_strlcpy(info->active_user, active.user_name, sizeof(info->active_user));
        g_strlcpy(info->active_session_id,
                  active.session_id,
                  sizeof(info->active_session_id));
    }

    return 0;
}

static gboolean
broker_management_disconnect_viewer(gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || !broker_session_owns_slot(broker))
        return FALSE;

    revoke_session(broker, "management disconnect request");
    return TRUE;
}

static WebServerAuthResult
broker_web_begin_auth(const char *username,
                      const char *password,
                      const char *peer_addr,
                      const char *device_id,
                      WebServerAuthComplete completion,
                      gpointer completion_data,
                      gpointer user_data)
{
    Broker *broker = user_data;

    if (!broker || !username || !password || !device_id || !completion ||
        strlen(device_id) != VNC_BROKER_DEVICE_ID_HEX_LEN)
        return WEB_SERVER_AUTH_ERROR;

    if (broker_session_owns_slot(broker))
        return WEB_SERVER_AUTH_BUSY;

    ActiveSession active;
    int active_rc = query_active_session(broker, &active);
    if (active_rc != 1)
        return WEB_SERVER_AUTH_UNAVAILABLE;

    const char *active_user = active.user_name;
    struct passwd *pw = NULL;
    if (!active_user[0]) {
        pw = getpwuid(active.uid);
        active_user = pw && pw->pw_name ? pw->pw_name : "";
    }

    /*
     * Do not forward credentials for another account to PAM. Externally this
     * is the same generic authentication failure as a bad password.
     */
    if (!active_user[0] || strcmp(username, active_user) != 0)
        return WEB_SERVER_AUTH_DENIED;

    broker_session_set_state(broker, BROKER_SESSION_AUTH_WEB);

    int control_fd = connect_agent(&active);
    if (control_fd < 0) {
        LOG_INFO("Broker cannot reach WebRTC agent for uid=%lu session=%s: %s",
                 (unsigned long)active.uid,
                 active.session_id,
                 strerror(errno));
        broker_session_set_state(broker, BROKER_SESSION_IDLE);
        return WEB_SERVER_AUTH_UNAVAILABLE;
    }

    broker->client_fd = -1;
    broker->control_fd = control_fd;
    broker->uid = active.uid;
    g_strlcpy(broker->session_id, active.session_id, sizeof(broker->session_id));
    g_strlcpy(broker->peer_addr,
              peer_addr && *peer_addr ? peer_addr : "unknown",
              sizeof(broker->peer_addr));
    broker->web_auth_complete = completion;
    broker->web_auth_complete_data = completion_data;
    broker->web_auth_reuse = FALSE;
    g_strlcpy(broker->web_device_id,
              device_id,
              sizeof(broker->web_device_id));

    if (vnc_broker_send_handoff_transport(control_fd,
                                          VNC_BROKER_TRANSPORT_WEBRTC,
                                          -1,
                                          active.uid,
                                          active.session_id,
                                          broker->peer_addr) < 0 ||
        vnc_broker_send_web_auth_request(control_fd, username, password) < 0) {
        LOG_INFO("Broker could not start WebRTC authentication for %s: %s",
                 broker->peer_addr,
                 strerror(errno));
        broker->web_auth_complete = NULL;
        broker->web_auth_complete_data = NULL;
        clear_session(broker, 1);
        return WEB_SERVER_AUTH_UNAVAILABLE;
    }

    broker->control_source =
        g_unix_fd_add(control_fd,
                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                      web_control_ready_cb,
                      broker);
    if (!broker->control_source) {
        broker->web_auth_complete = NULL;
        broker->web_auth_complete_data = NULL;
        clear_session(broker, 1);
        return WEB_SERVER_AUTH_ERROR;
    }

    LOG_INFO("Broker started WebRTC authentication for peer=%s uid=%lu session=%s",
             broker->peer_addr,
             (unsigned long)broker->uid,
             broker->session_id);

    /* Close the same seat-switch race as the VNC handoff path. */
    enforce_session_binding(broker);
    return WEB_SERVER_AUTH_STARTED;
}

static int
connect_agent(const ActiveSession *session)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int n = snprintf(path,
                     sizeof(path),
                     "/run/user/%lu/%s/%s",
                     (unsigned long)session->uid,
                     BROKER_AGENT_DIR,
                     BROKER_AGENT_SOCKET);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) < 0 ||
        peer.uid != session->uid) {
        int saved = errno ? errno : EPERM;
        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

static int
route_vnc_client(Broker *broker,
                 int client_fd,
                 const char *peer_addr,
                 const ActiveSession *session)
{
    broker_session_set_state(broker, BROKER_SESSION_AUTH_VNC);

    int control_fd = connect_agent(session);
    if (control_fd < 0) {
        LOG_INFO("Broker cannot reach agent for uid=%lu session=%s: %s",
                 (unsigned long)session->uid,
                 session->session_id,
                 strerror(errno));
        broker_session_set_state(broker, BROKER_SESSION_IDLE);
        return -1;
    }

    if (vnc_broker_send_handoff(control_fd,
                                client_fd,
                                session->uid,
                                session->session_id,
                                peer_addr) < 0) {
        int saved = errno;
        close(control_fd);
        broker_session_set_state(broker, BROKER_SESSION_IDLE);
        errno = saved;
        return -1;
    }

    broker->client_fd = client_fd;
    broker->control_fd = control_fd;
    broker->uid = session->uid;
    g_strlcpy(broker->session_id,
              session->session_id,
              sizeof(broker->session_id));
    g_strlcpy(broker->peer_addr,
              peer_addr,
              sizeof(broker->peer_addr));

    broker->control_source = g_unix_fd_add(control_fd,
                                            G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                            vnc_control_ready_cb,
                                            broker);

    /*
     * ACTIVE_VNC means the broker-owned global slot has been handed to the
     * selected user agent. RA2/PAM still occurs inside that unprivileged
     * agent exactly as in beta.3; the broker does not duplicate that auth.
     */
    broker_session_set_state(broker, BROKER_SESSION_ACTIVE_VNC);

    LOG_INFO("Broker routed VNC %s to uid=%lu user=%s logind-session=%s",
             peer_addr,
             (unsigned long)session->uid,
             session->user_name[0] ? session->user_name : "unknown",
             session->session_id);

    /* Close a race where seat0 changed between the query and FD handoff. */
    enforce_session_binding(broker);
    return 0;
}

static gboolean
listener_ready_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    Broker *broker = user_data;

    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
        LOG_ERROR("Broker public listener failed");
        g_main_loop_quit(broker->loop);
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept4(fd,
                                (struct sockaddr *)&peer,
                                &peer_len,
                                SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            LOG_ERROR("Broker accept failed: %s", strerror(errno));
            break;
        }

        char peer_addr[VNC_BROKER_PEER_ADDR_MAX] = "unknown";
        (void)inet_ntop(AF_INET,
                        &peer.sin_addr,
                        peer_addr,
                        sizeof(peer_addr));

        if (broker_session_owns_slot(broker)) {
            LOG_INFO("Broker rejected additional VNC client %s: strong single-connect policy; state=%s",
                     peer_addr,
                     broker_session_state_name(broker->state));
            reset_client(client_fd);
            continue;
        }

        ActiveSession active;
        int active_rc = query_active_session(broker, &active);
        if (active_rc != 1) {
            LOG_INFO("Broker rejected %s: no active local Wayland user session on seat0",
                     peer_addr);
            reset_client(client_fd);
            continue;
        }

        if (route_vnc_client(broker, client_fd, peer_addr, &active) < 0) {
            LOG_INFO("Broker rejected %s: active session agent unavailable (%s)",
                     peer_addr,
                     strerror(errno));
            reset_client(client_fd);
        }
    }

    return G_SOURCE_CONTINUE;
}

static void
enforce_management_binding(Broker *broker)
{
    if (!broker || !broker->management_token_valid)
        return;

    ActiveSession active;
    if (query_active_session(broker, &active) != 1 ||
        active.uid != broker->management_uid ||
        strcmp(active.session_id, broker->management_session_id) != 0) {
        LOG_INFO("Broker invalidated management session because seat0 changed");
        broker_invalidate_management_token(broker);
    }
}

static void
logind_changed_cb(GDBusConnection *connection,
                  const gchar *sender_name,
                  const gchar *object_path,
                  const gchar *interface_name,
                  const gchar *signal_name,
                  GVariant *parameters,
                  gpointer user_data)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;

    Broker *broker = user_data;
    enforce_session_binding(broker);
    enforce_management_binding(broker);
}

static gboolean
periodic_binding_check(gpointer user_data)
{
    Broker *broker = user_data;
    enforce_session_binding(broker);
    enforce_management_binding(broker);
    return G_SOURCE_CONTINUE;
}

static gboolean
broker_restart_cb(gpointer user_data)
{
    Broker *broker = user_data;
    broker->restart_source = 0;
    broker->restart_requested = TRUE;

    LOG_INFO("Broker restart requested after management settings update");

    if (broker_session_owns_slot(broker))
        revoke_session(broker, "management settings restart");

    g_main_loop_quit(broker->loop);
    return G_SOURCE_REMOVE;
}

static void
broker_request_restart(gpointer user_data)
{
    Broker *broker = user_data;
    if (!broker || broker->restart_source)
        return;

    broker->restart_source = g_timeout_add(1000, broker_restart_cb, broker);
}

static gboolean
shutdown_cb(gpointer user_data)
{
    Broker *broker = user_data;

    /*
     * Returning G_SOURCE_REMOVE makes GLib destroy the signal source after
     * this callback returns. Clear the matching stored id now so normal
     * shutdown cleanup does not attempt to remove the same source twice.
     */
    GSource *current_source = g_main_current_source();
    guint current_id = current_source ? g_source_get_id(current_source) : 0;

    if (current_id == broker->sigterm_source)
        broker->sigterm_source = 0;
    if (current_id == broker->sigint_source)
        broker->sigint_source = 0;

    revoke_session(broker, "broker shutdown");
    g_main_loop_quit(broker->loop);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", VNC_MONITOR_VERSION);
        return 0;
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("VNC Monitor broker %s\n"
               "Usage: %s [--version]\n"
               "VNC port is read only from /etc/vnc-monitor/config.ini [network] port.\n"
               "Optional HTTPS settings are read only from /etc/vnc-monitor/web.ini.\n",
               VNC_MONITOR_VERSION,
               argv[0]);
        return 0;
    }

    if (argc != 1) {
        fprintf(stderr, "Unexpected broker argument\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    vnc_log_set_level(VNC_LOG_INFO);

    int port = load_public_port();
    if (port < 0)
        return 1;

    Broker broker;
    memset(&broker, 0, sizeof(broker));
    broker.state = BROKER_SESSION_IDLE;
    broker.listener_fd = -1;
    broker.client_fd = -1;
    broker.control_fd = -1;
    broker.uid = (uid_t)-1;
    broker.web_token_uid = (uid_t)-1;
    broker.management_uid = (uid_t)-1;
    broker.vnc_port = port;

    GError *error = NULL;
    broker.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!broker.bus) {
        LOG_ERROR("Broker cannot connect to system D-Bus: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return 1;
    }

    if (resolve_seat_path(&broker) < 0) {
        g_object_unref(broker.bus);
        return 1;
    }

    broker.listener_fd = create_public_listener(port);
    if (broker.listener_fd < 0) {
        LOG_ERROR("Broker cannot listen on TCP/%d: %s", port, strerror(errno));
        g_object_unref(broker.bus);
        return 1;
    }

    broker.loop = g_main_loop_new(NULL, FALSE);

    WebServerHooks web_hooks = {
        .slot_busy = broker_web_slot_busy,
        .slot_state = broker_web_slot_state,
        .begin_auth = broker_web_begin_auth,
        .begin_resume = broker_web_begin_resume,
        .validate_websocket_token = broker_web_validate_websocket_token,
        .bind_websocket = broker_web_bind_websocket,
        .validate_media_token = broker_web_validate_media_token,
        .websocket_protocol_ready = broker_websocket_protocol_ready,
        .websocket_frame_ack = broker_websocket_frame_ack,
        .websocket_frame_nack = broker_websocket_frame_nack,
        .websocket_display_state = broker_websocket_display_state,
        .websocket_client_diagnostic = broker_websocket_client_diagnostic,
        .websocket_closed = broker_websocket_closed,
        .begin_management_auth = broker_management_begin_auth,
        .validate_management_token = broker_validate_management_token,
        .browser_logout = broker_browser_logout,
        .management_logout = broker_management_logout,
        .get_management_info = broker_get_management_info,
        .disconnect_viewer = broker_management_disconnect_viewer,
        .request_restart = broker_request_restart
    };

    int web_rc = web_server_start(&broker.web_server,
                                  WEB_CONFIG_FILE,
                                  &web_hooks,
                                  &broker);
    if (web_rc < 0) {
        LOG_ERROR("Broker HTTPS endpoint is unavailable; continuing with VNC listener only");
    }

    broker.listener_source = g_unix_fd_add(broker.listener_fd,
                                            G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                            listener_ready_cb,
                                            &broker);

    broker.seat_subscription = g_dbus_connection_signal_subscribe(
        broker.bus,
        LOGIN1_NAME,
        DBUS_PROPERTIES_IFACE,
        "PropertiesChanged",
        broker.seat_path,
        LOGIN1_SEAT_IFACE,
        G_DBUS_SIGNAL_FLAGS_NONE,
        logind_changed_cb,
        &broker,
        NULL);

    broker.session_removed_subscription = g_dbus_connection_signal_subscribe(
        broker.bus,
        LOGIN1_NAME,
        LOGIN1_MANAGER_IFACE,
        "SessionRemoved",
        LOGIN1_MANAGER_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        logind_changed_cb,
        &broker,
        NULL);

    /* Signal-driven normally; periodic query is a low-cost fail-safe. */
    broker.periodic_source = g_timeout_add(1000,
                                            periodic_binding_check,
                                            &broker);
    broker.sigterm_source = g_unix_signal_add(SIGTERM, shutdown_cb, &broker);
    broker.sigint_source = g_unix_signal_add(SIGINT, shutdown_cb, &broker);

    LOG_INFO("VNC Monitor broker %s ready on TCP/%d; state=%s; only active seat0 Wayland user sessions are attachable",
             VNC_MONITOR_VERSION,
             port,
             broker_session_state_name(broker.state));

    g_main_loop_run(broker.loop);

    LOG_INFO("Stopping VNC Monitor broker");

    if (broker.listener_source)
        g_source_remove(broker.listener_source);
    if (broker.periodic_source)
        g_source_remove(broker.periodic_source);
    if (broker.sigterm_source)
        g_source_remove(broker.sigterm_source);
    if (broker.sigint_source)
        g_source_remove(broker.sigint_source);
    if (broker.restart_source)
        g_source_remove(broker.restart_source);

    if (broker.seat_subscription)
        g_dbus_connection_signal_unsubscribe(broker.bus,
                                              broker.seat_subscription);
    if (broker.session_removed_subscription)
        g_dbus_connection_signal_unsubscribe(broker.bus,
                                              broker.session_removed_subscription);

    if (broker.management_auth) {
        ManagementAuth *auth = broker.management_auth;
        broker.management_auth = NULL;
        if (auth->source) {
            guint source = auth->source;
            auth->source = 0;
            g_source_remove(source);
        }
        if (auth->control_fd >= 0)
            close(auth->control_fd);
        if (auth->completion)
            auth->completion(WEB_SERVER_AUTH_ERROR, NULL, auth->completion_data);
        g_free(auth);
    }
    broker_invalidate_management_token(&broker);
    broker_invalidate_web_token(&broker);

    if (broker_session_owns_slot(&broker)) {
        if (broker.client_fd >= 0)
            (void)shutdown(broker.client_fd, SHUT_RDWR);
        clear_session(&broker, 0);
    }

    if (broker.web_server) {
        web_server_stop(broker.web_server);
        broker.web_server = NULL;
    }

    if (broker.listener_fd >= 0)
        close(broker.listener_fd);

    g_main_loop_unref(broker.loop);
    g_object_unref(broker.bus);

    LOG_INFO("VNC Monitor broker stopped");
    return broker.restart_requested ? 75 : 0;
}
