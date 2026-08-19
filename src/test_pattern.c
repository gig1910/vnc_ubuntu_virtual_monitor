#include "test_pattern.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int old_x = 0;
static int old_y = 0;
static int old_size = 64;
static int initialized = 0;

static char stage_name[64] = "normal";

static void
put_bgrx(
    uint8_t *fb,
    int width,
    int height,
    int x,
    int y,
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    if (
        x < 0 || x >= width ||
        y < 0 || y >= height
    )
        return;

    uint8_t *p =
        fb + ((size_t)y * width + x) * 4;

    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 0;
}

static void
background_pixel(
    int width,
    int x,
    int y,
    uint8_t *r,
    uint8_t *g,
    uint8_t *b)
{
    static const uint8_t bars[8][3] = {
        {255,   0,   0},
        {  0, 255,   0},
        {  0,   0, 255},
        {255, 255,   0},
        {255,   0, 255},
        {  0, 255, 255},
        {255, 255, 255},
        { 32,  32,  32}
    };

    int index =
        x * 8 / width;

    if (index < 0) index = 0;
    if (index > 7) index = 7;

    *r = bars[index][0];
    *g = bars[index][1];
    *b = bars[index][2];

    if (((x / 32) + (y / 32)) & 1) {
        *r = (uint8_t)((unsigned)*r * 3 / 4);
        *g = (uint8_t)((unsigned)*g * 3 / 4);
        *b = (uint8_t)((unsigned)*b * 3 / 4);
    }
}

static void
restore_background_region(
    uint8_t *fb,
    int width,
    int height,
    int x1,
    int y1,
    int x2,
    int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > width) x2 = width;
    if (y2 > height) y2 = height;

    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            uint8_t r, g, b;

            background_pixel(
                width,
                x,
                y,
                &r,
                &g,
                &b
            );

            put_bgrx(
                fb,
                width,
                height,
                x,
                y,
                r,
                g,
                b
            );
        }
    }
}

static void
draw_square(
    uint8_t *fb,
    int width,
    int height,
    int sx,
    int sy,
    int size,
    unsigned int frame)
{
    for (int y = sy; y < sy + size; y++) {
        for (int x = sx; x < sx + size; x++) {
            int lx = x - sx;
            int ly = y - sy;

            uint8_t r =
                (uint8_t)(
                    (frame * 5 + lx * 3) & 0xff
                );

            uint8_t g =
                (uint8_t)(
                    (frame * 3 + ly * 5) & 0xff
                );

            uint8_t b =
                (uint8_t)(
                    (255 - frame * 2 + lx + ly) & 0xff
                );

            if (
                (lx % 16) == 0 ||
                (ly % 16) == 0
            ) {
                r = g = b = 0;
            }

            put_bgrx(
                fb,
                width,
                height,
                x,
                y,
                r,
                g,
                b
            );
        }
    }
}

static void
render_full_solid(
    uint8_t *fb,
    int width,
    int height,
    unsigned int frame)
{
    uint8_t r =
        (uint8_t)(frame * 3);

    uint8_t g =
        (uint8_t)(frame * 5);

    uint8_t b =
        (uint8_t)(frame * 7);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            put_bgrx(
                fb,
                width,
                height,
                x,
                y,
                r,
                g,
                b
            );
        }
    }
}

static uint32_t
xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void
render_full_noise(
    uint8_t *fb,
    int width,
    int height,
    unsigned int frame)
{
    uint32_t state =
        0x9e3779b9u ^
        (frame * 0x85ebca6bu);

    size_t pixels =
        (size_t)width *
        (size_t)height;

    for (size_t i = 0; i < pixels; i++) {
        uint32_t v = xorshift32(&state);

        fb[i * 4 + 0] =
            (uint8_t)(v);

        fb[i * 4 + 1] =
            (uint8_t)(v >> 8);

        fb[i * 4 + 2] =
            (uint8_t)(v >> 16);

        fb[i * 4 + 3] = 0;
    }
}

static int
clamp_square(
    int value,
    int width,
    int height)
{
    int max_allowed =
        width < height
            ? width
            : height;

    if (value < 1)
        value = 1;

    if (value > max_allowed)
        value = max_allowed;

    return value;
}

static int
benchmark_square_size(
    const RuntimeConfig *cfg,
    double elapsed)
{
    int min_size =
        clamp_square(
            cfg->benchmark_square_min,
            cfg->width,
            cfg->height
        );

    int max_size =
        clamp_square(
            cfg->benchmark_square_max,
            cfg->width,
            cfg->height
        );

    if (max_size < min_size)
        max_size = min_size;

    int step =
        cfg->benchmark_square_step;

    if (step < 1)
        step = 1;

    int count =
        1 +
        (max_size - min_size) /
        step;

    if (count < 1)
        count = 1;

    unsigned long long stage =
        (unsigned long long)(
            elapsed /
            cfg->benchmark_step_seconds
        );

    int index =
        (int)(stage % (unsigned long long)count);

    return min_size + index * step;
}

static void
render_moving_square(
    uint8_t *fb,
    const RuntimeConfig *cfg,
    double elapsed,
    unsigned int frame,
    int square_size,
    TestPatternFrame *out)
{
    int width = cfg->width;
    int height = cfg->height;

    int new_size =
        clamp_square(
            square_size,
            width,
            height
        );

    int move_range =
        width - new_size;

    int cycle =
        move_range * 2;

    int move_phase =
        cycle > 0
            ? (int)(
                elapsed *
                cfg->square_speed
            ) % cycle
            : 0;

    int new_x =
        move_phase <= move_range
            ? move_phase
            : cycle - move_phase;

    int new_y =
        (height - new_size) / 2;

    restore_background_region(
        fb,
        width,
        height,
        old_x,
        old_y,
        old_x + old_size,
        old_y + old_size
    );

    draw_square(
        fb,
        width,
        height,
        new_x,
        new_y,
        new_size,
        frame
    );

    int old_x2 =
        old_x + old_size;

    int old_y2 =
        old_y + old_size;

    int new_x2 =
        new_x + new_size;

    int new_y2 =
        new_y + new_size;

    out->dirty_x1 =
        old_x < new_x
            ? old_x
            : new_x;

    out->dirty_y1 =
        old_y < new_y
            ? old_y
            : new_y;

    out->dirty_x2 =
        old_x2 > new_x2
            ? old_x2
            : new_x2;

    out->dirty_y2 =
        old_y2 > new_y2
            ? old_y2
            : new_y2;

    out->square_size = new_size;

    old_x = new_x;
    old_y = new_y;
    old_size = new_size;
}

void
test_pattern_reset(void)
{
    initialized = 0;
    old_x = 0;
    old_y = 0;
    old_size = 64;
    snprintf(stage_name, sizeof(stage_name), "normal");
}

void
test_pattern_init(
    uint8_t *fb,
    const RuntimeConfig *cfg)
{
    restore_background_region(
        fb,
        cfg->width,
        cfg->height,
        0,
        0,
        cfg->width,
        cfg->height
    );

    old_size =
        clamp_square(
            cfg->square_min,
            cfg->width,
            cfg->height
        );

    old_x = 0;
    old_y =
        (cfg->height - old_size) / 2;

    draw_square(
        fb,
        cfg->width,
        cfg->height,
        old_x,
        old_y,
        old_size,
        0
    );

    initialized = 1;
}

void
test_pattern_render(
    uint8_t *fb,
    const RuntimeConfig *cfg,
    double elapsed,
    unsigned int frame,
    TestPatternFrame *out)
{
    if (!initialized)
        test_pattern_init(fb, cfg);

    memset(out, 0, sizeof(*out));

    if (
        cfg->benchmark_mode ==
        BENCHMARK_SUITE
    ) {
        int min_size =
            clamp_square(
                cfg->benchmark_square_min,
                cfg->width,
                cfg->height
            );

        int max_size =
            clamp_square(
                cfg->benchmark_square_max,
                cfg->width,
                cfg->height
            );

        int step =
            cfg->benchmark_square_step > 0
                ? cfg->benchmark_square_step
                : 1;

        int square_count =
            1 +
            (max_size - min_size) /
            step;

        if (square_count < 1)
            square_count = 1;

        unsigned long long stage =
            (unsigned long long)(
                elapsed /
                cfg->benchmark_step_seconds
            );

        unsigned long long total =
            (unsigned long long)square_count +
            2ULL;

        unsigned long long phase =
            stage % total;

        if (
            phase <
            (unsigned long long)square_count
        ) {
            int size =
                min_size +
                (int)phase * step;

            snprintf(
                stage_name,
                sizeof(stage_name),
                "square-%d",
                size
            );

            render_moving_square(
                fb,
                cfg,
                elapsed,
                frame,
                size,
                out
            );
        }
        else if (
            phase ==
            (unsigned long long)square_count
        ) {
            snprintf(
                stage_name,
                sizeof(stage_name),
                "full-solid"
            );

            render_full_solid(
                fb,
                cfg->width,
                cfg->height,
                frame
            );

            out->dirty_x1 = 0;
            out->dirty_y1 = 0;
            out->dirty_x2 = cfg->width;
            out->dirty_y2 = cfg->height;
            out->square_size = 0;
        }
        else {
            snprintf(
                stage_name,
                sizeof(stage_name),
                "full-noise"
            );

            render_full_noise(
                fb,
                cfg->width,
                cfg->height,
                frame
            );

            out->dirty_x1 = 0;
            out->dirty_y1 = 0;
            out->dirty_x2 = cfg->width;
            out->dirty_y2 = cfg->height;
            out->square_size = 0;
        }
    }
    else if (
        cfg->benchmark_mode ==
        BENCHMARK_SQUARE_SWEEP
    ) {
        int size =
            benchmark_square_size(
                cfg,
                elapsed
            );

        snprintf(
            stage_name,
            sizeof(stage_name),
            "square-%d",
            size
        );

        render_moving_square(
            fb,
            cfg,
            elapsed,
            frame,
            size,
            out
        );
    }
    else {
        double phase =
            elapsed /
            cfg->square_cycle_seconds;

        phase -= (long long)phase;

        double triangle =
            phase < 0.5
                ? phase * 2.0
                : 2.0 - phase * 2.0;

        int min_size =
            clamp_square(
                cfg->square_min,
                cfg->width,
                cfg->height
            );

        int max_size =
            clamp_square(
                cfg->square_max,
                cfg->width,
                cfg->height
            );

        if (max_size < min_size)
            max_size = min_size;

        int size =
            min_size +
            (int)(
                triangle *
                (max_size - min_size)
            );

        snprintf(
            stage_name,
            sizeof(stage_name),
            "normal"
        );

        render_moving_square(
            fb,
            cfg,
            elapsed,
            frame,
            size,
            out
        );
    }

    if (cfg->damage_mode == DAMAGE_FULL) {
        out->dirty_x1 = 0;
        out->dirty_y1 = 0;
        out->dirty_x2 = cfg->width;
        out->dirty_y2 = cfg->height;
    }

    out->stage = stage_name;
}
