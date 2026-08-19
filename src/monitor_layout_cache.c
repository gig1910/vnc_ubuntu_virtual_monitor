#include "monitor_layout_cache.h"

#include <glib.h>

#include <stdio.h>
#include <string.h>

#define DISPLAY_CONFIG_BUS \
    "org.gnome.Mutter.DisplayConfig"

#define DISPLAY_CONFIG_PATH \
    "/org/gnome/Mutter/DisplayConfig"

#define DISPLAY_CONFIG_IFACE \
    "org.gnome.Mutter.DisplayConfig"

typedef struct {
    char *connector;
    char *vendor;
    char *product;
    char *serial;
    char *current_mode;
    int current_width;
    int current_height;
    double current_refresh;
} CurrentMonitor;

typedef struct {
    CurrentMonitor *items;
    size_t count;
} CurrentMonitorList;

static void
current_monitor_clear(CurrentMonitor *monitor)
{
    if (!monitor)
        return;

    g_free(monitor->connector);
    g_free(monitor->vendor);
    g_free(monitor->product);
    g_free(monitor->serial);
    g_free(monitor->current_mode);

    memset(monitor, 0, sizeof(*monitor));
}

static void
current_monitor_list_clear(
    CurrentMonitorList *list)
{
    if (!list)
        return;

    for (size_t i = 0; i < list->count; i++)
        current_monitor_clear(
            &list->items[i]
        );

    g_free(list->items);
    memset(list, 0, sizeof(*list));
}

static GVariant *
display_config_get_current_state(
    GDBusConnection *bus)
{
    GError *error = NULL;

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            DISPLAY_CONFIG_BUS,
            DISPLAY_CONFIG_PATH,
            DISPLAY_CONFIG_IFACE,
            "GetCurrentState",
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
            "DisplayConfig.GetCurrentState failed: %s\n",
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        return NULL;
    }

    return reply;
}

static int
get_mode_flag(
    GVariant *properties,
    const char *name)
{
    gboolean value = FALSE;

    if (
        properties &&
        g_variant_lookup(
            properties,
            name,
            "b",
            &value
        )
    )
        return value ? 1 : 0;

    return 0;
}

static int
current_monitors_from_state(
    GVariant *state,
    CurrentMonitorList *out)
{
    if (!state || !out)
        return -1;

    guint32 serial = 0;
    GVariant *physical = NULL;
    GVariant *logical = NULL;
    GVariant *properties = NULL;

    g_variant_get(
        state,
        "(u@a((ssss)a(siiddada{sv})a{sv})"
        "@a(iiduba(ssss)a{sv})"
        "@a{sv})",
        &serial,
        &physical,
        &logical,
        &properties
    );

    (void)serial;

    GVariantIter iter;
    g_variant_iter_init(
        &iter,
        physical
    );

    GVariant *spec = NULL;
    GVariant *modes = NULL;
    GVariant *monitor_properties = NULL;

    while (
        g_variant_iter_next(
            &iter,
            "(@(ssss)@a(siiddada{sv})@a{sv})",
            &spec,
            &modes,
            &monitor_properties
        )
    ) {
        const char *connector = NULL;
        const char *vendor = NULL;
        const char *product = NULL;
        const char *monitor_serial = NULL;

        g_variant_get(
            spec,
            "(&s&s&s&s)",
            &connector,
            &vendor,
            &product,
            &monitor_serial
        );

        char *selected_mode = NULL;
        char *preferred_mode = NULL;
        int selected_width = 0;
        int selected_height = 0;
        double selected_refresh = 0.0;
        int preferred_width = 0;
        int preferred_height = 0;
        double preferred_refresh = 0.0;

        GVariantIter mode_iter;
        g_variant_iter_init(
            &mode_iter,
            modes
        );

        const char *mode_id = NULL;
        gint32 width = 0;
        gint32 height = 0;
        gdouble refresh = 0.0;
        gdouble preferred_scale = 1.0;
        GVariant *supported_scales = NULL;
        GVariant *mode_properties = NULL;

        while (
            g_variant_iter_next(
                &mode_iter,
                "(&siidd@ad@a{sv})",
                &mode_id,
                &width,
                &height,
                &refresh,
                &preferred_scale,
                &supported_scales,
                &mode_properties
            )
        ) {
            (void)preferred_scale;

            if (
                !selected_mode &&
                get_mode_flag(
                    mode_properties,
                    "is-current"
                )
            ) {
                selected_mode =
                    g_strdup(mode_id);
                selected_width = width;
                selected_height = height;
                selected_refresh = refresh;
            }

            if (
                !preferred_mode &&
                get_mode_flag(
                    mode_properties,
                    "is-preferred"
                )
            ) {
                preferred_mode =
                    g_strdup(mode_id);
                preferred_width = width;
                preferred_height = height;
                preferred_refresh = refresh;
            }

            g_variant_unref(
                supported_scales
            );

            g_variant_unref(
                mode_properties
            );
        }

        if (!selected_mode) {
            selected_mode =
                preferred_mode;
            selected_width = preferred_width;
            selected_height = preferred_height;
            selected_refresh = preferred_refresh;

            preferred_mode = NULL;
        }

        g_free(preferred_mode);

        CurrentMonitor *new_items =
            g_realloc_n(
                out->items,
                out->count + 1,
                sizeof(*out->items)
            );

        out->items = new_items;

        CurrentMonitor *item =
            &out->items[out->count++];

        memset(item, 0, sizeof(*item));

        item->connector =
            g_strdup(connector);

        item->vendor =
            g_strdup(vendor);

        item->product =
            g_strdup(product);

        item->serial =
            g_strdup(monitor_serial);

        item->current_mode =
            selected_mode;
        item->current_width = selected_width;
        item->current_height = selected_height;
        item->current_refresh = selected_refresh;

        g_variant_unref(spec);
        g_variant_unref(modes);
        g_variant_unref(
            monitor_properties
        );

        spec = NULL;
        modes = NULL;
        monitor_properties = NULL;
    }

    g_variant_unref(physical);
    g_variant_unref(logical);
    g_variant_unref(properties);

    return 0;
}

static CurrentMonitor *
find_current_monitor(
    CurrentMonitorList *list,
    const char *connector,
    const char *vendor,
    const char *product,
    const char *serial)
{
    if (!list)
        return NULL;

    /*
     * Connector names are preferred when Mutter keeps them stable.
     */
    for (size_t i = 0; i < list->count; i++) {
        if (
            connector &&
            list->items[i].connector &&
            strcmp(
                connector,
                list->items[i].connector
            ) == 0
        )
            return &list->items[i];
    }

    /*
     * Fallback for a virtual connector whose runtime connector name changes:
     * match the monitor spec returned by Mutter.
     */
    for (size_t i = 0; i < list->count; i++) {
        CurrentMonitor *item =
            &list->items[i];

        if (
            g_strcmp0(
                vendor,
                item->vendor
            ) == 0 &&
            g_strcmp0(
                product,
                item->product
            ) == 0 &&
            g_strcmp0(
                serial,
                item->serial
            ) == 0
        )
            return item;
    }

    return NULL;
}

static int
keyfile_save_state(
    GKeyFile *keyfile,
    GVariant *state)
{
    guint32 serial = 0;
    GVariant *physical = NULL;
    GVariant *logical = NULL;
    GVariant *properties = NULL;

    g_variant_get(
        state,
        "(u@a((ssss)a(siiddada{sv})a{sv})"
        "@a(iiduba(ssss)a{sv})"
        "@a{sv})",
        &serial,
        &physical,
        &logical,
        &properties
    );

    (void)serial;

    CurrentMonitorList current = {0};

    /*
     * Re-read physical monitor mode state from this same reply.
     */
    GVariant *state_ref =
        g_variant_ref(state);

    if (
        current_monitors_from_state(
            state_ref,
            &current
        ) < 0
    ) {
        g_variant_unref(state_ref);
        g_variant_unref(physical);
        g_variant_unref(logical);
        g_variant_unref(properties);
        return -1;
    }

    g_variant_unref(state_ref);

    GVariantIter logical_iter;
    g_variant_iter_init(
        &logical_iter,
        logical
    );

    gint32 x = 0;
    gint32 y = 0;
    gdouble scale = 1.0;
    guint32 transform = 0;
    gboolean primary = FALSE;
    GVariant *specs = NULL;
    GVariant *logical_properties = NULL;

    int logical_index = 0;

    while (
        g_variant_iter_next(
            &logical_iter,
            "(iidub@a(ssss)@a{sv})",
            &x,
            &y,
            &scale,
            &transform,
            &primary,
            &specs,
            &logical_properties
        )
    ) {
        char *group =
            g_strdup_printf(
                "logical.%d",
                logical_index
            );

        g_key_file_set_integer(
            keyfile,
            group,
            "x",
            x
        );

        g_key_file_set_integer(
            keyfile,
            group,
            "y",
            y
        );

        g_key_file_set_double(
            keyfile,
            group,
            "scale",
            scale
        );

        g_key_file_set_uint64(
            keyfile,
            group,
            "transform",
            transform
        );

        g_key_file_set_boolean(
            keyfile,
            group,
            "primary",
            primary
        );

        GVariantIter spec_iter;
        g_variant_iter_init(
            &spec_iter,
            specs
        );

        const char *connector = NULL;
        const char *vendor = NULL;
        const char *product = NULL;
        const char *monitor_serial = NULL;

        int monitor_index = 0;

        while (
            g_variant_iter_next(
                &spec_iter,
                "(&s&s&s&s)",
                &connector,
                &vendor,
                &product,
                &monitor_serial
            )
        ) {
            CurrentMonitor *current_monitor =
                find_current_monitor(
                    &current,
                    connector,
                    vendor,
                    product,
                    monitor_serial
                );

            char *monitor_group =
                g_strdup_printf(
                    "logical.%d.monitor.%d",
                    logical_index,
                    monitor_index
                );

            g_key_file_set_string(
                keyfile,
                monitor_group,
                "connector",
                connector
            );

            g_key_file_set_string(
                keyfile,
                monitor_group,
                "vendor",
                vendor
            );

            g_key_file_set_string(
                keyfile,
                monitor_group,
                "product",
                product
            );

            g_key_file_set_string(
                keyfile,
                monitor_group,
                "serial",
                monitor_serial
            );

            if (
                current_monitor &&
                current_monitor->current_mode
            ) {
                g_key_file_set_string(
                    keyfile,
                    monitor_group,
                    "mode",
                    current_monitor->
                        current_mode
                );
            }

            g_free(monitor_group);
            monitor_index++;
        }

        g_key_file_set_integer(
            keyfile,
            group,
            "monitor-count",
            monitor_index
        );

        g_free(group);

        g_variant_unref(specs);
        g_variant_unref(
            logical_properties
        );

        logical_index++;
    }

    g_key_file_set_integer(
        keyfile,
        "layout",
        "logical-count",
        logical_index
    );

    current_monitor_list_clear(
        &current
    );

    g_variant_unref(physical);
    g_variant_unref(logical);
    g_variant_unref(properties);

    return logical_index > 0
        ? 0
        : -1;
}

static int
load_string(
    GKeyFile *keyfile,
    const char *group,
    const char *key,
    char **value)
{
    GError *error = NULL;

    *value =
        g_key_file_get_string(
            keyfile,
            group,
            key,
            &error
        );

    if (!*value) {
        fprintf(
            stderr,
            "Layout cache missing %s/%s: %s\n",
            group,
            key,
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        return -1;
    }

    return 0;
}

static int
apply_cached_layout_once(
    MonitorLayoutCache *cache)
{
    GVariant *state =
        display_config_get_current_state(
            cache->bus
        );

    if (!state)
        return -1;

    guint32 serial = 0;
    GVariant *physical = NULL;
    GVariant *logical = NULL;
    GVariant *properties = NULL;

    g_variant_get(
        state,
        "(u@a((ssss)a(siiddada{sv})a{sv})"
        "@a(iiduba(ssss)a{sv})"
        "@a{sv})",
        &serial,
        &physical,
        &logical,
        &properties
    );

    g_variant_unref(state);

    CurrentMonitorList current = {0};

    /*
     * Build a synthetic state wrapper only to reuse the parser without
     * duplicating the physical-monitor traversal.
     */
    GVariant *physical_copy =
        g_variant_ref(physical);

    GVariant *logical_copy =
        g_variant_ref(logical);

    GVariant *properties_copy =
        g_variant_ref(properties);

    GVariant *synthetic =
        g_variant_new(
            "(u@a((ssss)a(siiddada{sv})a{sv})"
            "@a(iiduba(ssss)a{sv})"
            "@a{sv})",
            serial,
            physical_copy,
            logical_copy,
            properties_copy
        );

    g_variant_ref_sink(synthetic);

    if (
        current_monitors_from_state(
            synthetic,
            &current
        ) < 0
    ) {
        g_variant_unref(synthetic);
        g_variant_unref(physical);
        g_variant_unref(logical);
        g_variant_unref(properties);
        return -1;
    }

    g_variant_unref(synthetic);

    GKeyFile *keyfile =
        g_key_file_new();

    GError *error = NULL;

    if (
        !g_key_file_load_from_file(
            keyfile,
            cache->cache_path,
            G_KEY_FILE_NONE,
            &error
        )
    ) {
        fprintf(
            stderr,
            "Cannot read layout cache %s: %s\n",
            cache->cache_path,
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        g_key_file_unref(keyfile);
        current_monitor_list_clear(
            &current
        );
        g_variant_unref(physical);
        g_variant_unref(logical);
        g_variant_unref(properties);
        return -1;
    }

    int logical_count =
        g_key_file_get_integer(
            keyfile,
            "layout",
            "logical-count",
            &error
        );

    if (error || logical_count <= 0) {
        fprintf(
            stderr,
            "Invalid cached logical monitor count: %s\n",
            error
                ? error->message
                : "zero"
        );

        g_clear_error(&error);
        g_key_file_unref(keyfile);
        current_monitor_list_clear(
            &current
        );
        g_variant_unref(physical);
        g_variant_unref(logical);
        g_variant_unref(properties);
        return -1;
    }

    GVariantBuilder logical_builder;
    g_variant_builder_init(
        &logical_builder,
        G_VARIANT_TYPE(
            "a(iiduba(ssa{sv}))"
        )
    );

    for (
        int logical_index = 0;
        logical_index < logical_count;
        logical_index++
    ) {
        char *group =
            g_strdup_printf(
                "logical.%d",
                logical_index
            );

        GError *group_error = NULL;

        gint32 x =
            g_key_file_get_integer(
                keyfile,
                group,
                "x",
                &group_error
            );

        gint32 y =
            g_key_file_get_integer(
                keyfile,
                group,
                "y",
                &group_error
            );

        gdouble scale =
            g_key_file_get_double(
                keyfile,
                group,
                "scale",
                &group_error
            );

        guint32 transform =
            (guint32)
            g_key_file_get_uint64(
                keyfile,
                group,
                "transform",
                &group_error
            );

        gboolean primary =
            g_key_file_get_boolean(
                keyfile,
                group,
                "primary",
                &group_error
            );

        int monitor_count =
            g_key_file_get_integer(
                keyfile,
                group,
                "monitor-count",
                &group_error
            );

        if (
            group_error ||
            monitor_count <= 0
        ) {
            fprintf(
                stderr,
                "Invalid cached group %s: %s\n",
                group,
                group_error
                    ? group_error->message
                    : "no monitors"
            );

            g_clear_error(
                &group_error
            );

            g_free(group);
            g_key_file_unref(keyfile);
            current_monitor_list_clear(
                &current
            );
            g_variant_unref(physical);
            g_variant_unref(logical);
            g_variant_unref(properties);
            return -1;
        }

        GVariantBuilder assignments;
        g_variant_builder_init(
            &assignments,
            G_VARIANT_TYPE(
                "a(ssa{sv})"
            )
        );

        for (
            int monitor_index = 0;
            monitor_index < monitor_count;
            monitor_index++
        ) {
            char *monitor_group =
                g_strdup_printf(
                    "logical.%d.monitor.%d",
                    logical_index,
                    monitor_index
                );

            char *saved_connector = NULL;
            char *vendor = NULL;
            char *product = NULL;
            char *saved_serial = NULL;
            char *saved_mode = NULL;

            int ok =
                load_string(
                    keyfile,
                    monitor_group,
                    "connector",
                    &saved_connector
                ) == 0 &&
                load_string(
                    keyfile,
                    monitor_group,
                    "vendor",
                    &vendor
                ) == 0 &&
                load_string(
                    keyfile,
                    monitor_group,
                    "product",
                    &product
                ) == 0 &&
                load_string(
                    keyfile,
                    monitor_group,
                    "serial",
                    &saved_serial
                ) == 0 &&
                load_string(
                    keyfile,
                    monitor_group,
                    "mode",
                    &saved_mode
                ) == 0;

            CurrentMonitor *matched =
                ok
                    ? find_current_monitor(
                        &current,
                        saved_connector,
                        vendor,
                        product,
                        saved_serial
                    )
                    : NULL;

            if (
                !matched ||
                !matched->current_mode
            ) {
                fprintf(
                    stderr,
                    "Cached monitor not present yet: "
                    "connector=%s vendor=%s product=%s serial=%s\n",
                    saved_connector
                        ? saved_connector
                        : "?",
                    vendor ? vendor : "?",
                    product ? product : "?",
                    saved_serial
                        ? saved_serial
                        : "?"
                );

                g_free(saved_connector);
                g_free(vendor);
                g_free(product);
                g_free(saved_serial);
                g_free(saved_mode);
                g_free(monitor_group);
                g_free(group);
                g_key_file_unref(keyfile);
                current_monitor_list_clear(
                    &current
                );
                g_variant_unref(physical);
                g_variant_unref(logical);
                g_variant_unref(properties);
                return 1;
            }

            /*
             * Mode IDs are only valid for the current monitor state. Prefer
             * the current mode ID supplied by Mutter after hotplug. For the
             * virtual monitor this is the negotiated 1024x768@fps mode.
             */
            GVariantBuilder assignment_props;
            g_variant_builder_init(
                &assignment_props,
                G_VARIANT_TYPE("a{sv}")
            );

            g_variant_builder_add(
                &assignments,
                "(ss@a{sv})",
                matched->connector,
                matched->current_mode,
                g_variant_builder_end(
                    &assignment_props
                )
            );

            g_free(saved_connector);
            g_free(vendor);
            g_free(product);
            g_free(saved_serial);
            g_free(saved_mode);
            g_free(monitor_group);
        }

        g_variant_builder_add(
            &logical_builder,
            "(iidub@a(ssa{sv}))",
            x,
            y,
            scale,
            transform,
            primary,
            g_variant_builder_end(
                &assignments
            )
        );

        g_free(group);
    }

    GVariantBuilder apply_properties;
    g_variant_builder_init(
        &apply_properties,
        G_VARIANT_TYPE("a{sv}")
    );

    /*
     * method=1 is temporary and applies immediately without GNOME's
     * confirmation dialog. Persistence is handled by our own layout cache,
     * so Mutter does not need to persist this transient virtual-monitor
     * configuration.
     */
    GVariant *parameters =
        g_variant_new(
            "(uu@a(iiduba(ssa{sv}))@a{sv})",
            serial,
            1u,
            g_variant_builder_end(
                &logical_builder
            ),
            g_variant_builder_end(
                &apply_properties
            )
        );

    GError *apply_error = NULL;

    GVariant *reply =
        g_dbus_connection_call_sync(
            cache->bus,
            DISPLAY_CONFIG_BUS,
            DISPLAY_CONFIG_PATH,
            DISPLAY_CONFIG_IFACE,
            "ApplyMonitorsConfig",
            parameters,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &apply_error
        );

    if (!reply) {
        fprintf(
            stderr,
            "DisplayConfig.ApplyMonitorsConfig failed: %s\n",
            apply_error
                ? apply_error->message
                : "unknown error"
        );

        g_clear_error(&apply_error);
        g_key_file_unref(keyfile);
        current_monitor_list_clear(
            &current
        );
        g_variant_unref(physical);
        g_variant_unref(logical);
        g_variant_unref(properties);
        return -1;
    }

    g_variant_unref(reply);
    g_key_file_unref(keyfile);
    current_monitor_list_clear(
        &current
    );
    g_variant_unref(physical);
    g_variant_unref(logical);
    g_variant_unref(properties);

    printf(
        "Applied cached Mutter logical monitor layout temporarily through DisplayConfig\n"
    );

    return 0;
}

int
monitor_layout_cache_prepare(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg)
{
    if (!cache || !cfg)
        return -1;

    memset(cache, 0, sizeof(*cache));

    if (!cfg->layout_remember)
        return 0;

    GError *error = NULL;

    cache->bus =
        g_bus_get_sync(
            G_BUS_TYPE_SESSION,
            NULL,
            &error
        );

    if (!cache->bus) {
        fprintf(
            stderr,
            "Cannot connect layout cache to session D-Bus: %s\n",
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        return -1;
    }

    const char *config_dir =
        g_get_user_config_dir();

    char *own_dir =
        g_build_filename(
            config_dir,
            "vnc-monitor-server",
            NULL
        );

    if (
        g_mkdir_with_parents(
            own_dir,
            0700
        ) < 0
    ) {
        perror(
            "create layout cache directory"
        );

        g_free(own_dir);
        monitor_layout_cache_clear(
            cache
        );
        return -1;
    }

    char *cache_name =
        g_strdup_printf(
            "layout-v2-%dx%d.ini",
            cfg->width,
            cfg->height
        );

    cache->cache_path =
        g_build_filename(
            own_dir,
            cache_name,
            NULL
        );

    g_free(cache_name);
    g_free(own_dir);

    cache->cache_existed =
        g_file_test(
            cache->cache_path,
            G_FILE_TEST_IS_REGULAR
        );

    if (cache->cache_existed) {
        printf(
            "Cached Mutter layout available: %s\n",
            cache->cache_path
        );
    }
    else {
        printf(
            "No cached Mutter layout yet; arrange displays once, "
            "then disconnect to save the live DisplayConfig state\n"
        );
    }

    cache->prepared = 1;
    return 0;
}

int
monitor_layout_cache_apply(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg,
    int timeout_ms)
{
    if (
        !cache ||
        !cfg ||
        !cfg->layout_remember ||
        !cache->prepared ||
        !cache->cache_existed
    )
        return 0;

    const gint64 deadline =
        g_get_monotonic_time() +
        (gint64)timeout_ms *
        1000;

    for (;;) {
        int rc =
            apply_cached_layout_once(
                cache
            );

        if (rc == 0)
            return 0;

        if (rc < 0)
            return -1;

        if (
            g_get_monotonic_time() >=
            deadline
        ) {
            fprintf(
                stderr,
                "Timed out waiting for all cached monitors "
                "to appear in Mutter DisplayConfig\n"
            );

            return -1;
        }

        g_usleep(100000);
    }
}

int
monitor_layout_cache_save(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg)
{
    if (
        !cache ||
        !cfg ||
        !cfg->layout_remember ||
        !cache->prepared
    )
        return 0;

    if (
        cache->cache_existed &&
        !cfg->layout_resave
    )
        return 0;

    GVariant *state =
        display_config_get_current_state(
            cache->bus
        );

    if (!state)
        return -1;

    GKeyFile *keyfile =
        g_key_file_new();

    int rc =
        keyfile_save_state(
            keyfile,
            state
        );

    g_variant_unref(state);

    if (rc < 0) {
        fprintf(
            stderr,
            "Current Mutter layout could not be serialized\n"
        );

        g_key_file_unref(keyfile);
        return -1;
    }

    GError *error = NULL;

    if (
        !g_key_file_save_to_file(
            keyfile,
            cache->cache_path,
            &error
        )
    ) {
        fprintf(
            stderr,
            "Cannot save Mutter layout cache %s: %s\n",
            cache->cache_path,
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        g_key_file_unref(keyfile);
        return -1;
    }

    g_key_file_unref(keyfile);

    printf(
        "%s live Mutter logical monitor layout: %s\n",
        cache->cache_existed
            ? "Updated"
            : "Saved",
        cache->cache_path
    );

    cache->cache_existed = 1;
    return 0;
}

int
monitor_layout_log_matching_modes(
    MonitorLayoutCache *cache,
    const RuntimeConfig *cfg)
{
    if (!cache || !cfg || !cache->bus)
        return -1;

    GVariant *state =
        display_config_get_current_state(
            cache->bus
        );

    if (!state)
        return -1;

    CurrentMonitorList monitors = {0};

    if (
        current_monitors_from_state(
            state,
            &monitors
        ) < 0
    ) {
        g_variant_unref(state);
        return -1;
    }

    g_variant_unref(state);

    int matches = 0;

    for (size_t i = 0; i < monitors.count; i++) {
        CurrentMonitor *monitor =
            &monitors.items[i];

        if (
            monitor->current_width != cfg->width ||
            monitor->current_height != cfg->height
        )
            continue;

        fprintf(
            stderr,
            "[MUTTER-MODE] connector=%s vendor=%s product=%s "
            "serial=%s mode=%s size=%dx%d refresh=%.3fHz\n",
            monitor->connector ? monitor->connector : "(unknown)",
            monitor->vendor ? monitor->vendor : "(unknown)",
            monitor->product ? monitor->product : "(unknown)",
            monitor->serial ? monitor->serial : "(unknown)",
            monitor->current_mode ? monitor->current_mode : "(unknown)",
            monitor->current_width,
            monitor->current_height,
            monitor->current_refresh
        );

        matches++;
    }

    if (matches == 0) {
        fprintf(
            stderr,
            "[MUTTER-MODE] no current %dx%d mode found in DisplayConfig\n",
            cfg->width,
            cfg->height
        );
    }

    current_monitor_list_clear(&monitors);
    return matches > 0 ? 0 : -1;
}

void
monitor_layout_cache_clear(
    MonitorLayoutCache *cache)
{
    if (!cache)
        return;

    g_clear_object(&cache->bus);
    g_clear_pointer(
        &cache->cache_path,
        g_free
    );

    memset(cache, 0, sizeof(*cache));
}
