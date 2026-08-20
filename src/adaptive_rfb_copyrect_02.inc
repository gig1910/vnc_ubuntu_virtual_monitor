
    if (
        copy_pixels < COPY_MIN_PIXELS ||
        share < COPY_MIN_SHARE_PERCENT
    )
        return 0;

    if (copy_out)
        *copy_out = copy;
    if (pixels_out)
        *pixels_out = copy_pixels;

    return 1;
}

static void
copy_append_residual(
    CopyPlan *plan,
    int x1,
    int y1,
    int x2,
    int y2)
{
    if (
        !plan ||
        plan->jpeg_count >= 4 ||
        x2 <= x1 ||
        y2 <= y1
    )
        return;

    CopyRectBox *rect =
        &plan->jpeg[plan->jpeg_count++];

    rect->x1 = x1;
    rect->y1 = y1;
    rect->x2 = x2;
    rect->y2 = y2;
    plan->jpeg_pixels += copy_box_area(rect);
}

static void
copy_build_residuals(
    CopyPlan *plan,
    const CopyRectBox *pending)
{
    const CopyRectBox *copy = &plan->copy;

    plan->jpeg_count = 0;
    plan->jpeg_pixels = 0;

    copy_append_residual(
        plan,
        pending->x1,
        pending->y1,
        pending->x2,
        copy->y1
    );
    copy_append_residual(
        plan,
        pending->x1,
        copy->y2,
        pending->x2,
        pending->y2
    );
    copy_append_residual(
        plan,
        pending->x1,
        copy->y1,
        copy->x1,
        copy->y2
    );
    copy_append_residual(
        plan,
        copy->x2,
        copy->y1,
        pending->x2,
        copy->y2
    );
}

static int
copy_candidate_seen(
    const int *dxs,
    const int *dys,
    int count,
    int dx,
    int dy)
{
    for (int i = 0; i < count; i++) {
        if (dxs[i] == dx && dys[i] == dy)
            return 1;
    }

    return 0;
}

static int
copy_find_plan(
    AdaptiveRfbState *state,
    CopySidecar *sidecar,
    const CopyRectBox *pending,
    CopyPlan *plan)
{
    if (
        !state ||
        !sidecar ||
        !pending ||
        !plan ||
        !copy_reference_is_valid(state, sidecar)
    )
        return 0;

    memset(plan, 0, sizeof(*plan));

    if (copy_box_area(pending) < COPY_MIN_PIXELS)
        return 0;

    const uint8_t *current =
        (const uint8_t *)state->screen->frameBuffer;
    const int patch = COPY_PATCH_SIZE;

    static const int anchors[][2] = {
        {2, 2},
        {1, 1},
        {3, 1},
        {1, 3},
        {3, 3}
    };

    int candidate_dxs[COPY_MAX_CANDIDATES];
    int candidate_dys[COPY_MAX_CANDIDATES];
    int candidate_count = 0;

    for (
        size_t anchor_index = 0;
        anchor_index < sizeof(anchors) / sizeof(anchors[0]);
        anchor_index++
    ) {
        int current_x =
            pending->x1 +
            (pending->x2 - pending->x1) *
                anchors[anchor_index][0] / 4;
        int current_y =
            pending->y1 +
            (pending->y2 - pending->y1) *
                anchors[anchor_index][1] / 4;

        if (current_x + patch > pending->x2)
            current_x = pending->x2 - patch;
        if (current_y + patch > pending->y2)
            current_y = pending->y2 - patch;
        if (current_x < pending->x1 || current_y < pending->y1)
            continue;

        int source_x1 = current_x - COPY_SEARCH_RADIUS;
        int source_y1 = current_y - COPY_SEARCH_RADIUS;
        int source_x2 = current_x + COPY_SEARCH_RADIUS;
        int source_y2 = current_y + COPY_SEARCH_RADIUS;

        if (source_x1 < 0)
            source_x1 = 0;
        if (source_y1 < 0)
            source_y1 = 0;
        if (source_x2 > state->width - patch)
            source_x2 = state->width - patch;
        if (source_y2 > state->height - patch)
            source_y2 = state->height - patch;

        const uint32_t *current_first =
            (const uint32_t *)(
                current +
                (size_t)current_y *
                    (size_t)state->screen->paddedWidthInBytes +
                (size_t)current_x * 4
            );
        const uint32_t *current_last =
            (const uint32_t *)(
                current +
                (size_t)(current_y + patch - 1) *
