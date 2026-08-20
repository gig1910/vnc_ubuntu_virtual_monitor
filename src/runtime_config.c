#define _GNU_SOURCE

#include "runtime_config.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int
copy_text(const char *name, char *out, size_t out_size, const char *value)
{
    if (!value || !*value) {
        fprintf(stderr, "Invalid %s: empty value\n", name);
        return -1;
    }

    int n = snprintf(out, out_size, "%s", value);
    if (n < 0 || (size_t)n >= out_size) {
        fprintf(stderr, "Invalid %s: value is too long\n", name);
        return -1;
    }

    return 0;
}

static int
expand_path(const char *name, char *out, size_t out_size, const char *value)
{
    const char *home = getenv("HOME");

    if (value && home && *home && value[0] == '~' && value[1] == '/') {
        int n = snprintf(out, out_size, "%s/%s", home, value + 2);
        if (n < 0 || (size_t)n >= out_size) {
            fprintf(stderr, "Invalid %s: expanded path is too long\n", name);
            return -1;
        }
        return 0;
    }

    if (value && home && *home && strncmp(value, "$HOME/", 6) == 0) {
        int n = snprintf(out, out_size, "%s/%s", home, value + 6);
        if (n < 0 || (size_t)n >= out_size) {
            fprintf(stderr, "Invalid %s: expanded path is too long\n", name);
            return -1;
        }
        return 0;
    }

    return copy_text(name, out, out_size, value);
}

static int
parse_int(const char *name, const char *value, int min_value, int max_value, int *out)
{
    errno = 0;
    char *end = NULL;
    long v = strtol(value, &end, 10);

    if (!value || !*value || errno != 0 || !end || *end != '\0' ||
        v < min_value || v > max_value) {
        fprintf(stderr,
                "Invalid %s: %s (expected %d..%d)\n",
                name,
                value ? value : "(null)",
                min_value,
                max_value);
        return -1;
    }

    *out = (int)v;
    return 0;
}

static int
parse_bool(const char *name, const char *value, int *out)
{
    if (value &&
        (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
         strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0)) {
        *out = 1;
        return 0;
    }

    if (value &&
        (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
         strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0)) {
        *out = 0;
        return 0;
    }

    fprintf(stderr,
            "Invalid %s: %s (use on/off, true/false or 1/0)\n",
            name,
            value ? value : "(null)");
    return -1;
}

const char *
runtime_config_capture_backend_name(CaptureBackend backend)
{
    return backend == CAPTURE_BACKEND_GSTREAMER ? "gstreamer" : "pipewire";
}

static int
parse_capture_backend(const char *value, CaptureBackend *out)
{
    if (value &&
        (strcasecmp(value, "pipewire") == 0 || strcasecmp(value, "native") == 0)) {
        *out = CAPTURE_BACKEND_PIPEWIRE;
        return 0;
    }

    if (value &&
        (strcasecmp(value, "gstreamer") == 0 || strcasecmp(value, "gst") == 0)) {
        *out = CAPTURE_BACKEND_GSTREAMER;
        return 0;
    }

    fprintf(stderr,
            "Invalid capture backend: %s (use pipewire or gstreamer)\n",
            value ? value : "(null)");
    return -1;
}

const char *
runtime_config_mutter_cursor_name(MutterCursorMode mode)
{
    switch (mode) {
        case MUTTER_CURSOR_HIDDEN:
            return "hidden";
        case MUTTER_CURSOR_EMBEDDED:
            return "embedded";
        case MUTTER_CURSOR_METADATA:
            return "metadata";
        default:
            return "unknown";
    }
}

static int
parse_mutter_cursor(const char *value, MutterCursorMode *out)
{
    if (value && strcmp(value, "hidden") == 0) {
        *out = MUTTER_CURSOR_HIDDEN;
        return 0;
    }

    if (value && strcmp(value, "embedded") == 0) {
        *out = MUTTER_CURSOR_EMBEDDED;
        return 0;
    }

    if (value && strcmp(value, "metadata") == 0) {
        *out = MUTTER_CURSOR_METADATA;
        return 0;
    }

    fprintf(stderr,
            "Invalid mutter-cursor value: %s (expected hidden|embedded|metadata)\n",
            value ? value : "(null)");
    return -1;
}

const char *
runtime_config_screen_size_mode_name(ScreenSizeMode mode)
{
    return mode == SCREEN_SIZE_FIXED ? "fixed" : "auto";
}

static int
parse_screen_size_mode(const char *value, ScreenSizeMode *out)
{
    if (value &&
        (strcasecmp(value, "auto") == 0 || strcasecmp(value, "client") == 0)) {
        *out = SCREEN_SIZE_AUTO;
        return 0;
    }

    if (value && strcasecmp(value, "fixed") == 0) {
        *out = SCREEN_SIZE_FIXED;
        return 0;
    }

    fprintf(stderr,
            "Invalid screen-mode: %s (use auto or fixed)\n",
            value ? value : "(null)");
    return -1;
}

static int
parse_vnc_fps(const char *value, int *out)
{
    if (value &&
        (strcasecmp(value, "source") == 0 || strcasecmp(value, "unlimited") == 0)) {
        *out = 0;
        return 0;
    }

    return parse_int("vnc-fps", value, 0, 240, out);
}

static void
apply_log_level(RuntimeConfig *cfg)
{
    cfg->frame_stats = cfg->verbose >= VNC_LOG_DEBUG;
    cfg->frame_trace = cfg->verbose >= VNC_LOG_TRACE;
    cfg->latency_trace = cfg->verbose >= VNC_LOG_TRACE;
    cfg->capture_trace = cfg->verbose >= VNC_LOG_TRACE;
    cfg->capture_stall_ms = 500;
    cfg->frame_stats_interval_ms = 1000;
}

static int
apply_setting(RuntimeConfig *cfg, const char *name, const char *value)
{
    if (strcmp(name, "capture-backend") == 0)
        return parse_capture_backend(value, &cfg->capture_backend);
    if (strcmp(name, "capture-timeout-ms") == 0)
        return parse_int(name, value, 100, 60000, &cfg->capture_timeout_ms);
    if (strcmp(name, "mutter-cursor") == 0)
        return parse_mutter_cursor(value, &cfg->mutter_cursor_mode);
    if (strcmp(name, "vnc-fps") == 0)
        return parse_vnc_fps(value, &cfg->vnc_max_fps);
    if (strcmp(name, "latest-only") == 0)
        return parse_bool(name, value, &cfg->latest_only);
    if (strcmp(name, "external-send-buffer") == 0)
        return parse_int(name, value, 4096, 4194304, &cfg->external_send_buffer);
    if (strcmp(name, "backend-recv-buffer") == 0)
        return parse_int(name, value, 4096, 4194304, &cfg->backend_receive_buffer);
    if (strcmp(name, "client-keepalive-idle") == 0)
        return parse_int(name, value, 1, 3600, &cfg->client_keepalive_idle_s);
    if (strcmp(name, "client-keepalive-interval") == 0)
        return parse_int(name, value, 1, 3600, &cfg->client_keepalive_interval_s);
    if (strcmp(name, "client-keepalive-probes") == 0)
        return parse_int(name, value, 1, 20, &cfg->client_keepalive_probes);
    if (strcmp(name, "client-user-timeout-ms") == 0)
        return parse_int(name, value, 1000, 600000, &cfg->client_user_timeout_ms);
    if (strcmp(name, "diff-detect") == 0)
        return parse_bool(name, value, &cfg->diff_detect);
    if (strcmp(name, "diff-tile-size") == 0)
        return parse_int(name, value, 8, 256, &cfg->diff_tile_size);
    if (strcmp(name, "layout-remember") == 0)
        return parse_bool(name, value, &cfg->layout_remember);
    if (strcmp(name, "layout-resave") == 0)
        return parse_bool(name, value, &cfg->layout_resave);
    if (strcmp(name, "port") == 0)
        return parse_int(name, value, 1, 65535, &cfg->public_port);
    if (strcmp(name, "backend-port") == 0)
        return parse_int(name, value, 1, 65535, &cfg->backend_port);
    if (strcmp(name, "backend-bind") == 0)
        return copy_text(name, cfg->backend_bind, sizeof(cfg->backend_bind), value);
    if (strcmp(name, "screen-mode") == 0)
        return parse_screen_size_mode(value, &cfg->screen_size_mode);
    if (strcmp(name, "width") == 0)
        return parse_int(name, value, 64, 16384, &cfg->width);
    if (strcmp(name, "height") == 0)
        return parse_int(name, value, 64, 16384, &cfg->height);
    if (strcmp(name, "fps") == 0)
        return parse_int(name, value, 1, 240, &cfg->max_fps);
    if (strcmp(name, "ra2-record-size") == 0)
        return parse_int(name, value, 1, 65535, &cfg->ra2_stream_record_max);
    if (strcmp(name, "ra2-coalesce") == 0)
        return parse_bool(name, value, &cfg->ra2_coalesce);
    if (strcmp(name, "ra2-coalesce-us") == 0)
        return parse_int(name, value, 0, 1000000, &cfg->ra2_coalesce_us);
    if (strcmp(name, "ra2-key") == 0)
        return expand_path(name, cfg->ra2_key_file, sizeof(cfg->ra2_key_file), value);
    if (strcmp(name, "auth-socket") == 0)
        return expand_path(name, cfg->auth_socket, sizeof(cfg->auth_socket), value);
    if (strcmp(name, "zrle") == 0)
        return parse_bool(name, value, &cfg->enable_zrle);
    if (strcmp(name, "raw") == 0)
        return parse_bool(name, value, &cfg->enable_raw);
    if (strcmp(name, "cursor") == 0)
        return parse_bool(name, value, &cfg->enable_cursor);
    if (strcmp(name, "newfbsize") == 0)
        return parse_bool(name, value, &cfg->enable_newfbsize);
    if (strcmp(name, "verbose") == 0) {
        if (vnc_log_parse_level(value, &cfg->verbose) < 0) {
            fprintf(stderr,
                    "Invalid verbose level: %s (use 0|error, 1|info, 2|debug, 3|trace)\n",
                    value ? value : "(null)");
            return -1;
        }
        return 0;
    }

    fprintf(stderr, "Unknown configuration setting: %s\n", name);
    return -1;
}

typedef struct {
    const char *group;
    const char *key;
    const char *setting;
} ConfigBinding;

static const ConfigBinding config_bindings[] = {
    {"capture", "backend", "capture-backend"},
    {"capture", "timeout-ms", "capture-timeout-ms"},
    {"capture", "cursor", "mutter-cursor"},
    {"capture", "fps", "fps"},

    {"display", "mode", "screen-mode"},
    {"display", "width", "width"},
    {"display", "height", "height"},
    {"display", "layout-remember", "layout-remember"},
    {"display", "layout-resave", "layout-resave"},

    {"transport", "vnc-fps", "vnc-fps"},
    {"transport", "latest-only", "latest-only"},
    {"transport", "diff-detect", "diff-detect"},
    {"transport", "diff-tile-size", "diff-tile-size"},

    {"network", "port", "port"},
    {"network", "backend-port", "backend-port"},
    {"network", "backend-bind", "backend-bind"},
    {"network", "external-send-buffer", "external-send-buffer"},
    {"network", "backend-recv-buffer", "backend-recv-buffer"},
    {"network", "client-keepalive-idle", "client-keepalive-idle"},
    {"network", "client-keepalive-interval", "client-keepalive-interval"},
    {"network", "client-keepalive-probes", "client-keepalive-probes"},
    {"network", "client-user-timeout-ms", "client-user-timeout-ms"},

    {"ra2", "record-size", "ra2-record-size"},
    {"ra2", "coalesce", "ra2-coalesce"},
    {"ra2", "coalesce-us", "ra2-coalesce-us"},
    {"ra2", "key", "ra2-key"},
    {"ra2", "auth-socket", "auth-socket"},

    {"rfb", "zrle", "zrle"},
    {"rfb", "raw", "raw"},
    {"rfb", "cursor", "cursor"},
    {"rfb", "newfbsize", "newfbsize"},

    {"logging", "level", "verbose"}
};

static const ConfigBinding *
find_config_binding(const char *group, const char *key)
{
    for (size_t i = 0; i < sizeof(config_bindings) / sizeof(config_bindings[0]); i++) {
        if (strcmp(config_bindings[i].group, group) == 0 &&
            strcmp(config_bindings[i].key, key) == 0)
            return &config_bindings[i];
    }

    return NULL;
}

static int
load_config_file(RuntimeConfig *cfg, int explicit_config)
{
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile,
                                   cfg->config_file,
                                   G_KEY_FILE_NONE,
                                   &error)) {
        if (!explicit_config &&
            error && error->domain == G_FILE_ERROR && error->code == G_FILE_ERROR_NOENT) {
            g_clear_error(&error);
            g_key_file_free(keyfile);
            return 0;
        }

        fprintf(stderr,
                "Cannot load config %s: %s\n",
                cfg->config_file,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        g_key_file_free(keyfile);
        return -1;
    }

    gsize group_count = 0;
    gchar **groups = g_key_file_get_groups(keyfile, &group_count);

    for (gsize gi = 0; gi < group_count; gi++) {
        gsize key_count = 0;
        GError *keys_error = NULL;
        gchar **keys = g_key_file_get_keys(keyfile, groups[gi], &key_count, &keys_error);

        if (!keys) {
            fprintf(stderr,
                    "Cannot read config group [%s]: %s\n",
                    groups[gi],
                    keys_error ? keys_error->message : "unknown error");
            g_clear_error(&keys_error);
            g_strfreev(groups);
            g_key_file_free(keyfile);
            return -1;
        }

        for (gsize ki = 0; ki < key_count; ki++) {
            const ConfigBinding *binding = find_config_binding(groups[gi], keys[ki]);
            if (!binding) {
                fprintf(stderr,
                        "Unknown config key %s/%s in %s\n",
                        groups[gi],
                        keys[ki],
                        cfg->config_file);
                g_strfreev(keys);
                g_strfreev(groups);
                g_key_file_free(keyfile);
                return -1;
            }

            GError *value_error = NULL;
            gchar *value = g_key_file_get_string(keyfile,
                                                 groups[gi],
                                                 keys[ki],
                                                 &value_error);
            if (!value) {
                fprintf(stderr,
                        "Cannot read config value %s/%s: %s\n",
                        groups[gi],
                        keys[ki],
                        value_error ? value_error->message : "unknown error");
                g_clear_error(&value_error);
                g_strfreev(keys);
                g_strfreev(groups);
                g_key_file_free(keyfile);
                return -1;
            }

            g_strstrip(value);
            if (apply_setting(cfg, binding->setting, value) < 0) {
                fprintf(stderr,
                        "While reading %s [%s] %s\n",
                        cfg->config_file,
                        groups[gi],
                        keys[ki]);
                g_free(value);
                g_strfreev(keys);
                g_strfreev(groups);
                g_key_file_free(keyfile);
                return -1;
            }
            g_free(value);
        }

        g_strfreev(keys);
    }

    g_strfreev(groups);
    g_key_file_free(keyfile);
    return 0;
}

static int
prescan_config_path(RuntimeConfig *cfg, int argc, char **argv, int *explicit_config)
{
    *explicit_config = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --config\n");
                return -1;
            }
            if (expand_path("config", cfg->config_file, sizeof(cfg->config_file), argv[++i]) < 0)
                return -1;
            *explicit_config = 1;
        }
        else if (strncmp(argv[i], "--config=", 9) == 0) {
            if (expand_path("config", cfg->config_file, sizeof(cfg->config_file), argv[i] + 9) < 0)
                return -1;
            *explicit_config = 1;
        }
    }

    return 0;
}

void
runtime_config_defaults(RuntimeConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const char *home = getenv("HOME");
    const char *config_home = getenv("XDG_CONFIG_HOME");

    if (config_home && *config_home) {
        snprintf(cfg->config_file,
                 sizeof(cfg->config_file),
                 "%s/vnc-monitor/config.ini",
                 config_home);
        snprintf(cfg->ra2_key_file,
                 sizeof(cfg->ra2_key_file),
                 "%s/vnc-monitor/ra2-server-key.pem",
                 config_home);
    }
    else if (home && *home) {
        snprintf(cfg->config_file,
                 sizeof(cfg->config_file),
                 "%s/.config/vnc-monitor/config.ini",
                 home);
        snprintf(cfg->ra2_key_file,
                 sizeof(cfg->ra2_key_file),
                 "%s/.config/vnc-monitor/ra2-server-key.pem",
                 home);
    }
    else {
        snprintf(cfg->config_file, sizeof(cfg->config_file), "./vnc-monitor.ini");
        snprintf(cfg->ra2_key_file, sizeof(cfg->ra2_key_file), "./ra2-server-key.pem");
    }

    snprintf(cfg->auth_socket,
             sizeof(cfg->auth_socket),
             "/run/vnc-monitor-auth.sock");
    snprintf(cfg->backend_bind,
             sizeof(cfg->backend_bind),
             "127.0.0.1");

    cfg->capture_backend = CAPTURE_BACKEND_PIPEWIRE;
    cfg->capture_timeout_ms = 5000;
    cfg->mutter_cursor_mode = MUTTER_CURSOR_METADATA;
    cfg->vnc_max_fps = 0;
    cfg->latest_only = 1;

    cfg->external_send_buffer = 65536;
    cfg->backend_receive_buffer = 65536;
    cfg->client_keepalive_idle_s = 15;
    cfg->client_keepalive_interval_s = 5;
    cfg->client_keepalive_probes = 3;
    cfg->client_user_timeout_ms = 20000;
    cfg->diff_detect = 1;
    cfg->diff_tile_size = 32;
    cfg->layout_remember = 1;
    cfg->layout_resave = 0;

    cfg->public_port = 5901;
    cfg->backend_port = 5903;
    cfg->screen_size_mode = SCREEN_SIZE_AUTO;
    cfg->width = 1024;
    cfg->height = 768;
    cfg->max_fps = 60;
    cfg->ra2_stream_record_max = 16384;
    cfg->ra2_coalesce = 1;
    cfg->ra2_coalesce_us = 500;

    cfg->enable_zrle = 1;
    cfg->enable_raw = 1;
    cfg->enable_cursor = 1;
    cfg->enable_newfbsize = 1;

    cfg->enable_clipboard = 0;
    cfg->enable_file_transfer = 0;
    cfg->view_only = 1;
    cfg->verbose = VNC_LOG_INFO;

    apply_log_level(cfg);
}

void
runtime_config_usage(const char *argv0)
{
    printf(
        "VNC Monitor %s\n"
        "Usage: %s [options]\n"
        "\n"
        "Configuration:\n"
        "  --config FILE                INI config [~/.config/vnc-monitor/config.ini]\n"
        "  Config is loaded after built-in defaults; CLI options override it.\n"
        "\n"
        "Wayland capture:\n"
        "  --capture-backend MODE       pipewire|gstreamer\n"
        "  --capture-timeout-ms N       Wait for first frame\n"
        "  --mutter-cursor MODE         hidden|embedded|metadata\n"
        "  --vnc-fps source|N           Max publish rate\n"
        "  --latest-only on|off         Keep newest source state\n"
        "  --diff-detect on|off         Tile diff detection\n"
        "  --diff-tile-size N           Diff tile size\n"
        "  --layout-remember on|off     Restore saved GNOME layout\n"
        "  --layout-resave on|off       Replace saved layout\n"
        "\n"
        "Network:\n"
        "  --port N                     Public RA2r VNC port\n"
        "  --backend-port N             Internal RFB port\n"
        "  --backend-bind ADDR          Internal bind address\n"
        "  --external-send-buffer N     External TCP SO_SNDBUF\n"
        "  --backend-recv-buffer N      Backend TCP SO_RCVBUF\n"
        "  --client-keepalive-idle N    Seconds before TCP keepalive probes [15]\n"
        "  --client-keepalive-interval N Seconds between keepalive probes [5]\n"
        "  --client-keepalive-probes N  Failed probes before dead peer [3]\n"
        "  --client-user-timeout-ms N   Max unacknowledged TCP time [20000]\n"
        "\n"
        "Virtual monitor:\n"
        "  --screen-mode MODE           auto|fixed [auto]\n"
        "  --width N                    Initial/fallback width [1024]\n"
        "  --height N                   Initial/fallback height [768]\n"
        "  --fps N                      Maximum capture FPS [60]\n"
        "\n"
        "RA2r:\n"
        "  --ra2-record-size N          Max encrypted payload\n"
        "  --ra2-coalesce on|off        Merge backend reads\n"
        "  --ra2-coalesce-us N          Max merge delay, usec\n"
        "  --ra2-key FILE               Persistent RSA identity\n"
        "  --auth-socket PATH           PAM helper socket\n"
        "\n"
        "RFB compatibility:\n"
        "  --zrle on|off                Advertise/allow ZRLE\n"
        "  --raw on|off                 Advertise/allow Raw\n"
        "  --cursor on|off              Cursor extension\n"
        "  --newfbsize on|off           NewFBSize extension\n"
        "\n"
        "Logging:\n"
        "  --verbose LEVEL              0|error, 1|info, 2|debug, 3|trace\n"
        "\n"
        "Connection policy is fixed in beta: strong single-connect; additional\n"
        "clients are rejected immediately while one session owns the display.\n"
        "Security is fixed in beta: view-only, clipboard input disabled,\n"
        "file transfer disabled. These are not runtime switches.\n"
        "\n"
        "Other:\n"
        "  --show-config                Print effective config and exit\n"
        "  --version                    Print version and exit\n"
        "  -h, --help                   Show this help\n",
        VNC_MONITOR_VERSION,
        argv0);
}

void
runtime_config_print(const RuntimeConfig *cfg)
{
    printf(
        "VNC Monitor %s configuration:\n"
        "  config file:       %s\n"
        "  capture backend:   %s\n"
        "  capture timeout:   %d ms\n"
        "  Mutter cursor:     %s\n"
        "  capture max FPS:   %d\n"
        "  VNC max rate:      %s\n"
        "  latest-only:       %s\n"
        "  diff detect:       %s (%d px tiles)\n"
        "  layout remember:   %s\n"
        "  layout resave:     %s\n"
        "  public port:       %d\n"
        "  backend:           %s:%d\n"
        "  connection policy: strong single-connect\n"
        "  TCP keepalive:     idle=%ds interval=%ds probes=%d\n"
        "  TCP user timeout:  %d ms\n"
        "  screen mode:       %s\n"
        "  framebuffer:       %dx%d (%s)\n"
        "  RA2 record max:    %d bytes\n"
        "  RA2 coalesce:      %s (%d us)\n"
        "  RA2 key:           %s\n"
        "  auth socket:       %s\n"
        "  RFB ZRLE/raw:      %s/%s\n"
        "  cursor/NewFBSize:  %s/%s\n"
        "  security:          view-only, clipboard=off, file-transfer=off\n"
        "  log level:         %s (%d)\n",
        VNC_MONITOR_VERSION,
        cfg->config_file,
        runtime_config_capture_backend_name(cfg->capture_backend),
        cfg->capture_timeout_ms,
        runtime_config_mutter_cursor_name(cfg->mutter_cursor_mode),
        cfg->max_fps,
        cfg->vnc_max_fps > 0 ? "rate-limited" : "source-driven",
        cfg->latest_only ? "on" : "off",
        cfg->diff_detect ? "on" : "off",
        cfg->diff_tile_size,
        cfg->layout_remember ? "on" : "off",
        cfg->layout_resave ? "on" : "off",
        cfg->public_port,
        cfg->backend_bind,
        cfg->backend_port,
        cfg->client_keepalive_idle_s,
        cfg->client_keepalive_interval_s,
        cfg->client_keepalive_probes,
        cfg->client_user_timeout_ms,
        runtime_config_screen_size_mode_name(cfg->screen_size_mode),
        cfg->width,
        cfg->height,
        cfg->screen_size_mode == SCREEN_SIZE_FIXED ? "fixed" : "initial/fallback",
        cfg->ra2_stream_record_max,
        cfg->ra2_coalesce ? "on" : "off",
        cfg->ra2_coalesce_us,
        cfg->ra2_key_file,
        cfg->auth_socket,
        cfg->enable_zrle ? "on" : "off",
        cfg->enable_raw ? "on" : "off",
        cfg->enable_cursor ? "on" : "off",
        cfg->enable_newfbsize ? "on" : "off",
        vnc_log_level_name(cfg->verbose),
        cfg->verbose);

    if (cfg->vnc_max_fps > 0)
        printf("  VNC max FPS:       %d\n", cfg->vnc_max_fps);
}

int
runtime_config_parse(RuntimeConfig *cfg, int argc, char **argv)
{
    enum {
        OPT_CONFIG = 1000,
        OPT_CAPTURE_BACKEND,
        OPT_CAPTURE_TIMEOUT_MS,
        OPT_MUTTER_CURSOR,
        OPT_VNC_FPS,
        OPT_LATEST_ONLY,
        OPT_EXTERNAL_SEND_BUFFER,
        OPT_BACKEND_RECV_BUFFER,
        OPT_CLIENT_KEEPALIVE_IDLE,
        OPT_CLIENT_KEEPALIVE_INTERVAL,
        OPT_CLIENT_KEEPALIVE_PROBES,
        OPT_CLIENT_USER_TIMEOUT_MS,
        OPT_DIFF_DETECT,
        OPT_DIFF_TILE_SIZE,
        OPT_LAYOUT_REMEMBER,
        OPT_LAYOUT_RESAVE,
        OPT_PORT,
        OPT_BACKEND_PORT,
        OPT_BACKEND_BIND,
        OPT_SCREEN_MODE,
        OPT_WIDTH,
        OPT_HEIGHT,
        OPT_FPS,
        OPT_RA2_RECORD_SIZE,
        OPT_RA2_COALESCE,
        OPT_RA2_COALESCE_US,
        OPT_RA2_KEY,
        OPT_AUTH_SOCKET,
        OPT_ZRLE,
        OPT_RAW,
        OPT_CURSOR,
        OPT_NEWFBSIZE,
        OPT_VERBOSE,
        OPT_SHOW_CONFIG,
        OPT_VERSION
    };

    static const struct option options[] = {
        {"config", required_argument, NULL, OPT_CONFIG},
        {"capture-backend", required_argument, NULL, OPT_CAPTURE_BACKEND},
        {"capture-timeout-ms", required_argument, NULL, OPT_CAPTURE_TIMEOUT_MS},
        {"mutter-cursor", required_argument, NULL, OPT_MUTTER_CURSOR},
        {"vnc-fps", required_argument, NULL, OPT_VNC_FPS},
        {"latest-only", required_argument, NULL, OPT_LATEST_ONLY},
        {"external-send-buffer", required_argument, NULL, OPT_EXTERNAL_SEND_BUFFER},
        {"backend-recv-buffer", required_argument, NULL, OPT_BACKEND_RECV_BUFFER},
        {"client-keepalive-idle", required_argument, NULL, OPT_CLIENT_KEEPALIVE_IDLE},
        {"client-keepalive-interval", required_argument, NULL, OPT_CLIENT_KEEPALIVE_INTERVAL},
        {"client-keepalive-probes", required_argument, NULL, OPT_CLIENT_KEEPALIVE_PROBES},
        {"client-user-timeout-ms", required_argument, NULL, OPT_CLIENT_USER_TIMEOUT_MS},
        {"diff-detect", required_argument, NULL, OPT_DIFF_DETECT},
        {"diff-tile-size", required_argument, NULL, OPT_DIFF_TILE_SIZE},
        {"layout-remember", required_argument, NULL, OPT_LAYOUT_REMEMBER},
        {"layout-resave", required_argument, NULL, OPT_LAYOUT_RESAVE},
        {"port", required_argument, NULL, OPT_PORT},
        {"backend-port", required_argument, NULL, OPT_BACKEND_PORT},
        {"backend-bind", required_argument, NULL, OPT_BACKEND_BIND},
        {"screen-mode", required_argument, NULL, OPT_SCREEN_MODE},
        {"width", required_argument, NULL, OPT_WIDTH},
        {"height", required_argument, NULL, OPT_HEIGHT},
        {"fps", required_argument, NULL, OPT_FPS},
        {"ra2-record-size", required_argument, NULL, OPT_RA2_RECORD_SIZE},
        {"ra2-coalesce", required_argument, NULL, OPT_RA2_COALESCE},
        {"ra2-coalesce-us", required_argument, NULL, OPT_RA2_COALESCE_US},
        {"ra2-key", required_argument, NULL, OPT_RA2_KEY},
        {"auth-socket", required_argument, NULL, OPT_AUTH_SOCKET},
        {"zrle", required_argument, NULL, OPT_ZRLE},
        {"raw", required_argument, NULL, OPT_RAW},
        {"cursor", required_argument, NULL, OPT_CURSOR},
        {"newfbsize", required_argument, NULL, OPT_NEWFBSIZE},
        {"verbose", required_argument, NULL, OPT_VERBOSE},
        {"show-config", no_argument, NULL, OPT_SHOW_CONFIG},
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VNC_MONITOR_VERSION);
            exit(0);
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            runtime_config_usage(argv[0]);
            exit(0);
        }
    }

    int explicit_config = 0;
    if (prescan_config_path(cfg, argc, argv, &explicit_config) < 0)
        return -1;

    if (load_config_file(cfg, explicit_config) < 0)
        return -1;

    int show_config = 0;
    optind = 1;

    for (;;) {
        int c = getopt_long(argc, argv, "h", options, NULL);
        if (c == -1)
            break;

        const char *setting = NULL;

        switch (c) {
            case OPT_CONFIG:
                /* Already handled before loading the config file. */
                break;
            case OPT_CAPTURE_BACKEND: setting = "capture-backend"; break;
            case OPT_CAPTURE_TIMEOUT_MS: setting = "capture-timeout-ms"; break;
            case OPT_MUTTER_CURSOR: setting = "mutter-cursor"; break;
            case OPT_VNC_FPS: setting = "vnc-fps"; break;
            case OPT_LATEST_ONLY: setting = "latest-only"; break;
            case OPT_EXTERNAL_SEND_BUFFER: setting = "external-send-buffer"; break;
            case OPT_BACKEND_RECV_BUFFER: setting = "backend-recv-buffer"; break;
            case OPT_CLIENT_KEEPALIVE_IDLE: setting = "client-keepalive-idle"; break;
            case OPT_CLIENT_KEEPALIVE_INTERVAL: setting = "client-keepalive-interval"; break;
            case OPT_CLIENT_KEEPALIVE_PROBES: setting = "client-keepalive-probes"; break;
            case OPT_CLIENT_USER_TIMEOUT_MS: setting = "client-user-timeout-ms"; break;
            case OPT_DIFF_DETECT: setting = "diff-detect"; break;
            case OPT_DIFF_TILE_SIZE: setting = "diff-tile-size"; break;
            case OPT_LAYOUT_REMEMBER: setting = "layout-remember"; break;
            case OPT_LAYOUT_RESAVE: setting = "layout-resave"; break;
            case OPT_PORT: setting = "port"; break;
            case OPT_BACKEND_PORT: setting = "backend-port"; break;
            case OPT_BACKEND_BIND: setting = "backend-bind"; break;
            case OPT_SCREEN_MODE: setting = "screen-mode"; break;
            case OPT_WIDTH: setting = "width"; break;
            case OPT_HEIGHT: setting = "height"; break;
            case OPT_FPS: setting = "fps"; break;
            case OPT_RA2_RECORD_SIZE: setting = "ra2-record-size"; break;
            case OPT_RA2_COALESCE: setting = "ra2-coalesce"; break;
            case OPT_RA2_COALESCE_US: setting = "ra2-coalesce-us"; break;
            case OPT_RA2_KEY: setting = "ra2-key"; break;
            case OPT_AUTH_SOCKET: setting = "auth-socket"; break;
            case OPT_ZRLE: setting = "zrle"; break;
            case OPT_RAW: setting = "raw"; break;
            case OPT_CURSOR: setting = "cursor"; break;
            case OPT_NEWFBSIZE: setting = "newfbsize"; break;
            case OPT_VERBOSE: setting = "verbose"; break;
            case OPT_SHOW_CONFIG:
                show_config = 1;
                break;
            case OPT_VERSION:
                printf("%s\n", VNC_MONITOR_VERSION);
                exit(0);
            case 'h':
                runtime_config_usage(argv[0]);
                exit(0);
            default:
                return -1;
        }

        if (setting && apply_setting(cfg, setting, optarg) < 0)
            return -1;
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected positional argument: %s\n", argv[optind]);
        return -1;
    }

    if (cfg->public_port == cfg->backend_port) {
        fprintf(stderr, "Public and backend ports must differ\n");
        return -1;
    }

    apply_log_level(cfg);

    if (show_config) {
        runtime_config_print(cfg);
        exit(0);
    }

    return 0;
}
