#ifndef VNC_MONITOR_RUNTIME_CONFIG_H
#define VNC_MONITOR_RUNTIME_CONFIG_H

#include <limits.h>

typedef enum {
    CAPTURE_BACKEND_PIPEWIRE = 0,
    CAPTURE_BACKEND_GSTREAMER
} CaptureBackend;

typedef enum {
    MUTTER_CURSOR_HIDDEN = 0,
    MUTTER_CURSOR_EMBEDDED = 1,
    MUTTER_CURSOR_METADATA = 2
} MutterCursorMode;

typedef enum {
    SCREEN_SIZE_AUTO = 0,
    SCREEN_SIZE_FIXED = 1
} ScreenSizeMode;

typedef struct {
    CaptureBackend capture_backend;
    int capture_timeout_ms;
    MutterCursorMode mutter_cursor_mode;

    /* 0 = source-driven: publish every newly consumed source frame. */
    int vnc_max_fps;
    int latest_only;

    int external_send_buffer;
    int backend_receive_buffer;

    /*
     * External client liveness policy. Keepalive/user-timeout are transport
     * liveness controls, not application-idle timers. Handshake timeout only
     * limits the unauthenticated RA2/PAM negotiation phase.
     */
    int client_keepalive_idle_s;
    int client_keepalive_interval_s;
    int client_keepalive_probes;
    int client_user_timeout_ms;
    int client_handshake_timeout_ms;

    int diff_detect;
    int diff_tile_size;
    int layout_remember;
    int layout_resave;

    /* Internal telemetry derived from --verbose. Not separate CLI knobs. */
    int frame_stats;
    int frame_trace;
    int latency_trace;
    int capture_trace;
    int capture_stall_ms;
    int frame_stats_interval_ms;

    int public_port;
    int backend_port;

    /*
     * AUTO: width/height are the initial/fallback size. A client that
     * supports ExtendedDesktopSize may request another size during the
     * session. FIXED: client resize requests are rejected.
     */
    ScreenSizeMode screen_size_mode;
    int width;
    int height;
    int max_fps;

    int ra2_stream_record_max;
    int ra2_coalesce;
    int ra2_coalesce_us;

    int enable_zrle;
    int enable_raw;
    int enable_cursor;
    int enable_newfbsize;

    /* Production security invariants: fixed to view-only. */
    int enable_clipboard;
    int enable_file_transfer;
    int view_only;

    int verbose;

    /*
     * Layered configuration:
     * built-ins < /etc/vnc-monitor/config.ini < user/--config < CLI.
     * config_file is the normal per-user override path, or the explicit
     * --config path when one is supplied.
     */
    char system_config_file[PATH_MAX];
    char config_file[PATH_MAX];

    /* Owned storage: config/CLI strings never point into temporary buffers. */
    char auth_socket[PATH_MAX];
    char ra2_key_file[PATH_MAX];
    char backend_bind[64];
} RuntimeConfig;

void runtime_config_defaults(RuntimeConfig *cfg);
int runtime_config_parse(RuntimeConfig *cfg, int argc, char **argv);
void runtime_config_print(const RuntimeConfig *cfg);
void runtime_config_usage(const char *argv0);

const char *runtime_config_capture_backend_name(CaptureBackend backend);
const char *runtime_config_mutter_cursor_name(MutterCursorMode mode);
const char *runtime_config_screen_size_mode_name(ScreenSizeMode mode);

#endif
