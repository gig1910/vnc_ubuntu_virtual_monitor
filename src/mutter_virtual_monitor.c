#include "mutter_virtual_monitor.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define RD_BUS "org.gnome.Mutter.RemoteDesktop"
#define RD_MANAGER_PATH "/org/gnome/Mutter/RemoteDesktop"
#define RD_MANAGER_IFACE "org.gnome.Mutter.RemoteDesktop"
#define RD_SESSION_IFACE "org.gnome.Mutter.RemoteDesktop.Session"

#define SC_BUS "org.gnome.Mutter.ScreenCast"
#define SC_MANAGER_PATH "/org/gnome/Mutter/ScreenCast"
#define SC_MANAGER_IFACE "org.gnome.Mutter.ScreenCast"
#define SC_SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define SC_STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"

#define DISPLAY_CONFIG_BUS "org.gnome.Mutter.DisplayConfig"
#define DISPLAY_CONFIG_PATH "/org/gnome/Mutter/DisplayConfig"
#define DISPLAY_CONFIG_IFACE "org.gnome.Mutter.DisplayConfig"

#define LIFECYCLE_POLL_US 100000
#define TOPOLOGY_SETTLE_TIMEOUT_MS 1500

typedef struct {
    GMainLoop *loop;
    uint32_t node_id;
    int got_node;
    int timed_out;
} StreamWait;

/*
 * There is only one external viewer slot. Keep one process-wide notification
 * pipe stable across internal virtual-monitor recreation so SetDesktopSize
 * cannot accidentally look like an external Stop request to the RFB relay.
 */
static int lifecycle_pipe[2] = {-1, -1};
static gsize lifecycle_pipe_initialized = 0;

static int
set_fd_flags(int fd)
{
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        return -1;

    int status_flags = fcntl(fd, F_GETFL);
    if (status_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
        return -1;

    return 0;
}

static void
ensure_lifecycle_pipe(void)
{
    if (!g_once_init_enter(&lifecycle_pipe_initialized))
        return;

    int fds[2] = {-1, -1};
    if (pipe(fds) == 0 &&
        set_fd_flags(fds[0]) == 0 &&
        set_fd_flags(fds[1]) == 0) {
        lifecycle_pipe[0] = fds[0];
        lifecycle_pipe[1] = fds[1];
    }
    else {
        if (fds[0] >= 0)
            close(fds[0]);
        if (fds[1] >= 0)
            close(fds[1]);
        LOG_ERROR("Could not create Mutter lifecycle notification pipe: %s",
                  strerror(errno));
    }

    g_once_init_leave(&lifecycle_pipe_initialized, 1);
}

int
mutter_virtual_monitor_lifecycle_fd(void)
{
    ensure_lifecycle_pipe();
    return lifecycle_pipe[0];
}

void
mutter_virtual_monitor_lifecycle_drain(void)
{
    ensure_lifecycle_pipe();
    if (lifecycle_pipe[0] < 0)
        return;

    unsigned char buffer[64];
    for (;;) {
        ssize_t n = read(lifecycle_pipe[0], buffer, sizeof(buffer));
        if (n > 0)
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

static void
notify_external_lifecycle_close(void)
{
    ensure_lifecycle_pipe();
    if (lifecycle_pipe[1] < 0)
        return;

    const unsigned char byte = 1;
    ssize_t n;
    do {
        n = write(lifecycle_pipe[1], &byte, sizeof(byte));
    } while (n < 0 && errno == EINTR);

    /* EAGAIN only means a close notification is already pending. */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        LOG_DEBUG("Could not signal Mutter lifecycle close: %s", strerror(errno));
}

static int
call_object_path(GDBusConnection *bus,
                 const char *bus_name,
                 const char *path,
                 const char *interface_name,
                 const char *method,
                 GVariant *parameters,
                 char **object_path_out)
{
    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(bus,
                                    bus_name,
                                    path,
                                    interface_name,
                                    method,
                                    parameters,
                                    G_VARIANT_TYPE("(o)"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    5000,
                                    NULL,
                                    &error);

    if (!reply) {
        LOG_ERROR("D-Bus %s.%s failed: %s",
                  interface_name,
                  method,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    const char *object_path = NULL;
    g_variant_get(reply, "(&o)", &object_path);
    *object_path_out = g_strdup(object_path);
    g_variant_unref(reply);
    return 0;
}

static char *
get_string_property(GDBusConnection *bus,
                    const char *bus_name,
                    const char *path,
                    const char *interface_name,
                    const char *property)
{
    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(bus,
                                    bus_name,
                                    path,
                                    "org.freedesktop.DBus.Properties",
                                    "Get",
                                    g_variant_new("(ss)",
                                                  interface_name,
                                                  property),
                                    G_VARIANT_TYPE("(v)"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    5000,
                                    NULL,
                                    &error);

    if (!reply) {
        LOG_ERROR("D-Bus property %s.%s failed: %s",
                  interface_name,
                  property,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    char *value = NULL;

    if (boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_STRING))
        value = g_variant_dup_string(boxed, NULL);

    if (boxed)
        g_variant_unref(boxed);
    g_variant_unref(reply);
    return value;
}

/*
 * Quiet liveness probe for the exact RemoteDesktop session object. Some
 * Mutter/GNOME versions remove this object when Shell's Stop action is used
 * without delivering a useful Closed signal to our connection. Object
 * disappearance is therefore the authoritative lifecycle condition.
 */
static int
rd_session_is_alive(GDBusConnection *bus, const char *path)
{
    if (!bus || !path)
        return 0;

    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(bus,
                                    RD_BUS,
                                    path,
                                    "org.freedesktop.DBus.Properties",
                                    "Get",
                                    g_variant_new("(ss)",
                                                  RD_SESSION_IFACE,
                                                  "SessionId"),
                                    G_VARIANT_TYPE("(v)"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    1000,
                                    NULL,
                                    &error);

    if (!reply) {
        g_clear_error(&error);
        return 0;
    }

    g_variant_unref(reply);
    return 1;
}

static int
get_display_serial(GDBusConnection *bus, guint32 *serial_out)
{
    if (!bus || !serial_out)
        return -1;

    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(bus,
                                    DISPLAY_CONFIG_BUS,
                                    DISPLAY_CONFIG_PATH,
                                    DISPLAY_CONFIG_IFACE,
                                    "GetCurrentState",
                                    NULL,
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    1000,
                                    NULL,
                                    &error);

    if (!reply) {
        g_clear_error(&error);
        return -1;
    }

    GVariant *serial_value = g_variant_get_child_value(reply, 0);
    if (!serial_value ||
        !g_variant_is_of_type(serial_value, G_VARIANT_TYPE_UINT32)) {
        if (serial_value)
            g_variant_unref(serial_value);
        g_variant_unref(reply);
        return -1;
    }

    *serial_out = g_variant_get_uint32(serial_value);
    g_variant_unref(serial_value);
    g_variant_unref(reply);
    return 0;
}

static int
call_void(GDBusConnection *bus,
          const char *bus_name,
          const char *path,
          const char *interface_name,
          const char *method)
{
    GError *error = NULL;
    GVariant *reply =
        g_dbus_connection_call_sync(bus,
                                    bus_name,
                                    path,
                                    interface_name,
                                    method,
                                    NULL,
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    5000,
                                    NULL,
                                    &error);

    if (!reply) {
        LOG_ERROR("D-Bus %s.%s failed: %s",
                  interface_name,
                  method,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    g_variant_unref(reply);
    return 0;
}

static void
stream_signal(GDBusConnection *connection,
              const char *sender_name,
              const char *object_path,
              const char *interface_name,
              const char *signal_name,
              GVariant *parameters,
              gpointer user_data)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;

    StreamWait *wait = user_data;
    if (strcmp(signal_name, "PipeWireStreamAdded") != 0)
        return;

    guint32 node_id = 0;
    g_variant_get(parameters, "(u)", &node_id);
    wait->node_id = node_id;
    wait->got_node = 1;
    g_main_loop_quit(wait->loop);
}

static gboolean
stream_timeout(gpointer user_data)
{
    StreamWait *wait = user_data;
    wait->timed_out = 1;
    g_main_loop_quit(wait->loop);
    return G_SOURCE_REMOVE;
}

static gpointer
lifecycle_thread_main(gpointer user_data)
{
    MutterVirtualMonitor *monitor = user_data;

    for (;;) {
        int stop_requested = 0;
        int intentional_stop = 0;

        g_mutex_lock(&monitor->lifecycle_mutex);
        stop_requested = monitor->lifecycle_stop_requested;
        intentional_stop = monitor->lifecycle_intentional_stop;
        g_mutex_unlock(&monitor->lifecycle_mutex);

        if (stop_requested)
            break;

        if (intentional_stop) {
            g_usleep(LIFECYCLE_POLL_US);
            continue;
        }

        if (!rd_session_is_alive(monitor->bus, monitor->rd_session_path)) {
            int notify_external = 0;

            g_mutex_lock(&monitor->lifecycle_mutex);
            if (!monitor->lifecycle_closed) {
                monitor->lifecycle_closed = 1;
                monitor->lifecycle_display_serial_before_close =
                    monitor->lifecycle_last_display_serial;
                notify_external = !monitor->lifecycle_intentional_stop;
            }
            g_mutex_unlock(&monitor->lifecycle_mutex);

            if (notify_external) {
                LOG_INFO("Mutter RemoteDesktop session disappeared externally");
                notify_external_lifecycle_close();
            }
            break;
        }

        guint32 serial = 0;
        if (get_display_serial(monitor->bus, &serial) == 0) {
            g_mutex_lock(&monitor->lifecycle_mutex);
            monitor->lifecycle_last_display_serial = serial;
            g_mutex_unlock(&monitor->lifecycle_mutex);
        }

        g_usleep(LIFECYCLE_POLL_US);
    }

    return NULL;
}

static int
start_lifecycle_watcher(MutterVirtualMonitor *monitor)
{
    ensure_lifecycle_pipe();

    g_mutex_init(&monitor->lifecycle_mutex);
    monitor->lifecycle_sync_initialized = 1;

    guint32 serial = 0;
    if (get_display_serial(monitor->bus, &serial) == 0)
        monitor->lifecycle_last_display_serial = serial;

    GError *error = NULL;
    monitor->lifecycle_thread =
        g_thread_try_new("vnc-mutter-lifecycle",
                         lifecycle_thread_main,
                         monitor,
                         &error);

    if (!monitor->lifecycle_thread) {
        LOG_ERROR("Could not start Mutter lifecycle watcher: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        g_mutex_clear(&monitor->lifecycle_mutex);
        monitor->lifecycle_sync_initialized = 0;
        return -1;
    }

    return 0;
}

static void
wait_for_topology_settle(MutterVirtualMonitor *monitor,
                         guint32 serial_before_close)
{
    if (!monitor || !monitor->bus)
        return;

    const gint64 deadline =
        g_get_monotonic_time() +
        (gint64)TOPOLOGY_SETTLE_TIMEOUT_MS * 1000;

    int session_gone = 0;
    int topology_changed = 0;

    while (g_get_monotonic_time() < deadline) {
        session_gone =
            !rd_session_is_alive(monitor->bus, monitor->rd_session_path);

        guint32 serial_now = 0;
        if (get_display_serial(monitor->bus, &serial_now) == 0) {
            topology_changed =
                serial_before_close == 0 ||
                serial_now != serial_before_close;
        }

        if (session_gone && topology_changed)
            break;

        g_usleep(50000);
    }

    if (!session_gone) {
        LOG_DEBUG("Mutter RemoteDesktop session still appeared alive after Stop timeout");
    }
    else if (!topology_changed) {
        LOG_DEBUG("Mutter DisplayConfig serial did not advance within %d ms",
                  TOPOLOGY_SETTLE_TIMEOUT_MS);
    }
    else {
        LOG_DEBUG("Mutter monitor topology settled after RemoteDesktop teardown");
    }
}

static void
stop_lifecycle_watcher(MutterVirtualMonitor *monitor)
{
    if (!monitor->lifecycle_sync_initialized)
        return;

    g_mutex_lock(&monitor->lifecycle_mutex);
    monitor->lifecycle_stop_requested = 1;
    g_mutex_unlock(&monitor->lifecycle_mutex);

    if (monitor->lifecycle_thread) {
        g_thread_join(monitor->lifecycle_thread);
        monitor->lifecycle_thread = NULL;
    }

    g_mutex_clear(&monitor->lifecycle_mutex);
    monitor->lifecycle_sync_initialized = 0;
}

int
mutter_virtual_monitor_start(MutterVirtualMonitor *monitor,
                             int timeout_ms,
                             MutterCursorMode cursor_mode)
{
    if (!monitor)
        return -1;

    memset(monitor, 0, sizeof(*monitor));

    GError *error = NULL;
    monitor->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!monitor->bus) {
        LOG_ERROR("Cannot connect to session D-Bus: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    if (call_object_path(monitor->bus,
                         RD_BUS,
                         RD_MANAGER_PATH,
                         RD_MANAGER_IFACE,
                         "CreateSession",
                         NULL,
                         &monitor->rd_session_path) < 0)
        goto fail;

    char *session_id =
        get_string_property(monitor->bus,
                            RD_BUS,
                            monitor->rd_session_path,
                            RD_SESSION_IFACE,
                            "SessionId");
    if (!session_id) {
        LOG_ERROR("Mutter RemoteDesktop SessionId is unavailable");
        goto fail;
    }

    GVariantBuilder sc_options;
    g_variant_builder_init(&sc_options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&sc_options,
                          "{sv}",
                          "remote-desktop-session-id",
                          g_variant_new_string(session_id));
    g_free(session_id);

    if (call_object_path(monitor->bus,
                         SC_BUS,
                         SC_MANAGER_PATH,
                         SC_MANAGER_IFACE,
                         "CreateSession",
                         g_variant_new("(a{sv})", &sc_options),
                         &monitor->sc_session_path) < 0)
        goto fail;

    GVariantBuilder record_options;
    g_variant_builder_init(&record_options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&record_options,
                          "{sv}",
                          "cursor-mode",
                          g_variant_new_uint32((guint32)cursor_mode));
    g_variant_builder_add(&record_options,
                          "{sv}",
                          "is-platform",
                          g_variant_new_boolean(TRUE));

    if (call_object_path(monitor->bus,
                         SC_BUS,
                         monitor->sc_session_path,
                         SC_SESSION_IFACE,
                         "RecordVirtual",
                         g_variant_new("(a{sv})", &record_options),
                         &monitor->stream_path) < 0)
        goto fail;

    StreamWait wait = {0};
    wait.loop = g_main_loop_new(NULL, FALSE);

    guint subscription =
        g_dbus_connection_signal_subscribe(monitor->bus,
                                           SC_BUS,
                                           SC_STREAM_IFACE,
                                           "PipeWireStreamAdded",
                                           monitor->stream_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           stream_signal,
                                           &wait,
                                           NULL);

    guint timeout_source =
        g_timeout_add((guint)timeout_ms, stream_timeout, &wait);

    /* A ScreenCast session linked to RemoteDesktop is started from RD only. */
    if (call_void(monitor->bus,
                  RD_BUS,
                  monitor->rd_session_path,
                  RD_SESSION_IFACE,
                  "Start") < 0) {
        g_source_remove(timeout_source);
        g_dbus_connection_signal_unsubscribe(monitor->bus, subscription);
        g_main_loop_unref(wait.loop);
        goto fail;
    }

    monitor->started = 1;
    g_main_loop_run(wait.loop);

    if (!wait.timed_out)
        g_source_remove(timeout_source);

    g_dbus_connection_signal_unsubscribe(monitor->bus, subscription);
    g_main_loop_unref(wait.loop);

    if (!wait.got_node) {
        LOG_ERROR("Timed out waiting for Mutter PipeWireStreamAdded");
        goto fail;
    }

    monitor->node_id = wait.node_id;

    if (start_lifecycle_watcher(monitor) < 0)
        goto fail;

    LOG_DEBUG("Mutter virtual monitor started: PipeWire node id=%u",
              monitor->node_id);
    return 0;

fail:
    mutter_virtual_monitor_stop(monitor);
    return -1;
}

void
mutter_virtual_monitor_stop(MutterVirtualMonitor *monitor)
{
    if (!monitor)
        return;

    int already_closed = 0;
    guint32 serial_before_close = 0;

    if (monitor->lifecycle_sync_initialized) {
        g_mutex_lock(&monitor->lifecycle_mutex);
        already_closed = monitor->lifecycle_closed;
        if (already_closed) {
            serial_before_close =
                monitor->lifecycle_display_serial_before_close;
        }
        else {
            monitor->lifecycle_intentional_stop = 1;
            serial_before_close = monitor->lifecycle_last_display_serial;
        }
        g_mutex_unlock(&monitor->lifecycle_mutex);
    }
    else {
        (void)get_display_serial(monitor->bus, &serial_before_close);
    }

    if (monitor->started &&
        !already_closed &&
        monitor->bus &&
        monitor->rd_session_path) {
        /* Stopping RD is what removes the virtual monitor from GNOME. */
        (void)call_void(monitor->bus,
                        RD_BUS,
                        monitor->rd_session_path,
                        RD_SESSION_IFACE,
                        "Stop");
    }

    if (monitor->started)
        wait_for_topology_settle(monitor, serial_before_close);

    stop_lifecycle_watcher(monitor);

    monitor->started = 0;
    monitor->node_id = 0;
    g_clear_pointer(&monitor->stream_path, g_free);
    g_clear_pointer(&monitor->sc_session_path, g_free);
    g_clear_pointer(&monitor->rd_session_path, g_free);
    g_clear_object(&monitor->bus);
}
