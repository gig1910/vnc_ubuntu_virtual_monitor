#define _GNU_SOURCE

#include "benchmark.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    int initialized;

    const RuntimeConfig *cfg;
    FILE *csv;

    double started_at;
    double interval_started_at;

    char stage[64];
    int square_size;

    uint64_t frames;
    uint64_t dirty_pixels;

    double render_ms_sum;
    double render_ms_max;

    double rfb_ms_sum;
    double rfb_ms_max;

    double lateness_ms_sum;
    double lateness_ms_max;
    uint64_t late_frames;

    uint64_t backend_reads;
    uint64_t backend_read_bytes;

    uint64_t ra2_out_records;
    uint64_t ra2_out_payload;
    double ra2_send_ms_sum;
    double ra2_send_ms_max;

    uint64_t ra2_in_records;
    uint64_t ra2_in_payload;
} BenchmarkState;

static BenchmarkState b;

double
benchmark_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec / 1000000000.0;
}

static void
reset_interval_locked(double now)
{
    b.interval_started_at = now;

    b.frames = 0;
    b.dirty_pixels = 0;

    b.render_ms_sum = 0.0;
    b.render_ms_max = 0.0;

    b.rfb_ms_sum = 0.0;
    b.rfb_ms_max = 0.0;

    b.lateness_ms_sum = 0.0;
    b.lateness_ms_max = 0.0;
    b.late_frames = 0;

    b.backend_reads = 0;
    b.backend_read_bytes = 0;

    b.ra2_out_records = 0;
    b.ra2_out_payload = 0;
    b.ra2_send_ms_sum = 0.0;
    b.ra2_send_ms_max = 0.0;

    b.ra2_in_records = 0;
    b.ra2_in_payload = 0;
}

int
benchmark_init(const RuntimeConfig *cfg)
{
    memset(&b, 0, sizeof(b));

    if (pthread_mutex_init(&b.lock, NULL) != 0)
        return -1;

    b.cfg = cfg;
    b.started_at = benchmark_now();
    b.interval_started_at = b.started_at;
    snprintf(b.stage, sizeof(b.stage), "normal");

    if (cfg->benchmark_csv) {
        int needs_header = 1;

        FILE *existing =
            fopen(cfg->benchmark_csv, "rb");

        if (existing) {
            if (
                fseek(existing, 0, SEEK_END) == 0 &&
                ftell(existing) > 0
            )
                needs_header = 0;

            fclose(existing);
        }

        b.csv =
            fopen(
                cfg->benchmark_csv,
                "a"
            );

        if (!b.csv) {
            perror("benchmark CSV");
            pthread_mutex_destroy(&b.lock);
            return -1;
        }

        if (needs_header) {
            fprintf(
                b.csv,
                "elapsed_s,stage,square_px,frames,fps,"
                "dirty_mpix_s,dirty_pct_frame,"
                "render_avg_ms,render_max_ms,"
                "rfb_avg_ms,rfb_max_ms,"
                "late_frames,lateness_avg_ms,lateness_max_ms,"
                "backend_reads_s,backend_avg_read,"
"ra2_out_mbit_s,ra2_out_records_s,ra2_avg_record,coalesce_ratio,"
                "ra2_send_avg_ms,ra2_send_max_ms,"
                "ra2_in_kbit_s,ra2_in_records_s,"
                "ra2_record_cap,damage,width,height,target_fps\n"
            );

            fflush(b.csv);
        }
    }

    b.initialized = 1;
    return 0;
}

void
benchmark_shutdown(void)
{
    if (!b.initialized)
        return;

    pthread_mutex_lock(&b.lock);

    if (b.csv) {
        fflush(b.csv);
        fclose(b.csv);
        b.csv = NULL;
    }

    pthread_mutex_unlock(&b.lock);
    pthread_mutex_destroy(&b.lock);
    memset(&b, 0, sizeof(b));
}

void
benchmark_set_stage(
    const char *stage,
    int square_size)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    pthread_mutex_lock(&b.lock);

    snprintf(
        b.stage,
        sizeof(b.stage),
        "%s",
        stage ? stage : "unknown"
    );

    b.square_size = square_size;

    pthread_mutex_unlock(&b.lock);
}

void
benchmark_record_frame(
    uint64_t dirty_pixels,
    double render_ms,
    double rfb_process_ms,
    double frame_lateness_ms)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    pthread_mutex_lock(&b.lock);

    b.frames++;
    b.dirty_pixels += dirty_pixels;

    b.render_ms_sum += render_ms;

    if (render_ms > b.render_ms_max)
        b.render_ms_max = render_ms;

    b.rfb_ms_sum += rfb_process_ms;

    if (rfb_process_ms > b.rfb_ms_max)
        b.rfb_ms_max = rfb_process_ms;

    if (frame_lateness_ms > 0.0) {
        b.late_frames++;
        b.lateness_ms_sum += frame_lateness_ms;

        if (frame_lateness_ms > b.lateness_ms_max)
            b.lateness_ms_max = frame_lateness_ms;
    }

    pthread_mutex_unlock(&b.lock);
}

void
benchmark_record_backend_read(size_t bytes)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    pthread_mutex_lock(&b.lock);

    b.backend_reads++;
    b.backend_read_bytes += bytes;

    pthread_mutex_unlock(&b.lock);
}

void
benchmark_record_ra2_out(
    size_t payload_bytes,
    double send_ms)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    pthread_mutex_lock(&b.lock);

    b.ra2_out_records++;
    b.ra2_out_payload += payload_bytes;
    b.ra2_send_ms_sum += send_ms;

    if (send_ms > b.ra2_send_ms_max)
        b.ra2_send_ms_max = send_ms;

    pthread_mutex_unlock(&b.lock);
}

void
benchmark_record_ra2_in(
    size_t payload_bytes)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    pthread_mutex_lock(&b.lock);

    b.ra2_in_records++;
    b.ra2_in_payload += payload_bytes;

    pthread_mutex_unlock(&b.lock);
}

void
benchmark_maybe_report(void)
{
    if (
        !b.initialized ||
        !b.cfg ||
        b.cfg->benchmark_mode == BENCHMARK_OFF
    )
        return;

    double now = benchmark_now();

    pthread_mutex_lock(&b.lock);

    double interval =
        now - b.interval_started_at;

    if (
        interval <
        b.cfg->benchmark_interval_seconds
    ) {
        pthread_mutex_unlock(&b.lock);
        return;
    }

    double fps =
        interval > 0.0
            ? (double)b.frames / interval
            : 0.0;

    double dirty_mpix_s =
        interval > 0.0
            ? (double)b.dirty_pixels /
              interval /
              1000000.0
            : 0.0;

    double framebuffer_pixels =
        (double)b.cfg->width *
        (double)b.cfg->height;

    double dirty_pct_frame =
        b.frames > 0 &&
        framebuffer_pixels > 0.0
            ? (
                (double)b.dirty_pixels /
                (double)b.frames /
                framebuffer_pixels *
                100.0
            )
            : 0.0;

    double render_avg =
        b.frames
            ? b.render_ms_sum /
              (double)b.frames
            : 0.0;

    double rfb_avg =
        b.frames
            ? b.rfb_ms_sum /
              (double)b.frames
            : 0.0;

    double late_avg =
        b.late_frames
            ? b.lateness_ms_sum /
              (double)b.late_frames
            : 0.0;

    double backend_reads_s =
        interval > 0.0
            ? (double)b.backend_reads /
              interval
            : 0.0;

    double backend_avg_read =
        b.backend_reads
            ? (double)b.backend_read_bytes /
              (double)b.backend_reads
            : 0.0;

    double ra2_out_mbit_s =
        interval > 0.0
            ? (
                (double)b.ra2_out_payload *
                8.0 /
                interval /
                1000000.0
            )
            : 0.0;

    double ra2_out_records_s =
        interval > 0.0
            ? (double)b.ra2_out_records /
              interval
            : 0.0;

    double ra2_avg_record =
        b.ra2_out_records
            ? (double)b.ra2_out_payload /
              (double)b.ra2_out_records
            : 0.0;

    double coalesce_ratio =
        b.ra2_out_records
            ? (double)b.backend_reads /
              (double)b.ra2_out_records
            : 0.0;

    double ra2_send_avg =
        b.ra2_out_records
            ? b.ra2_send_ms_sum /
              (double)b.ra2_out_records
            : 0.0;

    double ra2_in_kbit_s =
        interval > 0.0
            ? (
                (double)b.ra2_in_payload *
                8.0 /
                interval /
                1000.0
            )
            : 0.0;

    double ra2_in_records_s =
        interval > 0.0
            ? (double)b.ra2_in_records /
              interval
            : 0.0;

    double elapsed =
        now - b.started_at;

    printf(
        "[BENCH] stage=%-18s sq=%4d "
        "srcfps=%5.1f dirty=%5.1f%% "
        "render=%5.2f/%5.2fms "
        "rfb=%5.2f/%5.2fms "
        "late=%llu "
        "bread=%7.1f/s bavg=%6.0fB "
        "out=%6.2fMbit/s rec=%6.1f/s avg=%6.0fB coal=%5.1fx "
        "send=%5.2f/%5.2fms\n",
        b.stage,
        b.square_size,
        fps,
        dirty_pct_frame,
        render_avg,
        b.render_ms_max,
        rfb_avg,
        b.rfb_ms_max,
        (unsigned long long)b.late_frames,
        backend_reads_s,
        backend_avg_read,
        ra2_out_mbit_s,
        ra2_out_records_s,
        ra2_avg_record,
        coalesce_ratio,
        ra2_send_avg,
        b.ra2_send_ms_max
    );

    if (b.csv) {
        fprintf(
            b.csv,
            "%.3f,%s,%d,%llu,%.3f,"
            "%.6f,%.3f,"
            "%.6f,%.6f,"
            "%.6f,%.6f,"
            "%llu,%.6f,%.6f,"
            "%.6f,%.3f,"
            "%.6f,%.6f,%.3f,%.3f,"
            "%.6f,%.6f,"
            "%.6f,%.6f,"
            "%d,%s,%d,%d,%d\n",
            elapsed,
            b.stage,
            b.square_size,
            (unsigned long long)b.frames,
            fps,
            dirty_mpix_s,
            dirty_pct_frame,
            render_avg,
            b.render_ms_max,
            rfb_avg,
            b.rfb_ms_max,
            (unsigned long long)b.late_frames,
            late_avg,
            b.lateness_ms_max,
            backend_reads_s,
            backend_avg_read,
            ra2_out_mbit_s,
            ra2_out_records_s,
            ra2_avg_record,
            coalesce_ratio,
            ra2_send_avg,
            b.ra2_send_ms_max,
            ra2_in_kbit_s,
            ra2_in_records_s,
            b.cfg->ra2_stream_record_max,
            runtime_config_damage_name(
                b.cfg->damage_mode
            ),
            b.cfg->width,
            b.cfg->height,
            b.cfg->max_fps
        );

        fflush(b.csv);
    }

    reset_interval_locked(now);

    pthread_mutex_unlock(&b.lock);
}
