/*
 * CopyRect acceleration is intentionally part of the same translation unit as
 * adaptive_rfb.c because it reuses the transport's private client/request and
 * JPEG helpers. The implementation keeps a reference to the last framebuffer
 * state actually sent to the client and only emits CopyRect after byte-exact
 * verification of 32x32 tiles.
 */

#define COPY_SEARCH_RADIUS        256
#define COPY_PATCH_SIZE           8
#define COPY_VERIFY_TILE          32
#define COPY_MIN_PIXELS           32768ULL
#define COPY_MIN_SHARE_PERCENT    20.0
#define COPY_MAX_CANDIDATES       32

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
} CopyRectBox;

typedef struct {
    int dx;
    int dy;
    CopyRectBox copy;
    CopyRectBox jpeg[4];
    int jpeg_count;
    uint64_t copy_pixels;
    uint64_t jpeg_pixels;
} CopyPlan;

typedef struct {
    CopyRectBox rect;
    unsigned char *data;
    unsigned long size;
} EncodedRect;

typedef struct CopySidecar {
    AdaptiveRfbState *state;
    uint8_t *reference;
    int reference_valid;
    rfbClientPtr reference_client;
    uint64_t copy_updates;
    uint64_t copy_pixels;
    uint64_t residual_pixels;
    uint64_t residual_jpeg_bytes;
    struct CopySidecar *next;
} CopySidecar;

static pthread_mutex_t copy_sidecars_mutex = PTHREAD_MUTEX_INITIALIZER;
static CopySidecar *copy_sidecars = NULL;

static uint64_t
copy_box_area(const CopyRectBox *box)
{
    if (!box || box->x2 <= box->x1 || box->y2 <= box->y1)
        return 0;

    return (uint64_t)(box->x2 - box->x1) *
           (uint64_t)(box->y2 - box->y1);
}

static CopySidecar *
copy_sidecar_find(AdaptiveRfbState *state)
{
    CopySidecar *found = NULL;

    pthread_mutex_lock(&copy_sidecars_mutex);
    for (CopySidecar *it = copy_sidecars; it; it = it->next) {
        if (it->state == state) {
            found = it;
            break;
        }
    }
    pthread_mutex_unlock(&copy_sidecars_mutex);
    return found;
}

static int
copy_sidecar_add(AdaptiveRfbState *state)
{
    CopySidecar *sidecar = calloc(1, sizeof(*sidecar));
    if (!sidecar)
        return -1;

    sidecar->reference =
        malloc((size_t)state->width * (size_t)state->height * 4);
    if (!sidecar->reference) {
        free(sidecar);
        return -1;
    }

    sidecar->state = state;

    pthread_mutex_lock(&copy_sidecars_mutex);
    sidecar->next = copy_sidecars;
    copy_sidecars = sidecar;
    pthread_mutex_unlock(&copy_sidecars_mutex);
    return 0;
}

static void
copy_sidecar_remove(AdaptiveRfbState *state)
{
    CopySidecar *removed = NULL;

    pthread_mutex_lock(&copy_sidecars_mutex);
    CopySidecar **cursor = &copy_sidecars;
    while (*cursor) {
        if ((*cursor)->state == state) {
            removed = *cursor;
            *cursor = removed->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&copy_sidecars_mutex);

    if (removed) {
        free(removed->reference);
        free(removed);
    }
}

static int
copy_reference_is_valid(AdaptiveRfbState *state, CopySidecar *sidecar)
{
    rfbClientPtr client = NULL;
    int supported = 0;

    pthread_mutex_lock(&state->mutex);
    client = state->jpeg_client;
    supported = state->jpeg_supported;
    pthread_mutex_unlock(&state->mutex);

    if (!supported || !client || !sidecar->reference_valid ||
        sidecar->reference_client != client) {
        sidecar->reference_valid = 0;
        sidecar->reference_client = client;
        return 0;
    }

    return 1;
}

static void
copy_reference_snapshot(AdaptiveRfbState *state, CopySidecar *sidecar)
{
    if (!state || !sidecar || !sidecar->reference || !state->screen ||
        !state->screen->frameBuffer)
        return;

    const uint8_t *source = (const uint8_t *)state->screen->frameBuffer;

    for (int y = 0; y < state->height; y++) {
        memcpy(sidecar->reference +
                   (size_t)y * (size_t)state->width * 4,
               source +
                   (size_t)y * (size_t)state->screen->paddedWidthInBytes,
               (size_t)state->width * 4);
    }

    pthread_mutex_lock(&state->mutex);
    sidecar->reference_client = state->jpeg_client;
    sidecar->reference_valid = state->jpeg_supported && state->jpeg_client;
    pthread_mutex_unlock(&state->mutex);
}

static int
copy_patch_matches(AdaptiveRfbState *state,
                   CopySidecar *sidecar,
                   int current_x,
                   int current_y,
                   int reference_x,
                   int reference_y)
{
    const int patch = COPY_PATCH_SIZE;
    const uint8_t *current = (const uint8_t *)state->screen->frameBuffer;

    for (int row = 0; row < patch; row++) {
        const uint8_t *a =
            current +
            (size_t)(current_y + row) *
                (size_t)state->screen->paddedWidthInBytes +
            (size_t)current_x * 4;
        const uint8_t *b =
            sidecar->reference +
            ((size_t)(reference_y + row) * (size_t)state->width +
             (size_t)reference_x) * 4;

        if (memcmp(a, b, (size_t)patch * 4) != 0)
            return 0;
    }

    return 1;
}

static int
copy_tile_matches_shift(AdaptiveRfbState *state,
                        CopySidecar *sidecar,
                        int x,
                        int y,
                        int dx,
                        int dy)
{
    const int tile = COPY_VERIFY_TILE;
    int source_x = x - dx;
    int source_y = y - dy;

    if (source_x < 0 || source_y < 0 ||
        source_x + tile > state->width ||
        source_y + tile > state->height)
        return 0;

    const uint8_t *current = (const uint8_t *)state->screen->frameBuffer;

    for (int row = 0; row < tile; row++) {
        const uint8_t *a =
            current +
            (size_t)(y + row) *
                (size_t)state->screen->paddedWidthInBytes +
            (size_t)x * 4;
        const uint8_t *b =
            sidecar->reference +
            ((size_t)(source_y + row) * (size_t)state->width +
             (size_t)source_x) * 4;

        if (memcmp(a, b, (size_t)tile * 4) != 0)
            return 0;
    }

    return 1;
}

static int
copy_largest_rect_for_shift(AdaptiveRfbState *state,
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
            if (copy_tile_matches_shift(state, sidecar, x, y, dx, dy))
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
                    (uint64_t)min_height * (uint64_t)(right - left + 1);
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
    double share = pending_pixels > 0
        ? (double)copy_pixels * 100.0 / (double)pending_pixels
        : 0.0;

    if (copy_pixels < COPY_MIN_PIXELS || share < COPY_MIN_SHARE_PERCENT)
        return 0;

    if (copy_out)
        *copy_out = copy;
    if (pixels_out)
        *pixels_out = copy_pixels;
    return 1;
}

static void
copy_append_residual(CopyPlan *plan,
                     int x1,
                     int y1,
                     int x2,
                     int y2)
{
    if (!plan || plan->jpeg_count >= 4 || x2 <= x1 || y2 <= y1)
        return;

    CopyRectBox *rect = &plan->jpeg[plan->jpeg_count++];
    rect->x1 = x1;
    rect->y1 = y1;
    rect->x2 = x2;
    rect->y2 = y2;
    plan->jpeg_pixels += copy_box_area(rect);
}

static void
copy_build_residuals(CopyPlan *plan, const CopyRectBox *pending)
{
    const CopyRectBox *copy = &plan->copy;
    plan->jpeg_count = 0;
    plan->jpeg_pixels = 0;

    copy_append_residual(plan,
                         pending->x1,
                         pending->y1,
                         pending->x2,
                         copy->y1);
    copy_append_residual(plan,
                         pending->x1,
                         copy->y2,
                         pending->x2,
                         pending->y2);
    copy_append_residual(plan,
                         pending->x1,
                         copy->y1,
                         copy->x1,
                         copy->y2);
    copy_append_residual(plan,
                         copy->x2,
                         copy->y1,
                         pending->x2,
                         copy->y2);
}

static int
copy_candidate_seen(const int *dxs,
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
copy_find_plan(AdaptiveRfbState *state,
               CopySidecar *sidecar,
               const CopyRectBox *pending,
               CopyPlan *plan)
{
    if (!state || !sidecar || !pending || !plan ||
        !copy_reference_is_valid(state, sidecar))
        return 0;

    memset(plan, 0, sizeof(*plan));
    if (copy_box_area(pending) < COPY_MIN_PIXELS)
        return 0;

    const uint8_t *current = (const uint8_t *)state->screen->frameBuffer;
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

    for (size_t anchor_index = 0;
         anchor_index < sizeof(anchors) / sizeof(anchors[0]);
         anchor_index++) {
        int current_x =
            pending->x1 +
            (pending->x2 - pending->x1) * anchors[anchor_index][0] / 4;
        int current_y =
            pending->y1 +
            (pending->y2 - pending->y1) * anchors[anchor_index][1] / 4;

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
                (size_t)current_x * 4);
        const uint32_t *current_last =
            (const uint32_t *)(
                current +
                (size_t)(current_y + patch - 1) *
                    (size_t)state->screen->paddedWidthInBytes +
                (size_t)current_x * 4);

        uint32_t signature0 = current_first[0];
        uint32_t signature1 = current_first[patch - 1];
        uint32_t signature2 = current_last[0];
        uint32_t signature3 = current_last[patch - 1];

        for (int source_y = source_y1; source_y <= source_y2; source_y++) {
            for (int source_x = source_x1; source_x <= source_x2; source_x++) {
                int dx = current_x - source_x;
                int dy = current_y - source_y;

                if (dx == 0 && dy == 0)
                    continue;

                if (copy_candidate_seen(candidate_dxs,
                                        candidate_dys,
                                        candidate_count,
                                        dx,
                                        dy))
                    continue;

                const uint32_t *reference_first =
                    (const uint32_t *)(
                        sidecar->reference +
                        ((size_t)source_y * (size_t)state->width +
                         (size_t)source_x) * 4);
                const uint32_t *reference_last =
                    (const uint32_t *)(
                        sidecar->reference +
                        ((size_t)(source_y + patch - 1) *
                            (size_t)state->width +
                         (size_t)source_x) * 4);

                if (reference_first[0] != signature0 ||
                    reference_first[patch - 1] != signature1 ||
                    reference_last[0] != signature2 ||
                    reference_last[patch - 1] != signature3)
                    continue;

                if (!copy_patch_matches(state,
                                        sidecar,
                                        current_x,
                                        current_y,
                                        source_x,
                                        source_y))
                    continue;

                if (candidate_count < COPY_MAX_CANDIDATES) {
                    candidate_dxs[candidate_count] = dx;
                    candidate_dys[candidate_count] = dy;
                    candidate_count++;
                }

                CopyRectBox copy;
                uint64_t copy_pixels = 0;

                if (copy_largest_rect_for_shift(state,
                                                sidecar,
                                                pending,
                                                dx,
                                                dy,
                                                &copy,
                                                &copy_pixels)) {
                    plan->dx = dx;
                    plan->dy = dy;
                    plan->copy = copy;
                    plan->copy_pixels = copy_pixels;
                    copy_build_residuals(plan, pending);
                    return 1;
                }

                if (candidate_count >= COPY_MAX_CANDIDATES)
                    return 0;
            }
        }
    }

    return 0;
}

static void
copy_free_encoded(EncodedRect *encoded, int count)
{
    for (int i = 0; i < count; i++) {
        free(encoded[i].data);
        encoded[i].data = NULL;
        encoded[i].size = 0;
    }
}

static int
copy_encode_residuals(rfbScreenInfoPtr screen,
                      const CopyPlan *plan,
                      EncodedRect *encoded,
                      unsigned long *total_size)
{
    *total_size = 0;

    for (int i = 0; i < plan->jpeg_count; i++) {
        encoded[i].rect = plan->jpeg[i];
        encoded[i].data = NULL;
        encoded[i].size = 0;

        if (encode_jpeg_rect(screen,
                             encoded[i].rect.x1,
                             encoded[i].rect.y1,
                             encoded[i].rect.x2 - encoded[i].rect.x1,
                             encoded[i].rect.y2 - encoded[i].rect.y1,
                             &encoded[i].data,
                             &encoded[i].size) < 0 ||
            encoded[i].size > (unsigned long)INT_MAX) {
            copy_free_encoded(encoded, i + 1);
            return -1;
        }

        *total_size += encoded[i].size;
    }

    return 0;
}

static int
copy_write_copyrect(rfbClientPtr cl, const CopyPlan *plan)
{
    rfbFramebufferUpdateRectHeader header;
    rfbCopyRect copy;
    memset(&header, 0, sizeof(header));
    memset(&copy, 0, sizeof(copy));

    header.r.x = Swap16IfLE((uint16_t)plan->copy.x1);
    header.r.y = Swap16IfLE((uint16_t)plan->copy.y1);
    header.r.w = Swap16IfLE((uint16_t)(plan->copy.x2 - plan->copy.x1));
    header.r.h = Swap16IfLE((uint16_t)(plan->copy.y2 - plan->copy.y1));
    header.encoding = Swap32IfLE(rfbEncodingCopyRect);

    copy.srcX = Swap16IfLE((uint16_t)(plan->copy.x1 - plan->dx));
    copy.srcY = Swap16IfLE((uint16_t)(plan->copy.y1 - plan->dy));

    return rfbWriteExact(cl,
                         (const char *)&header,
                         sz_rfbFramebufferUpdateRectHeader) > 0 &&
           rfbWriteExact(cl,
                         (const char *)&copy,
                         sz_rfbCopyRect) > 0;
}

static int
copy_write_jpeg(rfbClientPtr cl, const EncodedRect *encoded)
{
    rfbFramebufferUpdateRectHeader header;
    memset(&header, 0, sizeof(header));

    header.r.x = Swap16IfLE((uint16_t)encoded->rect.x1);
    header.r.y = Swap16IfLE((uint16_t)encoded->rect.y1);
    header.r.w = Swap16IfLE((uint16_t)(encoded->rect.x2 - encoded->rect.x1));
    header.r.h = Swap16IfLE((uint16_t)(encoded->rect.y2 - encoded->rect.y1));
    header.encoding = Swap32IfLE(ADAPTIVE_RFB_JPEG_ENCODING);

    return rfbWriteExact(cl,
                         (const char *)&header,
                         sz_rfbFramebufferUpdateRectHeader) > 0 &&
           rfbWriteExact(cl,
                         (const char *)encoded->data,
                         (int)encoded->size) > 0;
}

static int
copy_send_update(rfbClientPtr cl,
                 const CopyPlan *plan,
                 unsigned long *jpeg_size)
{
    EncodedRect encoded[4];
    memset(encoded, 0, sizeof(encoded));

    if (copy_encode_residuals(cl->screen,
                              plan,
                              encoded,
                              jpeg_size) < 0) {
        LOG_ERROR("CopyRect residual JPEG encode failed");
        return -1;
    }

    rfbFramebufferUpdateMsg update;
    memset(&update, 0, sizeof(update));
    update.type = rfbFramebufferUpdate;
    update.nRects = Swap16IfLE((uint16_t)(1 + plan->jpeg_count));

    int ok = rfbWriteExact(cl,
                           (const char *)&update,
                           sz_rfbFramebufferUpdateMsg) > 0 &&
             copy_write_copyrect(cl, plan);

    for (int i = 0; ok && i < plan->jpeg_count; i++)
        ok = copy_write_jpeg(cl, &encoded[i]);

    copy_free_encoded(encoded, plan->jpeg_count);
    return ok ? 0 : -1;
}

AdaptiveRfbState *
adaptive_rfb_create(rfbScreenInfoPtr screen, int width, int height)
{
    AdaptiveRfbState *state =
        adaptive_rfb_create_legacy(screen, width, height);

    if (!state)
        return NULL;

    if (copy_sidecar_add(state) < 0) {
        LOG_ERROR("Failed to allocate CopyRect reference frame");
        adaptive_rfb_destroy_legacy(state);
        return NULL;
    }

    LOG_DEBUG("CopyRect detector enabled: radius=%dpx verify=%dpx min=%llu px (%.0f%% of pending bbox)",
              COPY_SEARCH_RADIUS,
              COPY_VERIFY_TILE,
              (unsigned long long)COPY_MIN_PIXELS,
              COPY_MIN_SHARE_PERCENT);
    return state;
}

void
adaptive_rfb_destroy(AdaptiveRfbState *state)
{
    if (!state)
        return;

    copy_sidecar_remove(state);
    adaptive_rfb_destroy_legacy(state);
}

int
adaptive_rfb_try_send_pending(AdaptiveRfbState *state,
                              int transport_ready,
                              double now)
{
    CopySidecar *sidecar = copy_sidecar_find(state);

    if (!state || !sidecar || !transport_ready)
        return adaptive_rfb_try_send_pending_legacy(state,
                                                    transport_ready,
                                                    now);

    CopyRectBox pending;
    uint64_t sequence;
    uint64_t coalesced_frames;
    double changed_percent;

    pthread_mutex_lock(&state->mutex);
    if (!state->pending_jpeg || !state->jpeg_supported || !state->jpeg_client) {
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    pending.x1 = state->pending_x1;
    pending.y1 = state->pending_y1;
    pending.x2 = state->pending_x2;
    pending.y2 = state->pending_y2;
    sequence = state->pending_sequence;
    coalesced_frames = state->pending_source_frames;
    changed_percent = state->pending_changed_percent;
    pthread_mutex_unlock(&state->mutex);

    rfbClientPtr cl = get_jpeg_client_ref(state);
    if (!cl)
        return 0;

    int may_try_copy = 0;
    lock_client_update(cl);
    if (cl->sock != RFB_INVALID_SOCKET &&
        cl->state == RFB_NORMAL &&
        !cl->onHold &&
        cl->useCopyRect &&
        !FB_UPDATE_PENDING(cl) &&
        requested_covers_rect_locked(cl,
                                     pending.x1,
                                     pending.y1,
                                     pending.x2,
                                     pending.y2)) {
        may_try_copy = 1;
    }
    unlock_client_update(cl);
    rfbDecrClientRef(cl);

    CopyPlan plan;
    memset(&plan, 0, sizeof(plan));

    if (!may_try_copy || !copy_find_plan(state,
                                         sidecar,
                                         &pending,
                                         &plan)) {
        int rc = adaptive_rfb_try_send_pending_legacy(state,
                                                      transport_ready,
                                                      now);
        if (rc > 0)
            copy_reference_snapshot(state, sidecar);
        return rc;
    }

    cl = get_jpeg_client_ref(state);
    if (!cl)
        return 0;

    int reserved_request = 0;
    lock_client_send(cl);
    lock_client_update(cl);

    if (cl->sock != RFB_INVALID_SOCKET &&
        cl->state == RFB_NORMAL &&
        !cl->onHold &&
        cl->useCopyRect &&
        !FB_UPDATE_PENDING(cl) &&
        requested_covers_rect_locked(cl,
                                     pending.x1,
                                     pending.y1,
                                     pending.x2,
                                     pending.y2)) {
        sraRgnMakeEmpty(cl->requestedRegion);
        reserved_request = 1;
    }

    unlock_client_update(cl);

    if (!reserved_request) {
        unlock_client_send(cl);
        rfbDecrClientRef(cl);
        return 0;
    }

    unsigned long jpeg_size = 0;
    int send_rc = copy_send_update(cl, &plan, &jpeg_size);
    unlock_client_send(cl);

    if (send_rc < 0) {
        LOG_ERROR("CopyRect update send failed; closing backend client to keep RFB state unambiguous");
        rfbCloseClient(cl);
        rfbDecrClientRef(cl);
        return -1;
    }

    rfbDecrClientRef(cl);

    uint64_t raw_bytes = copy_box_area(&pending) * 4ULL;
    size_t repair_remaining;

    pthread_mutex_lock(&state->mutex);
    if (state->pending_jpeg && state->pending_sequence == sequence) {
        state->pending_jpeg = 0;
        state->pending_source_frames = 0;
        state->pending_changed_percent = 0.0;

        repair_mark_rect_locked(state,
                                pending.x1,
                                pending.y1,
                                pending.x2,
                                pending.y2);
        state->last_lossy_at = now;
        state->jpeg_updates++;
        state->jpeg_bytes += jpeg_size;
        state->jpeg_raw_bytes += raw_bytes;
    }
    repair_remaining = state->repair_count;
    pthread_mutex_unlock(&state->mutex);

    copy_reference_snapshot(state, sidecar);

    sidecar->copy_updates++;
    sidecar->copy_pixels += plan.copy_pixels;
    sidecar->residual_pixels += plan.jpeg_pixels;
    sidecar->residual_jpeg_bytes += jpeg_size;

    uint64_t pending_pixels = copy_box_area(&pending);
    double copy_share = pending_pixels > 0
        ? (double)plan.copy_pixels * 100.0 / (double)pending_pixels
        : 0.0;
    double ratio = raw_bytes > 0
        ? (double)jpeg_size * 100.0 / (double)raw_bytes
        : 0.0;

    LOG_TRACE("COPY seq=%llu shift=%+d,%+d copy=%d,%d %dx%d share=%.1f%% residuals=%d jpeg-pixels=%llu bytes=%lu",
              (unsigned long long)sequence,
              plan.dx,
              plan.dy,
              plan.copy.x1,
              plan.copy.y1,
              plan.copy.x2 - plan.copy.x1,
              plan.copy.y2 - plan.copy.y1,
              copy_share,
              plan.jpeg_count,
              (unsigned long long)plan.jpeg_pixels,
              jpeg_size);

    LOG_TRACE("JPEG21 seq=%llu rect=%d,%d %dx%d source-frames=%llu changed<=%.1f%% quality=%d bytes=%lu raw=%llu ratio=%.1f%% repair=%zu copy=yes",
              (unsigned long long)sequence,
              pending.x1,
              pending.y1,
              pending.x2 - pending.x1,
              pending.y2 - pending.y1,
              (unsigned long long)coalesced_frames,
              changed_percent,
              ADAPTIVE_RFB_JPEG_QUALITY,
              jpeg_size,
              (unsigned long long)raw_bytes,
              ratio,
              repair_remaining);

    return 1;
}

void
adaptive_rfb_print_summary(AdaptiveRfbState *state)
{
    adaptive_rfb_print_summary_legacy(state);

    if (!vnc_log_enabled(VNC_LOG_DEBUG))
        return;

    CopySidecar *sidecar = copy_sidecar_find(state);
    if (!sidecar)
        return;

    LOG_DEBUG("CopyRect summary: updates=%llu copy-pixels=%llu residual-pixels=%llu residual-jpeg-bytes=%llu",
              (unsigned long long)sidecar->copy_updates,
              (unsigned long long)sidecar->copy_pixels,
              (unsigned long long)sidecar->residual_pixels,
              (unsigned long long)sidecar->residual_jpeg_bytes);
}
