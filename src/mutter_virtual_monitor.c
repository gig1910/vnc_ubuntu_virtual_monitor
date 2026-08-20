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

#define TOPOLOGY_SETTLE_TIMEOUT_MS 1500
#define TOPOLOGY_NEAR_CLOSE_US 250000

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

static void
lifecycle_signal(GDBusConnection *connection,
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
    (void)parameters;

    MutterVirtualMonitor *monitor = user_data;

    if (strcmp(interface_name, SC_SESSION_IFACE) == 0 &&
        strcmp(signal_name, "Closed") == 0) {
        int notify_external = 0;

        g_mutex_lock(&monitor->lifecycle_mutex);
        if (!monitor->lifecycle_closed) {
            monitor->lifecycle_closed = 1;
            monitor->lifecycle_closed_at_us = g_get_monotonic_time();
            monitor->lifecycle_display_change_seq_at_close =
                monitor->lifecycle_display_change_seq;
            notify_external = !monitor->lifecycle_intentional_stop;
        }
        g_cond_broadcast(&monitor->lifecycle_cond);
        g_mutex_unlock(&monitor->lifecycle_mutex);

        if (notify_external) {
            LOG_INFO("Mutter ScreenCast session was stopped externally");
            notify_external_lifecycle_close();
        }
        return;
    }

    if (strcmp(interface_name, DISPLAY_CONFIG_IFACE) == 0 &&
        strcmp(signal_name, "MonitorsChanged") == 0) {
        g_mutex_lock(&monitor->lifecycle_mutex);
        monitor->lifecycle_display_change_seq++;
        monitor->lifecycle_last_display_change_us = g_get_monotonic_time();
        g_cond_broadcast(&monitor->lifecycle_cond);
        g_mutex_unlock(&monitor->lifecycle_mutex);
    }
}

static gpointer
lifecycle_thread_main(gpointer user_data)
{
    MutterVirtualMonitor *monitor = user_data;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    guint closed_subscription =
        g_dbus_connection_signal_subscribe(monitor->bus,
                                           SC_BUS,
                                           SC_SESSION_IFACE,
                                           "Closed",
                                           monitor->sc_session_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           lifecycle_signal,
                                           monitor,
                                           NULL);

    guint monitors_subscription =
        g_dbus_connection_signal_subscribe(monitor->bus,
                                           DISPLAY_CONFIG_BUS,
                                           DISPLAY_CONFIG_IFACE,
                                           "MonitorsChanged",
                                           DISPLAY_CONFIG_PATH,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           lifecycle_signal,
                                           monitor,
                                           NULL);

    g_mutex_lock(&monitor->lifecycle_mutex);
    monitor->lifecycle_context = g_main_context_ref(context);
    monitor->lifecycle_closed_subscription = closed_subscription;
    monitor->lifecycle_monitors_subscription = monitors_subscription;
    monitor->lifecycle_ready = 1;
    g_cond_broadcast(&monitor->lifecycle_cond);
    g_mutex_unlock(&monitor->lifecycle_mutex);

    for (;;) {
        g_mutex_lock(&monitor->lifecycle_mutex);
        int stop_requested = monitor->lifecycle_stop_requested;
        g_mutex_unlock(&monitor->lifecycle_mutex);

        if (stop_requested)
            break;

        (void)g_main_context_iteration(context, TRUE);
    }

    if (closed_subscription != 0)
        g_dbus_connection_signal_unsubscribe(monitor->bus,
                                             closed_subscription);
    if (monitors_subscription != 0)
        g_dbus_connection_signal_unsubscribe(monitor->bus,
                                             monitors_subscription);

    g_mutex_lock(&monitor->lifecycle_mutex);
    monitor->lifecycle_closed_subscription = 0;
    monitor->lifecycle_monitors_subscription = 0;
    GMainContext *stored_context = monitor->lifecycle_context;
    monitor->lifecycle_context = NULL;
    g_cond_broadcast(&monitor->lifecycle_cond);
    g_mutex_unlock(&monitor->lifecycle_mutex);

    if (stored_context)
        g_main_context_unref(stored_context);

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return NULL;
}

static int
start_lifecycle_watcher(MutterVirtualMonitor *monitor)
{
    ensure_lifecycle_pipe();

    g_mutex_init(&monitor->lifecycle_mutex);
    g_cond_init(&monitor->lifecycle_cond);
    monitor->lifecycle_sync_initialized = 1;

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
        g_cond_clear(&monitor->lifecycle_cond);
        g_mutex_clear(&monitor->lifecycle_mutex);
        monitor->lifecycle_sync_initialized = 0;
        return -1;
    }

    g_mutex_lock(&monitor->lifecycle_mutex);
    while (!monitor->lifecycle_ready)
        g_cond_wait(&monitor->lifecycle_cond, &monitor->lifecycle_mutex);
    g_mutex_unlock(&monitor->lifecycle_mutex);

    return 0;
}

static void
wait_for_topology_settle(MutterVirtualMonitor *monitor)
{
    if (!monitor->lifecycle_sync_initialized)
        return;

    const gint64 deadline =
        g_get_monotonic_time() +
        (gint64)TOPOLOGY_SETTLE_TIMEOUT_MS * 1000;

    int closed = 0;
    int topology_seen = 0;

    g_mutex_lock(&monitor->lifecycle_mutex);

    while (!monitor->lifecycle_closed) {
        if (!g_cond_wait_until(&monitor->lifecycle_cond,
                               &monitor->lifecycle_mutex,
                               deadline))
            break;
    }

    closed = monitor->lifecycle_closed;

    while (closed) {
        const int changed_after_close =
            monitor->lifecycle_display_change_seq >
            monitor->lifecycle_display_change_seq_at_close;

        gint64 delta_us =
            monitor->lifecycle_last_display_change_us -
            monitor->lifecycle_closed_at_us;
        if (delta_us < 0)
            delta_us = -delta_us;

        const int changed_near_close =
            monitor->lifecycle_last_display_change_us > 0 &&
            delta_us <= TOPOLOGY_NEAR_CLOSE_US;

        if (changed_after_close || changed_near_close) {
            topology_seen = 1;
            break;
        }

        if (!g_cond_wait_until(&monitor->lifecycle_cond,
                               &monitor->lifecycle_mutex,
                               deadline))
            break;
    }

    g_mutex_unlock(&monitor->lifecycle_mutex);

    if (!closed) {
        LOG_DEBUG("Mutter ScreenCast Closed was not observed during teardown");
    }
    else if (!topology_seen) {
        LOG_DEBUG("Mutter DisplayConfig did not signal a topology change within %d ms",
                  TOPOLOGY_SETTLE_TIMEOUT_MS);
    }
    else {
        LOG_DEBUG("Mutter monitor topology settled after ScreenCast close");
    }
}

static void
stop_lifecycle_watcher(MutterVirtualMonitor *monitor)
{
    if (!monitor->lifecycle_sync_initialized)
        return;

    GMainContext *context = NULL;

    g_mutex_lock(&monitor->lifecycle_mutex);
    monitor->lifecycle_stop_requested = 1;
    if (monitor->lifecycle_context)
        context = g_main_context_ref(monitor->lifecycle_context);
    g_mutex_unlock(&monitor->lifecycle_mutex);

    if (context) {
        g_main_context_wakeup(context);
        g_main_context_unref(context);
    }

    if (monitor->lifecycle_thread) {
        g_thread_join(monitor->lifecycle_thread);
        monitor->lifecycle_thread = NULL;
    }

    g_cond_clear(&monitor->lifecycle_cond);
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

    if (start_lifecycle_watcher(monitor) < 0)
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

    if (monitor->lifecycle_sync_initialized) {
        g_mutex_lock(&monitor->lifecycle_mutex);
        already_closed = monitor->lifecycle_closed;
        if (!already_closed)
            monitor->lifecycle_intentional_stop = 1;
        g_mutex_unlock(&monitor->lifecycle_mutex);
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
        wait_for_topology_settle(monitor);

    stop_lifecycle_watcher(monitor);

    monitor->started = 0;
    monitor->node_id = 0;
    g_clear_pointer(&monitor->stream_path, g_free);
    g_clear_pointer(&monitor->sc_session_path, g_free);
    g_clear_pointer(&monitor->rd_session_path, g_free);
    g_clear_object(&monitor->bus);
}
