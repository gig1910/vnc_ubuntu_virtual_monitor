#ifndef VNC_MONITOR_RUNTIME_CONFIG_H
#define VNC_MONITOR_RUNTIME_CONFIG_H

typedef enum {
    FRAME_SOURCE_MUTTER = 0,
    FRAME_SOURCE_TEST
} FrameSourceMode;

typedef enum {
    CAPTURE_BACKEND_PIPEWIRE = 0,
    CAPTURE_BACKEND_GSTREAMER
} CaptureBackend;

typedef enum {
    BENCHMARK_OFF = 0,
    BENCHMARK_SQUARE_SWEEP,
    BENCHMARK_SUITE
} BenchmarkMode;

typedef enum {
    DAMAGE_RECT = 0,
    DAMAGE_FULL
} DamageMode;

typedef enum {
    MUTTER_CURSOR_HIDDEN = 0,
    MUTTER_CURSOR_EMBEDDED = 1,
    MUTTER_CURSOR_METADATA = 2
} MutterCursorMode;

typedef enum {
    MUTTER_HW_CURSOR_AUTO = 0,
    MUTTER_HW_CURSOR_ENABLED,
    MUTTER_HW_CURSOR_DISABLED
} MutterHardwareCursorMode;

typedef struct {
    FrameSourceMode source_mode;
    CaptureBackend capture_backend;
    int capture_timeout_ms;
    MutterCursorMode mutter_cursor_mode;
    MutterHardwareCursorMode mutter_hardware_cursor_mode;
    /* 0 = source-driven: publish every newly consumed source frame. */
    int vnc_max_fps;
    int external_send_buffer;
    int backend_receive_buffer;
    int diff_detect;
    int diff_tile_size;
    int layout_remember;
    int layout_resave;
    int frame_stats;
    int frame_trace;
    int capture_trace;
    int capture_stall_ms;
    int frame_stats_interval_ms;

    int public_port;
    int backend_port;
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
    int enable_clipboard;
    int enable_file_transfer;
    int view_only;
    int verbose;

    const char *auth_socket;
    const char *ra2_key_file;
    const char *backend_bind;

    /* Normal synthetic workload. */
    int square_min;
    int square_max;
    double square_speed;
    double square_cycle_seconds;
    DamageMode damage_mode;

    /* Benchmark. */
    BenchmarkMode benchmark_mode;
    double benchmark_interval_seconds;
    double benchmark_step_seconds;
    int benchmark_square_min;
    int benchmark_square_max;
    int benchmark_square_step;
    const char *benchmark_csv;
} RuntimeConfig;

void runtime_config_defaults(RuntimeConfig *cfg);
int runtime_config_parse(RuntimeConfig *cfg, int argc, char **argv);
void runtime_config_print(const RuntimeConfig *cfg);
void runtime_config_usage(const char *argv0);

const char *runtime_config_source_name(FrameSourceMode mode);
const char *runtime_config_capture_backend_name(CaptureBackend backend);
const char *runtime_config_mutter_cursor_name(MutterCursorMode mode);
const char *runtime_config_mutter_hardware_cursor_name(MutterHardwareCursorMode mode);
const char *runtime_config_benchmark_name(BenchmarkMode mode);
const char *runtime_config_damage_name(DamageMode mode);

#endif
