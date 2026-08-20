    pthread_mutex_lock(&state->mutex);
    sidecar->reference_client = state->jpeg_client;
    sidecar->reference_valid =
        state->jpeg_supported && state->jpeg_client;
    pthread_mutex_unlock(&state->mutex);
}

static int
copy_patch_matches(
    AdaptiveRfbState *state,
    CopySidecar *sidecar,
    int current_x,
    int current_y,
    int reference_x,
    int reference_y)
{
    const int patch = COPY_PATCH_SIZE;
    const uint8_t *current =
        (const uint8_t *)state->screen->frameBuffer;

    for (int row = 0; row < patch; row++) {
        const uint8_t *a =
            current +
            (size_t)(current_y + row) *
                (size_t)state->screen->paddedWidthInBytes +
            (size_t)current_x * 4;
        const uint8_t *b =
            sidecar->reference +
            ((size_t)(reference_y + row) *
                (size_t)state->width +
             (size_t)reference_x) * 4;

        if (memcmp(a, b, (size_t)patch * 4) != 0)
            return 0;
    }

    return 1;
}

static int
copy_tile_matches_shift(
    AdaptiveRfbState *state,
    CopySidecar *sidecar,
    int x,
    int y,
    int dx,
    int dy)
{
    const int tile = COPY_VERIFY_TILE;
    int source_x = x - dx;
    int source_y = y - dy;

    if (
        source_x < 0 ||
        source_y < 0 ||
        source_x + tile > state->width ||
        source_y + tile > state->height
    )
        return 0;

    const uint8_t *current =
        (const uint8_t *)state->screen->frameBuffer;

    for (int row = 0; row < tile; row++) {
        const uint8_t *a =
            current +
            (size_t)(y + row) *
                (size_t)state->screen->paddedWidthInBytes +
            (size_t)x * 4;
        const uint8_t *b =
            sidecar->reference +
            ((size_t)(source_y + row) *
                (size_t)state->width +
             (size_t)source_x) * 4;

        if (memcmp(a, b, (size_t)tile * 4) != 0)
            return 0;
    }

    return 1;
}

static int
copy_largest_rect_for_shift(
    AdaptiveRfbState *state,
    CopySidecar *sidecar,
    const CopyRectBox *pending,
    int dx,
    int dy,
    CopyRectBox *copy_out,
    uint64_t *pixels_out)
{
    const int tile = COPY_VERIFY_TILE;

    int tile_x1 = (pending->x1 + tile - 1) / tile;
    int tile_y1 = (pending->y1 + tile - 1) / tile;
    int tile_x2 = pending->x2 / tile;
    int tile_y2 = pending->y2 / tile;

    int columns = tile_x2 - tile_x1;
    int rows = tile_y2 - tile_y1;

    if (columns <= 0 || rows <= 0)
        return 0;

    int *heights = calloc((size_t)columns, sizeof(*heights));
    if (!heights)
        return 0;

    uint64_t best_tiles = 0;
    int best_left = 0;
    int best_right = 0;
    int best_bottom = 0;
    int best_height = 0;

    for (int row = 0; row < rows; row++) {
        int y = (tile_y1 + row) * tile;

        for (int column = 0; column < columns; column++) {
            int x = (tile_x1 + column) * tile;

            if (
                copy_tile_matches_shift(
                    state,
                    sidecar,
                    x,
                    y,
                    dx,
                    dy
                )
            )
                heights[column]++;
            else
                heights[column] = 0;
        }

        for (int left = 0; left < columns; left++) {
            int min_height = INT_MAX;

            for (int right = left; right < columns; right++) {
                if (heights[right] < min_height)
                    min_height = heights[right];

                if (min_height <= 0)
                    break;

                uint64_t area_tiles =
                    (uint64_t)min_height *
                    (uint64_t)(right - left + 1);

                if (area_tiles > best_tiles) {
                    best_tiles = area_tiles;
                    best_left = left;
                    best_right = right + 1;
                    best_bottom = row + 1;
                    best_height = min_height;
                }
            }
        }
    }

    free(heights);

    if (best_tiles == 0)
        return 0;

    CopyRectBox copy = {
        .x1 = (tile_x1 + best_left) * tile,
        .y1 = (tile_y1 + best_bottom - best_height) * tile,
        .x2 = (tile_x1 + best_right) * tile,
        .y2 = (tile_y1 + best_bottom) * tile
    };

    uint64_t copy_pixels = copy_box_area(&copy);
    uint64_t pending_pixels = copy_box_area(pending);
    double share =
        pending_pixels > 0
            ? (double)copy_pixels * 100.0 /
                (double)pending_pixels
            : 0.0;
