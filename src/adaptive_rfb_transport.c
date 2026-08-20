/*
 * Production adaptive transport translation unit.
 *
 * The core owns JPEG21, exactness tracking and progressive lossless repair.
 * The CopyRect extension wraps create/send/summary while reusing the core's
 * private helpers and state. Keeping both in one translation unit avoids
 * exporting internal RFB client synchronization details as public API.
 */
#define adaptive_rfb_create adaptive_rfb_create_legacy
#define adaptive_rfb_destroy adaptive_rfb_destroy_legacy
#define adaptive_rfb_try_send_pending adaptive_rfb_try_send_pending_legacy
#define adaptive_rfb_print_summary adaptive_rfb_print_summary_legacy
#include "adaptive_rfb.c"
#undef adaptive_rfb_create
#undef adaptive_rfb_destroy
#undef adaptive_rfb_try_send_pending
#undef adaptive_rfb_print_summary

#include "adaptive_rfb_copyrect.inc"

int
adaptive_rfb_resize(AdaptiveRfbState *state, int width, int height)
{
    if (!state || width <= 0 || height <= 0)
        return -1;

    CopySidecar *sidecar = copy_sidecar_find(state);
    if (!sidecar)
        return -1;

    int tiles_x =
        (width + ADAPTIVE_RFB_REPAIR_TILE_SIZE - 1) /
        ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int tiles_y =
        (height + ADAPTIVE_RFB_REPAIR_TILE_SIZE - 1) /
        ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    size_t tile_count = (size_t)tiles_x * (size_t)tiles_y;

    uint8_t *new_repair_bits = calloc(tile_count, 1);
    uint8_t *new_reference =
        malloc((size_t)width * (size_t)height * 4);

    if (!new_repair_bits || !new_reference) {
        free(new_repair_bits);
        free(new_reference);
        return -1;
    }

    pthread_mutex_lock(&state->mutex);

    uint8_t *old_repair_bits = state->repair_bits;
    state->repair_bits = new_repair_bits;
    state->width = width;
    state->height = height;
    state->tiles_x = tiles_x;
    state->tiles_y = tiles_y;
    state->tile_count = tile_count;

    /*
     * Clear all size-dependent delivery/exactness state, but deliberately
     * preserve jpeg_client/jpeg_supported: framebuffer resize does not require
     * the viewer to repeat SetEncodings.
     */
    reset_client_accuracy_locked(state);

    pthread_mutex_unlock(&state->mutex);
    free(old_repair_bits);

    pthread_mutex_lock(&copy_sidecars_mutex);
    uint8_t *old_reference = sidecar->reference;
    sidecar->reference = new_reference;
    sidecar->reference_valid = 0;
    sidecar->reference_client = NULL;
    pthread_mutex_unlock(&copy_sidecars_mutex);
    free(old_reference);

    LOG_DEBUG("Adaptive transport resized: %dx%d repair-tiles=%zu; CopyRect reference reset",
              width,
              height,
              tile_count);
    return 0;
}
