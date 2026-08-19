#ifndef VNC_MONITOR_FRAME_DIFF_H
#define VNC_MONITOR_FRAME_DIFF_H

#include <stddef.h>
#include <stdint.h>

#include "frame_bridge.h"

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
} FrameDiffRect;

/*
 * Compare the newest FrameBridge image against the destination VNC
 * framebuffer tile-by-tile.
 *
 * Only changed tiles are copied into destination. Adjacent horizontal runs
 * are merged, and identical runs on consecutive tile rows are merged
 * vertically.
 *
 * Returns:
 *   >0  number of diff rectangles
 *    0  no new/changed pixels
 *   -1  error
 */
int frame_diff_consume(
    FrameBridge *bridge,
    uint8_t *destination,
    uint64_t *last_sequence,
    int tile_size,
    FrameDiffRect *rects,
    size_t max_rects);

#endif
