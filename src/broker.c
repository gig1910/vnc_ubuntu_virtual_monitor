#define _GNU_SOURCE

#include "broker_protocol.h"
#include "config.h"
#include "log.h"

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
#define BROKER_DEFAULT_PORT      5901
#define BROKER_AGENT_DIR         "vnc-monitor"
#define BROKER_AGENT_SOCKET      "agent.sock"

typedef struct {
    int valid;
    uid_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char object_path[256];
    char user_name[128];
} ActiveSession;

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

    BrokerSessionState state;
    int client_fd;
    int control_fd;
    guint control_source;
    uid_t uid;
    char session_id[VNC_BROKER_SESSION_ID_MAX];
    char peer_addr[VNC_BROKER_PEER_ADDR_MAX];
} Broker;

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
clear_session(Broker *broker, int reset)
{
    if (!broker)
        return;

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

    LOG_INFO("Broker revoking %s session for %s: bound session %s is no longer active (%s)",
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
         * Future WebRTC path has no browser fd in the agent. Control-channel
         * loss is therefore the authoritative fail-safe lifetime guard.
         */
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
control_ready_cb(gint fd, GIOCondition condition, gpointer user_data)
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
                                            control_ready_cb,
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

    enforce_session_binding((Broker *)user_data);
}

static gboolean
periodic_binding_check(gpointer user_data)
{
    enforce_session_binding((Broker *)user_data);
    return G_SOURCE_CONTINUE;
}

static gboolean
shutdown_cb(gpointer user_data)
{
    Broker *broker = user_data;
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
               "Public port is read only from /etc/vnc-monitor/config.ini [network] port.\n",
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

    if (broker.seat_subscription)
        g_dbus_connection_signal_unsubscribe(broker.bus,
                                              broker.seat_subscription);
    if (broker.session_removed_subscription)
        g_dbus_connection_signal_unsubscribe(broker.bus,
                                              broker.session_removed_subscription);

    if (broker_session_owns_slot(&broker)) {
        if (broker.client_fd >= 0)
            (void)shutdown(broker.client_fd, SHUT_RDWR);
        clear_session(&broker, 0);
    }

    if (broker.listener_fd >= 0)
        close(broker.listener_fd);

    g_main_loop_unref(broker.loop);
    g_object_unref(broker.bus);

    LOG_INFO("VNC Monitor broker stopped");
    return 0;
}
