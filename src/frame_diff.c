#include "frame_diff.h"

#include <stdlib.h>
#include <string.h>

static int
tile_changed(
    const uint8_t *source,
    const uint8_t *destination,
    int frame_width,
    int x1,
    int y1,
    int x2,
    int y2)
{
    size_t row_bytes =
        (size_t)(x2 - x1) * 4;

    for (int y = y1; y < y2; y++) {
        const uint8_t *src =
            source +
            (
                (size_t)y *
                (size_t)frame_width +
                (size_t)x1
            ) * 4;

        const uint8_t *dst =
            destination +
            (
                (size_t)y *
                (size_t)frame_width +
                (size_t)x1
            ) * 4;

        if (
            memcmp(
                src,
                dst,
                row_bytes
            ) != 0
        )
            return 1;
    }

    return 0;
}

static void
copy_tile(
    const uint8_t *source,
    uint8_t *destination,
    int frame_width,
    int x1,
    int y1,
    int x2,
    int y2)
{
    size_t row_bytes =
        (size_t)(x2 - x1) * 4;

    for (int y = y1; y < y2; y++) {
        const uint8_t *src =
            source +
            (
                (size_t)y *
                (size_t)frame_width +
                (size_t)x1
            ) * 4;

        uint8_t *dst =
            destination +
            (
                (size_t)y *
                (size_t)frame_width +
                (size_t)x1
            ) * 4;

        memcpy(
            dst,
            src,
            row_bytes
        );
    }
}

int
frame_diff_consume(
    FrameBridge *bridge,
    uint8_t *destination,
    uint64_t *last_sequence,
    int tile_size,
    FrameDiffRect *rects,
    size_t max_rects)
{
    if (
        !bridge ||
        !bridge->initialized ||
        !destination ||
        !last_sequence ||
        !rects ||
        max_rects == 0 ||
        tile_size <= 0
    )
        return -1;

    pthread_mutex_lock(
        &bridge->mutex
    );

    if (
        *last_sequence ==
        bridge->sequence
    ) {
        pthread_mutex_unlock(
            &bridge->mutex
        );

        return 0;
    }

    int width = bridge->width;
    int height = bridge->height;

    int tiles_x =
        (width + tile_size - 1) /
        tile_size;

    int tiles_y =
        (height + tile_size - 1) /
        tile_size;

    size_t tile_count =
        (size_t)tiles_x *
        (size_t)tiles_y;

    uint8_t *changed =
        calloc(
            tile_count,
            1
        );

    if (!changed) {
        pthread_mutex_unlock(
            &bridge->mutex
        );

        return -1;
    }

    for (int ty = 0; ty < tiles_y; ty++) {
        int y1 = ty * tile_size;
        int y2 = y1 + tile_size;

        if (y2 > height)
            y2 = height;

        for (int tx = 0; tx < tiles_x; tx++) {
            int x1 = tx * tile_size;
            int x2 = x1 + tile_size;

            if (x2 > width)
                x2 = width;

            if (
                tile_changed(
                    bridge->pixels,
                    destination,
                    width,
                    x1,
                    y1,
                    x2,
                    y2
                )
            ) {
                changed[
                    (size_t)ty *
                    (size_t)tiles_x +
                    (size_t)tx
                ] = 1;

                copy_tile(
                    bridge->pixels,
                    destination,
                    width,
                    x1,
                    y1,
                    x2,
                    y2
                );
            }
        }
    }

    *last_sequence =
        bridge->sequence;

    /*
     * First form horizontal runs. Then merge with a rectangle from the
     * previous tile row when x1/x2 are identical.
     */
    size_t rect_count = 0;

    for (int ty = 0; ty < tiles_y; ty++) {
        int tx = 0;

        while (tx < tiles_x) {
            size_t index =
                (size_t)ty *
                (size_t)tiles_x +
                (size_t)tx;

            if (!changed[index]) {
                tx++;
                continue;
            }

            int run_start = tx;

            while (
                tx + 1 < tiles_x &&
                changed[
                    (size_t)ty *
                    (size_t)tiles_x +
                    (size_t)(tx + 1)
                ]
            )
                tx++;

            int run_end = tx + 1;

            FrameDiffRect candidate = {
                .x1 = run_start * tile_size,
                .y1 = ty * tile_size,
                .x2 = run_end * tile_size,
                .y2 = (ty + 1) * tile_size
            };

            if (candidate.x2 > width)
                candidate.x2 = width;

            if (candidate.y2 > height)
                candidate.y2 = height;

            int merged = 0;

            for (size_t i = 0; i < rect_count; i++) {
                if (
                    rects[i].x1 ==
                        candidate.x1 &&
                    rects[i].x2 ==
                        candidate.x2 &&
                    rects[i].y2 ==
                        candidate.y1
                ) {
                    rects[i].y2 =
                        candidate.y2;

                    merged = 1;
                    break;
                }
            }

            if (!merged) {
                if (
                    rect_count >=
                    max_rects
                ) {
                    /*
                     * Safe fallback: one full-frame rectangle. Pixels have
                     * already been copied only where changed, but asking VNC
                     * for the full frame is always correct.
                     */
                    rects[0] =
                        (FrameDiffRect) {
                            .x1 = 0,
                            .y1 = 0,
                            .x2 = width,
                            .y2 = height
                        };

                    rect_count = 1;
                    goto done;
                }

                rects[rect_count++] =
                    candidate;
            }

            tx++;
        }
    }

done:
    free(changed);

    pthread_mutex_unlock(
        &bridge->mutex
    );

    return (int)rect_count;
}
