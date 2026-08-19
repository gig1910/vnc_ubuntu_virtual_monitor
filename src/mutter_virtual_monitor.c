#include "mutter_virtual_monitor.h"

#include <stdio.h>
#include <string.h>

#define RD_BUS "org.gnome.Mutter.RemoteDesktop"
#define RD_MANAGER_PATH "/org/gnome/Mutter/RemoteDesktop"
#define RD_MANAGER_IFACE "org.gnome.Mutter.RemoteDesktop"
#define RD_SESSION_IFACE "org.gnome.Mutter.RemoteDesktop.Session"

#define SC_BUS "org.gnome.Mutter.ScreenCast"
#define SC_MANAGER_PATH "/org/gnome/Mutter/ScreenCast"
#define SC_MANAGER_IFACE "org.gnome.Mutter.ScreenCast"
#define SC_SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define SC_STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"

typedef struct {
    GMainLoop *loop;
    uint32_t node_id;
    int got_node;
    int timed_out;
} StreamWait;

static int
call_object_path(
    GDBusConnection *bus,
    const char *bus_name,
    const char *path,
    const char *interface_name,
    const char *method,
    GVariant *parameters,
    char **object_path_out)
{
    GError *error = NULL;

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            bus_name,
            path,
            interface_name,
            method,
            parameters,
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error
        );

    if (!reply) {
        fprintf(
            stderr,
            "D-Bus %s.%s failed: %s\n",
            interface_name,
            method,
            error ? error->message : "unknown error"
        );

        g_clear_error(&error);
        return -1;
    }

    const char *object_path = NULL;

    g_variant_get(
        reply,
        "(&o)",
        &object_path
    );

    *object_path_out =
        g_strdup(object_path);

    g_variant_unref(reply);
    return 0;
}

static char *
get_string_property(
    GDBusConnection *bus,
    const char *bus_name,
    const char *path,
    const char *interface_name,
    const char *property)
{
    GError *error = NULL;

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            bus_name,
            path,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new(
                "(ss)",
                interface_name,
                property
            ),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error
        );

    if (!reply) {
        fprintf(
            stderr,
            "D-Bus property %s.%s failed: %s\n",
            interface_name,
            property,
            error ? error->message : "unknown error"
        );

        g_clear_error(&error);
        return NULL;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);

    char *value = NULL;

    if (
        boxed &&
        g_variant_is_of_type(
            boxed,
            G_VARIANT_TYPE_STRING
        )
    ) {
        value =
            g_variant_dup_string(
                boxed,
                NULL
            );
    }

    if (boxed)
        g_variant_unref(boxed);

    g_variant_unref(reply);
    return value;
}

static int
call_void(
    GDBusConnection *bus,
    const char *bus_name,
    const char *path,
    const char *interface_name,
    const char *method)
{
    GError *error = NULL;

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            bus_name,
            path,
            interface_name,
            method,
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error
        );

    if (!reply) {
        fprintf(
            stderr,
            "D-Bus %s.%s failed: %s\n",
            interface_name,
            method,
            error ? error->message : "unknown error"
        );

        g_clear_error(&error);
        return -1;
    }

    g_variant_unref(reply);
    return 0;
}

static void
stream_signal(
    GDBusConnection *connection,
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

    if (
        strcmp(
            signal_name,
            "PipeWireStreamAdded"
        ) != 0
    )
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

int
mutter_virtual_monitor_start(
    MutterVirtualMonitor *monitor,
    int timeout_ms,
    MutterCursorMode cursor_mode)
{
    if (!monitor)
        return -1;

    memset(monitor, 0, sizeof(*monitor));

    GError *error = NULL;

    monitor->bus =
        g_bus_get_sync(
            G_BUS_TYPE_SESSION,
            NULL,
            &error
        );

    if (!monitor->bus) {
        fprintf(
            stderr,
            "Cannot connect to session D-Bus: %s\n",
            error ? error->message : "unknown error"
        );

        g_clear_error(&error);
        return -1;
    }

    if (
        call_object_path(
            monitor->bus,
            RD_BUS,
            RD_MANAGER_PATH,
            RD_MANAGER_IFACE,
            "CreateSession",
            NULL,
            &monitor->rd_session_path
        ) < 0
    )
        goto fail;

    char *session_id =
        get_string_property(
            monitor->bus,
            RD_BUS,
            monitor->rd_session_path,
            RD_SESSION_IFACE,
            "SessionId"
        );

    if (!session_id) {
        fprintf(
            stderr,
            "Mutter RemoteDesktop SessionId is unavailable\n"
        );
        goto fail;
    }

    GVariantBuilder sc_options;
    g_variant_builder_init(
        &sc_options,
        G_VARIANT_TYPE("a{sv}")
    );

    g_variant_builder_add(
        &sc_options,
        "{sv}",
        "remote-desktop-session-id",
        g_variant_new_string(session_id)
    );

    g_free(session_id);

    if (
        call_object_path(
            monitor->bus,
            SC_BUS,
            SC_MANAGER_PATH,
            SC_MANAGER_IFACE,
            "CreateSession",
            g_variant_new(
                "(a{sv})",
                &sc_options
            ),
            &monitor->sc_session_path
        ) < 0
    )
        goto fail;

    GVariantBuilder record_options;
    g_variant_builder_init(
        &record_options,
        G_VARIANT_TYPE("a{sv}")
    );

    g_variant_builder_add(
        &record_options,
        "{sv}",
        "cursor-mode",
        g_variant_new_uint32(
            (guint32)cursor_mode
        )
    );

    g_variant_builder_add(
        &record_options,
        "{sv}",
        "is-platform",
        g_variant_new_boolean(TRUE)
    );

    if (
        call_object_path(
            monitor->bus,
            SC_BUS,
            monitor->sc_session_path,
            SC_SESSION_IFACE,
            "RecordVirtual",
            g_variant_new(
                "(a{sv})",
                &record_options
            ),
            &monitor->stream_path
        ) < 0
    )
        goto fail;

    StreamWait wait = {0};

    wait.loop =
        g_main_loop_new(
            NULL,
            FALSE
        );

    guint subscription =
        g_dbus_connection_signal_subscribe(
            monitor->bus,
            SC_BUS,
            SC_STREAM_IFACE,
            "PipeWireStreamAdded",
            monitor->stream_path,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            stream_signal,
            &wait,
            NULL
        );

    guint timeout_source =
        g_timeout_add(
            (guint)timeout_ms,
            stream_timeout,
            &wait
        );

    /*
     * For a ScreenCast session linked to RemoteDesktop, the session must be
     * started from the RemoteDesktop side. Calling ScreenCast.Session.Start
     * is intentionally not done here.
     */
    if (
        call_void(
            monitor->bus,
            RD_BUS,
            monitor->rd_session_path,
            RD_SESSION_IFACE,
            "Start"
        ) < 0
    ) {
        g_source_remove(timeout_source);
        g_dbus_connection_signal_unsubscribe(
            monitor->bus,
            subscription
        );
        g_main_loop_unref(wait.loop);
        goto fail;
    }

    monitor->started = 1;

    g_main_loop_run(wait.loop);

    if (!wait.timed_out)
        g_source_remove(timeout_source);

    g_dbus_connection_signal_unsubscribe(
        monitor->bus,
        subscription
    );

    g_main_loop_unref(wait.loop);

    if (!wait.got_node) {
        fprintf(
            stderr,
            "Timed out waiting for Mutter PipeWireStreamAdded\n"
        );
        goto fail;
    }

    monitor->node_id = wait.node_id;

    printf(
        "Mutter virtual monitor started: PipeWire node id=%u\n",
        monitor->node_id
    );

    return 0;

fail:
    mutter_virtual_monitor_stop(monitor);
    return -1;
}

void
mutter_virtual_monitor_stop(
    MutterVirtualMonitor *monitor)
{
    if (!monitor)
        return;

    if (
        monitor->started &&
        monitor->bus &&
        monitor->rd_session_path
    ) {
        /*
         * Stop RemoteDesktop session: this is what removes the virtual
         * monitor from the current GNOME Wayland session.
         */
        (void)call_void(
            monitor->bus,
            RD_BUS,
            monitor->rd_session_path,
            RD_SESSION_IFACE,
            "Stop"
        );
    }

    monitor->started = 0;
    monitor->node_id = 0;

    g_clear_pointer(
        &monitor->stream_path,
        g_free
    );

    g_clear_pointer(
        &monitor->sc_session_path,
        g_free
    );

    g_clear_pointer(
        &monitor->rd_session_path,
        g_free
    );

    g_clear_object(&monitor->bus);
}
