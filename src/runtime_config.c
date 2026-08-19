#define _GNU_SOURCE

#include "runtime_config.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
parse_int(
    const char *name,
    const char *value,
    int min_value,
    int max_value,
    int *out)
{
    errno = 0;
    char *end = NULL;
    long v = strtol(value, &end, 10);

    if (
        !value || !*value ||
        errno != 0 ||
        !end ||
        *end != '\0' ||
        v < min_value ||
        v > max_value
    ) {
        fprintf(
            stderr,
            "Invalid %s: %s (expected %d..%d)\n",
            name,
            value ? value : "(null)",
            min_value,
            max_value
        );

        return -1;
    }

    *out = (int)v;
    return 0;
}

static int
parse_double(
    const char *name,
    const char *value,
    double min_value,
    double max_value,
    double *out)
{
    errno = 0;
    char *end = NULL;
    double v = strtod(value, &end);

    if (
        !value || !*value ||
        errno != 0 ||
        !end ||
        *end != '\0' ||
        v < min_value ||
        v > max_value
    ) {
        fprintf(
            stderr,
            "Invalid %s: %s (expected %.3f..%.3f)\n",
            name,
            value ? value : "(null)",
            min_value,
            max_value
        );

        return -1;
    }

    *out = v;
    return 0;
}

static int
parse_bool(
    const char *name,
    const char *value,
    int *out)
{
    if (
        strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0
    ) {
        *out = 1;
        return 0;
    }

    if (
        strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0
    ) {
        *out = 0;
        return 0;
    }

    fprintf(
        stderr,
        "Invalid %s: %s (use on/off, true/false or 1/0)\n",
        name,
        value
    );

    return -1;
}

const char *
runtime_config_source_name(FrameSourceMode mode)
{
    return
        mode == FRAME_SOURCE_TEST
            ? "test"
            : "mutter";
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
    fprintf(stderr, "Invalid capture backend: %s (use pipewire or gstreamer)\n", value);
    return -1;
}

static int
parse_source_mode(
    const char *value,
    FrameSourceMode *out)
{
    if (strcasecmp(value, "mutter") == 0) {
        *out = FRAME_SOURCE_MUTTER;
        return 0;
    }

    if (strcasecmp(value, "test") == 0) {
        *out = FRAME_SOURCE_TEST;
        return 0;
    }

    fprintf(
        stderr,
        "Invalid source: %s (use mutter or test)\n",
        value
    );

    return -1;
}

const char *
runtime_config_benchmark_name(BenchmarkMode mode)
{
    switch (mode) {
        case BENCHMARK_OFF:
            return "off";
        case BENCHMARK_SQUARE_SWEEP:
            return "square-sweep";
        case BENCHMARK_SUITE:
            return "suite";
        default:
            return "unknown";
    }
}

const char *
runtime_config_damage_name(DamageMode mode)
{
    return mode == DAMAGE_FULL ? "full" : "rect";
}

static int
parse_benchmark_mode(
    const char *value,
    BenchmarkMode *out)
{
    if (strcasecmp(value, "off") == 0) {
        *out = BENCHMARK_OFF;
        return 0;
    }

    if (
        strcasecmp(value, "square") == 0 ||
        strcasecmp(value, "square-sweep") == 0
    ) {
        *out = BENCHMARK_SQUARE_SWEEP;
        return 0;
    }

    if (strcasecmp(value, "suite") == 0) {
        *out = BENCHMARK_SUITE;
        return 0;
    }

    fprintf(
        stderr,
        "Invalid benchmark mode: %s "
        "(use off, square-sweep or suite)\n",
        value
    );

    return -1;
}

static int
parse_damage_mode(
    const char *value,
    DamageMode *out)
{
    if (strcasecmp(value, "rect") == 0) {
        *out = DAMAGE_RECT;
        return 0;
    }

    if (strcasecmp(value, "full") == 0) {
        *out = DAMAGE_FULL;
        return 0;
    }

    fprintf(
        stderr,
        "Invalid damage mode: %s (use rect or full)\n",
        value
    );

    return -1;
}

void
runtime_config_defaults(RuntimeConfig *cfg)
{
    *cfg = (RuntimeConfig) {
        .source_mode = FRAME_SOURCE_MUTTER,
        .capture_backend = CAPTURE_BACKEND_PIPEWIRE,
        .capture_timeout_ms = 5000,
        .mutter_cursor_mode = MUTTER_CURSOR_HIDDEN,
        .mutter_hardware_cursor_mode = MUTTER_HW_CURSOR_AUTO,
        .vnc_max_fps = 0,
        .external_send_buffer = 65536,
        .backend_receive_buffer = 65536,
        .diff_detect = 1,
        .diff_tile_size = 32,
        .layout_remember = 1,
        .layout_resave = 0,
        .frame_stats = 1,
        .frame_trace = 0,
        .capture_trace = 0,
        .capture_stall_ms = 500,
        .frame_stats_interval_ms = 1000,

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
        .verbose = 1,

        .auth_socket = "/run/vnc-monitor-auth.sock",
        .ra2_key_file = "./ra2-server-key.pem",
        .backend_bind = "127.0.0.1",

        .square_min = 64,
        .square_max = 384,
        .square_speed = 180.0,
        .square_cycle_seconds = 4.0,
        .damage_mode = DAMAGE_RECT,

        .benchmark_mode = BENCHMARK_OFF,
        .benchmark_interval_seconds = 1.0,
        .benchmark_step_seconds = 5.0,
        .benchmark_square_min = 64,
        .benchmark_square_max = 768,
        .benchmark_square_step = 64,
        .benchmark_csv = NULL
    };
}

void
runtime_config_usage(const char *argv0)
{
    printf(
        "Usage: %s [options]\n"
        "\n"
        "Source:\n"
        "  --source mutter|test         Frame source                [mutter]\n"
        "  --capture-backend MODE       pipewire|gstreamer        [pipewire]\n"
        "  --capture-timeout-ms N       Wait for first real frame   [5000]\n"
        "  --mutter-cursor MODE         hidden|embedded|metadata   [hidden]\n"
        "  --mutter-hardware-cursor M   auto|enabled|disabled         [auto]\n"
        "  --vnc-fps source|N           Max VNC publish rate      [source]\n"
        "                              source/0 = every new source frame\n"
        "  --external-send-buffer N     RA2 TCP SO_SNDBUF bytes     [65536]\n"
        "  --backend-recv-buffer N      Backend TCP SO_RCVBUF bytes [65536]\n"
        "  --diff-detect on|off         Diff real frames by tiles    [on]\n"
        "  --diff-tile-size N           Diff tile size               [32]\n"
        "  --layout-remember on|off     Restore saved GNOME layout   [on]\n"
        "  --layout-resave on|off       Overwrite cached layout      [off]\n"
        "  --frame-stats on|off         Pipeline stats once/sec      [on]\n"
        "  --frame-trace on|off         One line per VNC publish     [off]\n"
        "  --capture-trace on|off       One line per capture frame   [off]\n"
        "  --capture-stall-ms N         Capture stall threshold      [500]\n"
        "  --frame-stats-interval-ms N  Stats interval               [1000]\n"
        "\n"
        "Network:\n"
        "  --port N                    Public RA2r VNC port       [5901]\n"
        "  --backend-port N            Internal backend port      [5903]\n"
        "  --backend-bind ADDR         Internal bind address      [127.0.0.1]\n"
        "\n"
        "Framebuffer / workload:\n"
        "  --width N                   Width                      [1024]\n"
        "  --height N                  Height                     [768]\n"
        "  --fps N                     Maximum source FPS         [60]\n"
        "  --square-min N              Normal pattern min square  [64]\n"
        "  --square-max N              Normal pattern max square  [384]\n"
        "  --square-speed N            Movement pixels/sec        [180]\n"
        "  --square-cycle SEC          Size cycle duration        [4]\n"
        "  --damage rect|full          Dirty rectangle strategy   [rect]\n"
        "\n"
        "RA2r:\n"
        "  --ra2-record-size N         Max encrypted payload      [16384]\n"
        "  --ra2-coalesce on|off       Merge backend reads         [on]\n"
        "  --ra2-coalesce-us N         Max merge delay, usec       [500]\n"
        "                              Protocol limit is 65535\n"
        "  --ra2-key FILE              Persistent RSA key         [./ra2-server-key.pem]\n"
        "  --auth-socket PATH          PAM helper socket          [/run/vnc-monitor-auth.sock]\n"
        "\n"
        "RFB compatibility switches:\n"
        "  --zrle on|off               Prefer/allow ZRLE flag      [on]\n"
        "  --raw on|off                Prefer/allow Raw flag       [on]\n"
        "  --cursor on|off             Cursor extension flag       [on]\n"
        "  --newfbsize on|off          NewFBSize extension flag    [on]\n"
        "\n"
        "Input / security:\n"
        "  --view-only on|off          Ignore keyboard/pointer    [on]\n"
        "  --clipboard on|off          Client clipboard input     [off]\n"
        "  --file-transfer on|off      File transfer              [off]\n"
        "\n"
        "Benchmark:\n"
        "  --benchmark MODE            off|square-sweep|suite      [off]\n"
        "  --benchmark-interval SEC    Stats/CSV interval          [1]\n"
        "  --benchmark-step SEC        Duration of each stage      [5]\n"
        "  --benchmark-square-min N    Sweep minimum               [64]\n"
        "  --benchmark-square-max N    Sweep maximum               [768]\n"
        "  --benchmark-square-step N   Sweep step                  [64]\n"
        "  --benchmark-csv FILE        Append detailed CSV         [disabled]\n"
        "\n"
        "Diagnostics:\n"
        "  --verbose on|off            Extra diagnostics          [on]\n"
        "  --show-config               Print config and exit\n"
        "  -h, --help                  Show this help\n"
        "\n"
        "Examples:\n"
        "  %s --ra2-record-size 32768\n"
        "  %s --benchmark square-sweep --benchmark-csv square.csv\n"
        "  %s --benchmark suite --benchmark-step 10 --benchmark-csv suite.csv\n"
        "  %s --benchmark suite --damage full --ra2-record-size 32768\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

void
runtime_config_print(const RuntimeConfig *cfg)
{
    printf(
        "Runtime configuration:\n"
        "  source:             %s\n"
        "  capture backend:    %s\n"
        "  capture timeout:    %d ms\n"
        "  Mutter cursor:      %s\n"
        "  Mutter HW cursor:   %s (requested)\n"
        "  VNC publisher:      source-driven\n"
        "  VNC max rate:       %s\n"
        "  external sndbuf:    %d bytes\n"
        "  backend rcvbuf:     %d bytes\n"
        "  diff detect:        %s\n"
        "  diff tile size:     %d px\n"
        "  layout remember:    %s\n"
        "  layout resave:      %s\n"
        "  frame stats:        %s\n"
        "  frame trace:        %s\n"
        "  capture trace:      %s\n"
        "  capture stall:      %d ms\n"
        "  stats interval:     %d ms\n"
        "  public port:        %d\n"
        "  backend:            %s:%d\n"
        "  framebuffer:        %dx%d @ max %d FPS\n"
        "  RA2 record max:     %d bytes\n"
        "  RA2 coalesce:       %s\n"
        "  RA2 coalesce wait:  %d us\n"
        "  RA2 key:            %s\n"
        "  auth socket:        %s\n"
        "  normal square:      %d..%d px, %.1f px/s, %.2f s cycle\n"
        "  damage:             %s\n"
        "  ZRLE flag:          %s\n"
        "  Raw flag:           %s\n"
        "  cursor flag:        %s\n"
        "  NewFBSize flag:     %s\n"
        "  view-only:          %s\n"
        "  clipboard input:    %s\n"
        "  file transfer:      %s\n"
        "  benchmark:          %s\n"
        "  benchmark interval: %.3f s\n"
        "  benchmark step:     %.3f s\n"
        "  benchmark square:   %d..%d step %d\n"
        "  benchmark CSV:      %s\n"
        "  verbose:            %s\n",
        runtime_config_source_name(cfg->source_mode),
        runtime_config_capture_backend_name(cfg->capture_backend),
        cfg->capture_timeout_ms,
        runtime_config_mutter_cursor_name(cfg->mutter_cursor_mode),
        runtime_config_mutter_hardware_cursor_name(cfg->mutter_hardware_cursor_mode),
        cfg->vnc_max_fps > 0 ? "rate-limited" : "unlimited/source",
        cfg->external_send_buffer,
        cfg->backend_receive_buffer,
        cfg->diff_detect ? "on" : "off",
        cfg->diff_tile_size,
        cfg->layout_remember ? "on" : "off",
        cfg->layout_resave ? "on" : "off",
        cfg->frame_stats ? "on" : "off",
        cfg->frame_trace ? "on" : "off",
        cfg->capture_trace ? "on" : "off",
        cfg->capture_stall_ms,
        cfg->frame_stats_interval_ms,
        cfg->public_port,
        cfg->backend_bind,
        cfg->backend_port,
        cfg->width,
        cfg->height,
        cfg->max_fps,
        cfg->ra2_stream_record_max,
        cfg->ra2_coalesce ? "on" : "off",
        cfg->ra2_coalesce_us,
        cfg->ra2_key_file,
        cfg->auth_socket,
        cfg->square_min,
        cfg->square_max,
        cfg->square_speed,
        cfg->square_cycle_seconds,
        runtime_config_damage_name(cfg->damage_mode),
        cfg->enable_zrle ? "on" : "off",
        cfg->enable_raw ? "on" : "off",
        cfg->enable_cursor ? "on" : "off",
        cfg->enable_newfbsize ? "on" : "off",
        cfg->view_only ? "on" : "off",
        cfg->enable_clipboard ? "on" : "off",
        cfg->enable_file_transfer ? "on" : "off",
        runtime_config_benchmark_name(cfg->benchmark_mode),
        cfg->benchmark_interval_seconds,
        cfg->benchmark_step_seconds,
        cfg->benchmark_square_min,
        cfg->benchmark_square_max,
        cfg->benchmark_square_step,
        cfg->benchmark_csv ? cfg->benchmark_csv : "(disabled)",
        cfg->verbose ? "on" : "off"
    );

    if (cfg->vnc_max_fps > 0)
        printf("  VNC max FPS value:  %d FPS\n", cfg->vnc_max_fps);
}

const char *
runtime_config_mutter_cursor_name(
    MutterCursorMode mode)
{
    switch (mode) {
        case MUTTER_CURSOR_HIDDEN:
            return "hidden";

        case MUTTER_CURSOR_EMBEDDED:
            return "embedded";

        case MUTTER_CURSOR_METADATA:
            return "metadata";
    }

    return "unknown";
}

const char *
runtime_config_mutter_hardware_cursor_name(
    MutterHardwareCursorMode mode)
{
    switch (mode) {
        case MUTTER_HW_CURSOR_AUTO:
            return "auto";
        case MUTTER_HW_CURSOR_ENABLED:
            return "enabled";
        case MUTTER_HW_CURSOR_DISABLED:
            return "disabled";
    }

    return "unknown";
}

static int
parse_mutter_hardware_cursor(
    const char *value,
    MutterHardwareCursorMode *out)
{
    if (!value || !out)
        return -1;

    if (strcasecmp(value, "auto") == 0) {
        *out = MUTTER_HW_CURSOR_AUTO;
        return 0;
    }

    if (strcasecmp(value, "enabled") == 0 ||
        strcasecmp(value, "on") == 0) {
        *out = MUTTER_HW_CURSOR_ENABLED;
        return 0;
    }

    if (strcasecmp(value, "disabled") == 0 ||
        strcasecmp(value, "off") == 0) {
        *out = MUTTER_HW_CURSOR_DISABLED;
        return 0;
    }

    fprintf(
        stderr,
        "Invalid --mutter-hardware-cursor value: %s "
        "(expected auto|enabled|disabled)\n",
        value
    );

    return -1;
}

static int
parse_vnc_fps(
    const char *value,
    int *out)
{
    if (!value || !out)
        return -1;

    if (strcasecmp(value, "source") == 0 ||
        strcasecmp(value, "unlimited") == 0) {
        *out = 0;
        return 0;
    }

    return parse_int("vnc-fps", value, 0, 240, out);
}

static int
parse_mutter_cursor(
    const char *value,
    MutterCursorMode *out)
{
    if (!value || !out)
        return -1;

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

    fprintf(
        stderr,
        "Invalid --mutter-cursor value: %s "
        "(expected hidden|embedded|metadata)\n",
        value
    );

    return -1;
}

int
runtime_config_parse(
    RuntimeConfig *cfg,
    int argc,
    char **argv)
{
    enum {
        OPT_SOURCE = 1000,
        OPT_CAPTURE_BACKEND,
        OPT_CAPTURE_TIMEOUT_MS,
        OPT_MUTTER_CURSOR,
        OPT_MUTTER_HARDWARE_CURSOR,
        OPT_VNC_FPS,
        OPT_EXTERNAL_SEND_BUFFER,
        OPT_BACKEND_RECV_BUFFER,
        OPT_DIFF_DETECT,
        OPT_DIFF_TILE_SIZE,
        OPT_LAYOUT_REMEMBER,
        OPT_LAYOUT_RESAVE,
        OPT_FRAME_STATS,
        OPT_FRAME_TRACE,
        OPT_CAPTURE_TRACE,
        OPT_CAPTURE_STALL_MS,
        OPT_FRAME_STATS_INTERVAL_MS,
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
        OPT_VIEW_ONLY,
        OPT_CLIPBOARD,
        OPT_FILE_TRANSFER,
        OPT_VERBOSE,
        OPT_SHOW_CONFIG,
        OPT_SQUARE_MIN,
        OPT_SQUARE_MAX,
        OPT_SQUARE_SPEED,
        OPT_SQUARE_CYCLE,
        OPT_DAMAGE,
        OPT_BENCHMARK,
        OPT_BENCHMARK_INTERVAL,
        OPT_BENCHMARK_STEP,
        OPT_BENCHMARK_SQUARE_MIN,
        OPT_BENCHMARK_SQUARE_MAX,
        OPT_BENCHMARK_SQUARE_STEP,
        OPT_BENCHMARK_CSV
    };

    static const struct option options[] = {
        {"source", required_argument, NULL, OPT_SOURCE},
        {"capture-backend", required_argument, NULL, OPT_CAPTURE_BACKEND},
        {"capture-timeout-ms", required_argument, NULL, OPT_CAPTURE_TIMEOUT_MS},
        {"mutter-cursor", required_argument, NULL, OPT_MUTTER_CURSOR},
        {"mutter-hardware-cursor", required_argument, NULL, OPT_MUTTER_HARDWARE_CURSOR},
        {"vnc-fps", required_argument, NULL, OPT_VNC_FPS},
        {"external-send-buffer", required_argument, NULL, OPT_EXTERNAL_SEND_BUFFER},
        {"backend-recv-buffer", required_argument, NULL, OPT_BACKEND_RECV_BUFFER},
        {"diff-detect", required_argument, NULL, OPT_DIFF_DETECT},
        {"diff-tile-size", required_argument, NULL, OPT_DIFF_TILE_SIZE},
        {"layout-remember", required_argument, NULL, OPT_LAYOUT_REMEMBER},
        {"layout-resave", required_argument, NULL, OPT_LAYOUT_RESAVE},
        {"frame-stats", required_argument, NULL, OPT_FRAME_STATS},
        {"frame-trace", required_argument, NULL, OPT_FRAME_TRACE},
        {"capture-trace", required_argument, NULL, OPT_CAPTURE_TRACE},
        {"capture-stall-ms", required_argument, NULL, OPT_CAPTURE_STALL_MS},
        {"frame-stats-interval-ms", required_argument, NULL, OPT_FRAME_STATS_INTERVAL_MS},
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
        {"view-only", required_argument, NULL, OPT_VIEW_ONLY},
        {"clipboard", required_argument, NULL, OPT_CLIPBOARD},
        {"file-transfer", required_argument, NULL, OPT_FILE_TRANSFER},
        {"verbose", required_argument, NULL, OPT_VERBOSE},
        {"show-config", no_argument, NULL, OPT_SHOW_CONFIG},
        {"square-min", required_argument, NULL, OPT_SQUARE_MIN},
        {"square-max", required_argument, NULL, OPT_SQUARE_MAX},
        {"square-speed", required_argument, NULL, OPT_SQUARE_SPEED},
        {"square-cycle", required_argument, NULL, OPT_SQUARE_CYCLE},
        {"damage", required_argument, NULL, OPT_DAMAGE},
        {"benchmark", required_argument, NULL, OPT_BENCHMARK},
        {"benchmark-interval", required_argument, NULL, OPT_BENCHMARK_INTERVAL},
        {"benchmark-step", required_argument, NULL, OPT_BENCHMARK_STEP},
        {"benchmark-square-min", required_argument, NULL, OPT_BENCHMARK_SQUARE_MIN},
        {"benchmark-square-max", required_argument, NULL, OPT_BENCHMARK_SQUARE_MAX},
        {"benchmark-square-step", required_argument, NULL, OPT_BENCHMARK_SQUARE_STEP},
        {"benchmark-csv", required_argument, NULL, OPT_BENCHMARK_CSV},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    for (;;) {
        int c =
            getopt_long(
                argc,
                argv,
                "h",
                options,
                NULL
            );

        if (c == -1)
            break;

        switch (c) {
            case OPT_SOURCE:
                if (
                    parse_source_mode(
                        optarg,
                        &cfg->source_mode
                    ) < 0
                )
                    return -1;
                break;

            case OPT_CAPTURE_BACKEND:
                if (parse_capture_backend(optarg, &cfg->capture_backend) < 0)
                    return -1;
                break;

            case OPT_CAPTURE_TIMEOUT_MS:
                if (
                    parse_int(
                        "capture-timeout-ms",
                        optarg,
                        100,
                        60000,
                        &cfg->capture_timeout_ms
                    ) < 0
                )
                    return -1;
                break;

            case OPT_MUTTER_CURSOR:
                if (
                    parse_mutter_cursor(
                        optarg,
                        &cfg->mutter_cursor_mode
                    ) < 0
                )
                    return -1;
                break;

            case OPT_MUTTER_HARDWARE_CURSOR:
                if (
                    parse_mutter_hardware_cursor(
                        optarg,
                        &cfg->mutter_hardware_cursor_mode
                    ) < 0
                )
                    return -1;
                break;

            case OPT_VNC_FPS:
                if (parse_vnc_fps(optarg, &cfg->vnc_max_fps) < 0)
                    return -1;
                break;

            case OPT_EXTERNAL_SEND_BUFFER:
                if (
                    parse_int(
                        "external-send-buffer",
                        optarg,
                        4096,
                        4194304,
                        &cfg->external_send_buffer
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BACKEND_RECV_BUFFER:
                if (
                    parse_int(
                        "backend-recv-buffer",
                        optarg,
                        4096,
                        4194304,
                        &cfg->backend_receive_buffer
                    ) < 0
                )
                    return -1;
                break;

            case OPT_DIFF_DETECT:
                if (
                    parse_bool(
                        "diff-detect",
                        optarg,
                        &cfg->diff_detect
                    ) < 0
                )
                    return -1;
                break;

            case OPT_DIFF_TILE_SIZE:
                if (
                    parse_int(
                        "diff-tile-size",
                        optarg,
                        8,
                        256,
                        &cfg->diff_tile_size
                    ) < 0
                )
                    return -1;
                break;

            case OPT_LAYOUT_REMEMBER:
                if (
                    parse_bool(
                        "layout-remember",
                        optarg,
                        &cfg->layout_remember
                    ) < 0
                )
                    return -1;
                break;

            case OPT_LAYOUT_RESAVE:
                if (
                    parse_bool(
                        "layout-resave",
                        optarg,
                        &cfg->layout_resave
                    ) < 0
                )
                    return -1;
                break;

            case OPT_FRAME_STATS:
                if (
                    parse_bool(
                        "frame-stats",
                        optarg,
                        &cfg->frame_stats
                    ) < 0
                )
                    return -1;
                break;

            case OPT_FRAME_TRACE:
                if (
                    parse_bool(
                        "frame-trace",
                        optarg,
                        &cfg->frame_trace
                    ) < 0
                )
                    return -1;
                break;

            case OPT_CAPTURE_TRACE:
                if (
                    parse_bool(
                        "capture-trace",
                        optarg,
                        &cfg->capture_trace
                    ) < 0
                )
                    return -1;
                break;

            case OPT_CAPTURE_STALL_MS:
                if (
                    parse_int(
                        "capture-stall-ms",
                        optarg,
                        50,
                        60000,
                        &cfg->capture_stall_ms
                    ) < 0
                )
                    return -1;
                break;

            case OPT_FRAME_STATS_INTERVAL_MS:
                if (
                    parse_int(
                        "frame-stats-interval-ms",
                        optarg,
                        100,
                        60000,
                        &cfg->frame_stats_interval_ms
                    ) < 0
                )
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
                if (
                    parse_int(
                        "ra2-record-size",
                        optarg,
                        1,
                        65535,
                        &cfg->ra2_stream_record_max
                    ) < 0
                )
                    return -1;
                break;

            case OPT_RA2_COALESCE:
                if (
                    parse_bool(
                        "ra2-coalesce",
                        optarg,
                        &cfg->ra2_coalesce
                    ) < 0
                )
                    return -1;
                break;

            case OPT_RA2_COALESCE_US:
                if (
                    parse_int(
                        "ra2-coalesce-us",
                        optarg,
                        0,
                        1000000,
                        &cfg->ra2_coalesce_us
                    ) < 0
                )
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

            case OPT_VIEW_ONLY:
                if (parse_bool("view-only", optarg, &cfg->view_only) < 0)
                    return -1;
                break;

            case OPT_CLIPBOARD:
                if (parse_bool("clipboard", optarg, &cfg->enable_clipboard) < 0)
                    return -1;
                break;

            case OPT_FILE_TRANSFER:
                if (parse_bool("file-transfer", optarg, &cfg->enable_file_transfer) < 0)
                    return -1;
                break;

            case OPT_VERBOSE:
                if (parse_bool("verbose", optarg, &cfg->verbose) < 0)
                    return -1;
                break;

            case OPT_SQUARE_MIN:
                if (parse_int("square-min", optarg, 1, 16384, &cfg->square_min) < 0)
                    return -1;
                break;

            case OPT_SQUARE_MAX:
                if (parse_int("square-max", optarg, 1, 16384, &cfg->square_max) < 0)
                    return -1;
                break;

            case OPT_SQUARE_SPEED:
                if (parse_double("square-speed", optarg, 0.0, 10000.0, &cfg->square_speed) < 0)
                    return -1;
                break;

            case OPT_SQUARE_CYCLE:
                if (parse_double("square-cycle", optarg, 0.05, 3600.0, &cfg->square_cycle_seconds) < 0)
                    return -1;
                break;

            case OPT_DAMAGE:
                if (parse_damage_mode(optarg, &cfg->damage_mode) < 0)
                    return -1;
                break;

            case OPT_BENCHMARK:
                if (parse_benchmark_mode(optarg, &cfg->benchmark_mode) < 0)
                    return -1;
                break;

            case OPT_BENCHMARK_INTERVAL:
                if (
                    parse_double(
                        "benchmark-interval",
                        optarg,
                        0.1,
                        3600.0,
                        &cfg->benchmark_interval_seconds
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BENCHMARK_STEP:
                if (
                    parse_double(
                        "benchmark-step",
                        optarg,
                        0.25,
                        3600.0,
                        &cfg->benchmark_step_seconds
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BENCHMARK_SQUARE_MIN:
                if (
                    parse_int(
                        "benchmark-square-min",
                        optarg,
                        1,
                        16384,
                        &cfg->benchmark_square_min
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BENCHMARK_SQUARE_MAX:
                if (
                    parse_int(
                        "benchmark-square-max",
                        optarg,
                        1,
                        16384,
                        &cfg->benchmark_square_max
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BENCHMARK_SQUARE_STEP:
                if (
                    parse_int(
                        "benchmark-square-step",
                        optarg,
                        1,
                        16384,
                        &cfg->benchmark_square_step
                    ) < 0
                )
                    return -1;
                break;

            case OPT_BENCHMARK_CSV:
                cfg->benchmark_csv = optarg;
                break;

            case OPT_SHOW_CONFIG:
                runtime_config_print(cfg);
                exit(0);

            case 'h':
                runtime_config_usage(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

    if (optind != argc) {
        fprintf(
            stderr,
            "Unexpected positional argument: %s\n",
            argv[optind]
        );
        return -1;
    }

    if (!cfg->enable_zrle && !cfg->enable_raw) {
        fprintf(
            stderr,
            "At least one of --zrle or --raw must be enabled\n"
        );
        return -1;
    }

    if (
        cfg->square_min > cfg->square_max ||
        cfg->benchmark_square_min > cfg->benchmark_square_max
    ) {
        fprintf(stderr, "Square minimum cannot exceed maximum\n");
        return -1;
    }

    return 0;
}
