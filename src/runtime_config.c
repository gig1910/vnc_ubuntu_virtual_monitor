#define _GNU_SOURCE

#include "runtime_config.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char default_ra2_key[PATH_MAX];

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
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }

    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
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
    if (strcasecmp(value, "pipewire") == 0 || strcasecmp(value, "native") == 0) {
        *out = CAPTURE_BACKEND_PIPEWIRE;
        return 0;
    }

    if (strcasecmp(value, "gstreamer") == 0 || strcasecmp(value, "gst") == 0) {
        *out = CAPTURE_BACKEND_GSTREAMER;
        return 0;
    }

    fprintf(stderr,
            "Invalid capture backend: %s (use pipewire or gstreamer)\n",
            value);
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
    if (strcmp(value, "hidden") == 0) {
        *out = MUTTER_CURSOR_HIDDEN;
        return 0;
    }

    if (strcmp(value, "embedded") == 0) {
        *out = MUTTER_CURSOR_EMBEDDED;
        return 0;
    }

    if (strcmp(value, "metadata") == 0) {
        *out = MUTTER_CURSOR_METADATA;
        return 0;
    }

    fprintf(stderr,
            "Invalid --mutter-cursor value: %s (expected hidden|embedded|metadata)\n",
            value);
    return -1;
}

static int
parse_vnc_fps(const char *value, int *out)
{
    if (strcasecmp(value, "source") == 0 || strcasecmp(value, "unlimited") == 0) {
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

void
runtime_config_defaults(RuntimeConfig *cfg)
{
    const char *home = getenv("HOME");

    if (home && *home) {
        snprintf(default_ra2_key,
                 sizeof(default_ra2_key),
                 "%s/.config/vnc-monitor/ra2-server-key.pem",
                 home);
    }
    else {
        snprintf(default_ra2_key,
                 sizeof(default_ra2_key),
                 "./ra2-server-key.pem");
    }

    *cfg = (RuntimeConfig) {
        .capture_backend = CAPTURE_BACKEND_PIPEWIRE,
        .capture_timeout_ms = 5000,
        .mutter_cursor_mode = MUTTER_CURSOR_METADATA,
        .vnc_max_fps = 0,
        .latest_only = 1,

        .external_send_buffer = 65536,
        .backend_receive_buffer = 65536,
        .diff_detect = 1,
        .diff_tile_size = 32,
        .layout_remember = 1,
        .layout_resave = 0,

        .public_port = 5901,
        .backend_port = 5903,
        .width = 1024,
        .height = 768,
        .max_fps = 60,
        .ra2_stream_record_max = 16384,
        .ra2_coalesce = 1,
        .ra2_coalesce_us = 500,

        .enable_zrle = 1,
        .enable_raw = 1,
        .enable_cursor = 1,
        .enable_newfbsize = 1,

        .enable_clipboard = 0,
        .enable_file_transfer = 0,
        .view_only = 1,

        .verbose = VNC_LOG_INFO,

        .auth_socket = "/run/vnc-monitor-auth.sock",
        .ra2_key_file = default_ra2_key,
        .backend_bind = "127.0.0.1"
    };

    apply_log_level(cfg);
}

void
runtime_config_usage(const char *argv0)
{
    printf(
        "VNC Monitor %s\n"
        "Usage: %s [options]\n"
        "\n"
        "Wayland capture:\n"
        "  --capture-backend MODE       pipewire|gstreamer        [pipewire]\n"
        "  --capture-timeout-ms N       Wait for first frame       [5000]\n"
        "  --mutter-cursor MODE         hidden|embedded|metadata [metadata]\n"
        "  --vnc-fps source|N           Max publish rate          [source]\n"
        "  --latest-only on|off         Keep newest source state      [on]\n"
        "  --diff-detect on|off         Tile diff detection           [on]\n"
        "  --diff-tile-size N           Diff tile size                 [32]\n"
        "  --layout-remember on|off     Restore saved GNOME layout     [on]\n"
        "  --layout-resave on|off       Replace saved layout          [off]\n"
        "\n"
        "Network:\n"
        "  --port N                     Public RA2r VNC port         [5901]\n"
        "  --backend-port N             Internal RFB port            [5903]\n"
        "  --backend-bind ADDR          Internal bind address   [127.0.0.1]\n"
        "  --external-send-buffer N    External TCP SO_SNDBUF      [65536]\n"
        "  --backend-recv-buffer N     Backend TCP SO_RCVBUF       [65536]\n"
        "\n"
        "Virtual monitor:\n"
        "  --width N                    Width                       [1024]\n"
        "  --height N                   Height                       [768]\n"
        "  --fps N                      Maximum capture FPS           [60]\n"
        "\n"
        "RA2r:\n"
        "  --ra2-record-size N          Max encrypted payload      [16384]\n"
        "  --ra2-coalesce on|off        Merge backend reads            [on]\n"
        "  --ra2-coalesce-us N          Max merge delay, usec          [500]\n"
        "  --ra2-key FILE               Persistent RSA identity\n"
        "  --auth-socket PATH           PAM helper socket\n"
        "\n"
        "RFB compatibility:\n"
        "  --zrle on|off                Advertise/allow ZRLE           [on]\n"
        "  --raw on|off                 Advertise/allow Raw            [on]\n"
        "  --cursor on|off              Cursor extension               [on]\n"
        "  --newfbsize on|off           NewFBSize extension            [on]\n"
        "\n"
        "Logging:\n"
        "  --verbose LEVEL              0|error, 1|info, 2|debug, 3|trace [info]\n"
        "\n"
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
        "  capture backend:   %s\n"
        "  capture timeout:   %d ms\n"
        "  Mutter cursor:     %s\n"
        "  source max FPS:    %d\n"
        "  VNC max rate:      %s\n"
        "  latest-only:       %s\n"
        "  diff detect:       %s (%d px tiles)\n"
        "  layout remember:   %s\n"
        "  layout resave:     %s\n"
        "  public port:       %d\n"
        "  backend:           %s:%d\n"
        "  framebuffer:       %dx%d\n"
        "  RA2 record max:    %d bytes\n"
        "  RA2 coalesce:      %s (%d us)\n"
        "  RA2 key:           %s\n"
        "  auth socket:       %s\n"
        "  RFB ZRLE/raw:      %s/%s\n"
        "  cursor/NewFBSize:  %s/%s\n"
        "  security:          view-only, clipboard=off, file-transfer=off\n"
        "  log level:         %s (%d)\n",
        VNC_MONITOR_VERSION,
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
        cfg->width,
        cfg->height,
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
        OPT_CAPTURE_BACKEND = 1000,
        OPT_CAPTURE_TIMEOUT_MS,
        OPT_MUTTER_CURSOR,
        OPT_VNC_FPS,
        OPT_LATEST_ONLY,
        OPT_EXTERNAL_SEND_BUFFER,
        OPT_BACKEND_RECV_BUFFER,
        OPT_DIFF_DETECT,
        OPT_DIFF_TILE_SIZE,
        OPT_LAYOUT_REMEMBER,
        OPT_LAYOUT_RESAVE,
        OPT_PORT,
        OPT_BACKEND_PORT,
        OPT_BACKEND_BIND,
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
        {"capture-backend", required_argument, NULL, OPT_CAPTURE_BACKEND},
        {"capture-timeout-ms", required_argument, NULL, OPT_CAPTURE_TIMEOUT_MS},
        {"mutter-cursor", required_argument, NULL, OPT_MUTTER_CURSOR},
        {"vnc-fps", required_argument, NULL, OPT_VNC_FPS},
        {"latest-only", required_argument, NULL, OPT_LATEST_ONLY},
        {"external-send-buffer", required_argument, NULL, OPT_EXTERNAL_SEND_BUFFER},
        {"backend-recv-buffer", required_argument, NULL, OPT_BACKEND_RECV_BUFFER},
        {"diff-detect", required_argument, NULL, OPT_DIFF_DETECT},
        {"diff-tile-size", required_argument, NULL, OPT_DIFF_TILE_SIZE},
        {"layout-remember", required_argument, NULL, OPT_LAYOUT_REMEMBER},
        {"layout-resave", required_argument, NULL, OPT_LAYOUT_RESAVE},
        {"port", required_argument, NULL, OPT_PORT},
        {"backend-port", required_argument, NULL, OPT_BACKEND_PORT},
        {"backend-bind", required_argument, NULL, OPT_BACKEND_BIND},
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

    for (;;) {
        int c = getopt_long(argc, argv, "h", options, NULL);
        if (c == -1)
            break;

        switch (c) {
            case OPT_CAPTURE_BACKEND:
                if (parse_capture_backend(optarg, &cfg->capture_backend) < 0)
                    return -1;
                break;
            case OPT_CAPTURE_TIMEOUT_MS:
                if (parse_int("capture-timeout-ms", optarg, 100, 60000,
                              &cfg->capture_timeout_ms) < 0)
                    return -1;
                break;
            case OPT_MUTTER_CURSOR:
                if (parse_mutter_cursor(optarg, &cfg->mutter_cursor_mode) < 0)
                    return -1;
                break;
            case OPT_VNC_FPS:
                if (parse_vnc_fps(optarg, &cfg->vnc_max_fps) < 0)
                    return -1;
                break;
            case OPT_LATEST_ONLY:
                if (parse_bool("latest-only", optarg, &cfg->latest_only) < 0)
                    return -1;
                break;
            case OPT_EXTERNAL_SEND_BUFFER:
                if (parse_int("external-send-buffer", optarg, 4096, 4194304,
                              &cfg->external_send_buffer) < 0)
                    return -1;
                break;
            case OPT_BACKEND_RECV_BUFFER:
                if (parse_int("backend-recv-buffer", optarg, 4096, 4194304,
                              &cfg->backend_receive_buffer) < 0)
                    return -1;
                break;
            case OPT_DIFF_DETECT:
                if (parse_bool("diff-detect", optarg, &cfg->diff_detect) < 0)
                    return -1;
                break;
            case OPT_DIFF_TILE_SIZE:
                if (parse_int("diff-tile-size", optarg, 8, 256,
                              &cfg->diff_tile_size) < 0)
                    return -1;
                break;
            case OPT_LAYOUT_REMEMBER:
                if (parse_bool("layout-remember", optarg, &cfg->layout_remember) < 0)
                    return -1;
                break;
            case OPT_LAYOUT_RESAVE:
                if (parse_bool("layout-resave", optarg, &cfg->layout_resave) < 0)
                    return -1;
                break;
            case OPT_PORT:
                if (parse_int("port", optarg, 1, 65535, &cfg->public_port) < 0)
                    return -1;
                break;
            case OPT_BACKEND_PORT:
                if (parse_int("backend-port", optarg, 1, 65535, &cfg->backend_port) < 0)
                    return -1;
                break;
            case OPT_BACKEND_BIND:
                cfg->backend_bind = optarg;
                break;
            case OPT_WIDTH:
                if (parse_int("width", optarg, 64, 16384, &cfg->width) < 0)
                    return -1;
                break;
            case OPT_HEIGHT:
                if (parse_int("height", optarg, 64, 16384, &cfg->height) < 0)
                    return -1;
                break;
            case OPT_FPS:
                if (parse_int("fps", optarg, 1, 240, &cfg->max_fps) < 0)
                    return -1;
                break;
            case OPT_RA2_RECORD_SIZE:
                if (parse_int("ra2-record-size", optarg, 1, 65535,
                              &cfg->ra2_stream_record_max) < 0)
                    return -1;
                break;
            case OPT_RA2_COALESCE:
                if (parse_bool("ra2-coalesce", optarg, &cfg->ra2_coalesce) < 0)
                    return -1;
                break;
            case OPT_RA2_COALESCE_US:
                if (parse_int("ra2-coalesce-us", optarg, 0, 1000000,
                              &cfg->ra2_coalesce_us) < 0)
                    return -1;
                break;
            case OPT_RA2_KEY:
                cfg->ra2_key_file = optarg;
                break;
            case OPT_AUTH_SOCKET:
                cfg->auth_socket = optarg;
                break;
            case OPT_ZRLE:
                if (parse_bool("zrle", optarg, &cfg->enable_zrle) < 0)
                    return -1;
                break;
            case OPT_RAW:
                if (parse_bool("raw", optarg, &cfg->enable_raw) < 0)
                    return -1;
                break;
            case OPT_CURSOR:
                if (parse_bool("cursor", optarg, &cfg->enable_cursor) < 0)
                    return -1;
                break;
            case OPT_NEWFBSIZE:
                if (parse_bool("newfbsize", optarg, &cfg->enable_newfbsize) < 0)
                    return -1;
                break;
            case OPT_VERBOSE:
                if (vnc_log_parse_level(optarg, &cfg->verbose) < 0) {
                    fprintf(stderr,
                            "Invalid --verbose level: %s (use 0|error, 1|info, 2|debug, 3|trace)\n",
                            optarg);
                    return -1;
                }
                break;
            case OPT_SHOW_CONFIG:
                apply_log_level(cfg);
                runtime_config_print(cfg);
                exit(0);
            case OPT_VERSION:
                printf("%s\n", VNC_MONITOR_VERSION);
                exit(0);
            case 'h':
                runtime_config_usage(argv[0]);
                exit(0);
            default:
                return -1;
        }
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
    return 0;
}
