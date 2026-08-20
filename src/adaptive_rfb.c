#include "adaptive_rfb.h"
#include "log.h"

#include <rfb/rfbregion.h>
#include <jpeglib.h>

#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AdaptiveRfbState {
    pthread_mutex_t mutex;
    rfbScreenInfoPtr screen;
    rfbDisplayFinishedHookPtr previous_display_finished_hook;

    int width;
    int height;
    int tiles_x;
    int tiles_y;
    size_t tile_count;
    uint8_t *repair_bits;
    size_t repair_count;

    int repair_pass;
    size_t repair_scan;
    int repair_inflight;
    size_t repair_inflight_index;
    int repair_inflight_pass;
    double next_repair_at;
    double last_lossy_at;

    int pending_jpeg;
    int pending_x1;
    int pending_y1;
    int pending_x2;
    int pending_y2;
    uint64_t pending_sequence;
    uint64_t pending_source_frames;
    double pending_changed_percent;

    rfbClientPtr jpeg_client;
    int jpeg_supported;

    uint64_t jpeg_updates;
    uint64_t jpeg_bytes;
    uint64_t jpeg_raw_bytes;
    uint64_t repair_tiles_sent;

    struct AdaptiveRfbState *next_global;
};

static pthread_mutex_t adaptive_states_mutex = PTHREAD_MUTEX_INITIALIZER;
static AdaptiveRfbState *adaptive_states = NULL;
static pthread_once_t adaptive_extension_once = PTHREAD_ONCE_INIT;

static AdaptiveRfbState *
find_state_for_screen(rfbScreenInfoPtr screen)
{
    AdaptiveRfbState *found = NULL;

    pthread_mutex_lock(&adaptive_states_mutex);
    for (AdaptiveRfbState *state = adaptive_states;
         state;
         state = state->next_global) {
        if (state->screen == screen) {
            found = state;
            break;
        }
    }
    pthread_mutex_unlock(&adaptive_states_mutex);
    return found;
}

static void
reset_client_accuracy_locked(AdaptiveRfbState *state)
{
    if (state->repair_bits)
        memset(state->repair_bits, 0, state->tile_count);

    state->repair_count = 0;
    state->repair_pass = 0;
    state->repair_scan = 0;
    state->repair_inflight = 0;
    state->repair_inflight_index = 0;
    state->repair_inflight_pass = 0;
    state->next_repair_at = 0.0;
    state->last_lossy_at = 0.0;

    state->pending_jpeg = 0;
    state->pending_x1 = 0;
    state->pending_y1 = 0;
    state->pending_x2 = 0;
    state->pending_y2 = 0;
    state->pending_sequence = 0;
    state->pending_source_frames = 0;
    state->pending_changed_percent = 0.0;
}

static rfbBool
adaptive_enable_jpeg21(rfbClientPtr cl, void **data, int encoding)
{
    (void)data;

    if (encoding != ADAPTIVE_RFB_JPEG_ENCODING)
        return FALSE;

    AdaptiveRfbState *state = find_state_for_screen(cl ? cl->screen : NULL);
    if (!state)
        return FALSE;

    int newly_enabled = 0;

    pthread_mutex_lock(&state->mutex);
    if (!state->jpeg_supported || state->jpeg_client != cl) {
        reset_client_accuracy_locked(state);
        state->jpeg_client = cl;
        state->jpeg_supported = 1;
        newly_enabled = 1;
    }
    pthread_mutex_unlock(&state->mutex);

    if (newly_enabled)
        LOG_DEBUG("Client advertised RFB JPEG encoding 21; adaptive JPEG enabled");

    return TRUE;
}

static void
adaptive_extension_close(rfbClientPtr cl, void *data)
{
    (void)data;

    AdaptiveRfbState *state = find_state_for_screen(cl ? cl->screen : NULL);
    if (!state)
        return;

    pthread_mutex_lock(&state->mutex);
    if (state->jpeg_client == cl) {
        state->jpeg_client = NULL;
        state->jpeg_supported = 0;
        reset_client_accuracy_locked(state);
    }
    pthread_mutex_unlock(&state->mutex);
}

static int adaptive_jpeg_encodings[] = {
    ADAPTIVE_RFB_JPEG_ENCODING,
    0
};

static rfbProtocolExtension adaptive_jpeg_extension = {
    NULL,
    NULL,
    adaptive_jpeg_encodings,
    adaptive_enable_jpeg21,
    NULL,
    adaptive_extension_close,
    NULL,
    NULL,
    NULL
};

static void
register_adaptive_extension(void)
{
    rfbRegisterProtocolExtension(&adaptive_jpeg_extension);
}

static void
lock_client_update(rfbClientPtr cl)
{
#if defined(LIBVNCSERVER_HAVE_LIBPTHREAD) || defined(LIBVNCSERVER_HAVE_WIN32THREADS)
    LOCK(cl->updateMutex);
#else
    (void)cl;
#endif
}

static void
unlock_client_update(rfbClientPtr cl)
{
#if defined(LIBVNCSERVER_HAVE_LIBPTHREAD) || defined(LIBVNCSERVER_HAVE_WIN32THREADS)
    UNLOCK(cl->updateMutex);
#else
    (void)cl;
#endif
}

static void
lock_client_send(rfbClientPtr cl)
{
#ifdef LIBVNCSERVER_SEND_MUTEX
    LOCK(cl->sendMutex);
#else
    (void)cl;
#endif
}

static void
unlock_client_send(rfbClientPtr cl)
{
#ifdef LIBVNCSERVER_SEND_MUTEX
    UNLOCK(cl->sendMutex);
#else
    (void)cl;
#endif
}

static uint64_t
region_area(sraRegionPtr region)
{
    uint64_t area = 0;
    sraRectangleIterator *iterator = sraRgnGetIterator(region);
    sraRect rect;

    while (iterator && sraRgnIteratorNext(iterator, &rect)) {
        if (rect.x2 > rect.x1 && rect.y2 > rect.y1) {
            area += (uint64_t)(rect.x2 - rect.x1) *
                    (uint64_t)(rect.y2 - rect.y1);
        }
    }

    if (iterator)
        sraRgnReleaseIterator(iterator);

    return area;
}

static int
requested_covers_rect_locked(rfbClientPtr cl,
                             int x1,
                             int y1,
                             int x2,
                             int y2)
{
    if (!cl || x2 <= x1 || y2 <= y1 || sraRgnEmpty(cl->requestedRegion))
        return 0;

    sraRegionPtr region = sraRgnCreateRect(x1, y1, x2, y2);
    if (!region)
        return 0;

    sraRgnAnd(region, cl->requestedRegion);

    uint64_t wanted = (uint64_t)(x2 - x1) * (uint64_t)(y2 - y1);
    uint64_t covered = region_area(region);

    sraRgnDestroy(region);
    return covered == wanted;
}

static rfbClientPtr
get_jpeg_client_ref(AdaptiveRfbState *state)
{
    if (!state || !state->screen)
        return NULL;

    pthread_mutex_lock(&state->mutex);
    rfbClientPtr target = state->jpeg_supported ? state->jpeg_client : NULL;
    pthread_mutex_unlock(&state->mutex);

    if (!target)
        return NULL;

    rfbClientPtr found = NULL;
    rfbClientIteratorPtr iterator = rfbGetClientIterator(state->screen);
    rfbClientPtr cl;

    while (iterator && (cl = rfbClientIteratorNext(iterator)) != NULL) {
        if (cl == target) {
            rfbIncrClientRef(cl);
            found = cl;
            break;
        }
    }

    if (iterator)
        rfbReleaseClientIterator(iterator);

    return found;
}

static void
repair_clear_index_locked(AdaptiveRfbState *state, size_t index)
{
    if (index >= state->tile_count || !state->repair_bits[index])
        return;

    state->repair_bits[index] = 0;
    if (state->repair_count > 0)
        state->repair_count--;
}

static void
repair_mark_rect_locked(AdaptiveRfbState *state,
                        int x1,
                        int y1,
                        int x2,
                        int y2)
{
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > state->width)
        x2 = state->width;
    if (y2 > state->height)
        y2 = state->height;

    if (x2 <= x1 || y2 <= y1)
        return;

    int tx1 = x1 / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int ty1 = y1 / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int tx2 = (x2 - 1) / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int ty2 = (y2 - 1) / ADAPTIVE_RFB_REPAIR_TILE_SIZE;

    for (int ty = ty1; ty <= ty2; ty++) {
        for (int tx = tx1; tx <= tx2; tx++) {
            size_t index = (size_t)ty * (size_t)state->tiles_x + (size_t)tx;
            if (!state->repair_bits[index]) {
                state->repair_bits[index] = 1;
                state->repair_count++;
            }
        }
    }

    state->repair_pass = 0;
    state->repair_scan = 0;
}

static int
repair_tile_matches_pass(int tx, int ty, int pass)
{
    switch (pass) {
        case 0:
            return (tx % 4 == 0) && (ty % 4 == 0);
        case 1:
            return ((tx & 1) == 0) && ((ty & 1) == 0);
        case 2:
            return ((tx + ty) & 1) == 0;
        default:
            return 1;
    }
}

static int
choose_repair_tile_locked(AdaptiveRfbState *state,
                          size_t *index_out,
                          int *tx_out,
                          int *ty_out,
                          int *pass_out)
{
    if (!state || state->repair_count == 0)
        return 0;

    for (int pass_attempt = 0; pass_attempt < 4; pass_attempt++) {
        int pass = state->repair_pass;

        for (size_t index = state->repair_scan;
             index < state->tile_count;
             index++) {
            if (!state->repair_bits[index])
                continue;

            int tx = (int)(index % (size_t)state->tiles_x);
            int ty = (int)(index / (size_t)state->tiles_x);

            if (!repair_tile_matches_pass(tx, ty, pass))
                continue;

            state->repair_scan = index + 1;
            if (index_out)
                *index_out = index;
            if (tx_out)
                *tx_out = tx;
            if (ty_out)
                *ty_out = ty;
            if (pass_out)
                *pass_out = pass;
            return 1;
        }

        state->repair_pass++;
        state->repair_scan = 0;
        if (state->repair_pass > 3)
            state->repair_pass = 0;
    }

    return 0;
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
} AdaptiveJpegError;

static void
adaptive_jpeg_error_exit(j_common_ptr cinfo)
{
    AdaptiveJpegError *error = (AdaptiveJpegError *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(error->jump, 1);
}

static int
encode_jpeg_rect(rfbScreenInfoPtr screen,
                 int x,
                 int y,
                 int width,
                 int height,
                 unsigned char **jpeg_data,
                 unsigned long *jpeg_size)
{
    if (!screen || !screen->frameBuffer || !jpeg_data || !jpeg_size ||
        width <= 0 || height <= 0 || screen->serverFormat.bitsPerPixel != 32)
        return -1;

    *jpeg_data = NULL;
    *jpeg_size = 0;

    struct jpeg_compress_struct cinfo;
    AdaptiveJpegError jerr;
    volatile int created = 0;
    unsigned char *row = NULL;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = adaptive_jpeg_error_exit;

    if (setjmp(jerr.jump)) {
        if (created)
            jpeg_destroy_compress(&cinfo);
        free(row);
        free(*jpeg_data);
        *jpeg_data = NULL;
        *jpeg_size = 0;
        return -1;
    }

    jpeg_create_compress(&cinfo);
    created = 1;
    jpeg_mem_dest(&cinfo, jpeg_data, jpeg_size);

    cinfo.image_width = (JDIMENSION)width;
    cinfo.image_height = (JDIMENSION)height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, ADAPTIVE_RFB_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row = malloc((size_t)width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        free(*jpeg_data);
        *jpeg_data = NULL;
        *jpeg_size = 0;
        return -1;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        int source_y = y + (int)cinfo.next_scanline;
        const uint8_t *src =
            (const uint8_t *)screen->frameBuffer +
            (size_t)source_y * (size_t)screen->paddedWidthInBytes +
            (size_t)x * 4;

        for (int px = 0; px < width; px++) {
            row[(size_t)px * 3 + 0] = src[(size_t)px * 4 + 2];
            row[(size_t)px * 3 + 1] = src[(size_t)px * 4 + 1];
            row[(size_t)px * 3 + 2] = src[(size_t)px * 4 + 0];
        }

        JSAMPROW rows[1] = {row};
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    return 0;
}

static int
send_jpeg21_rect(rfbClientPtr cl,
                 int x,
                 int y,
                 int width,
                 int height,
                 unsigned long *encoded_size)
{
    unsigned char *jpeg_data = NULL;
    unsigned long jpeg_size = 0;

    if (encode_jpeg_rect(cl->screen,
                         x,
                         y,
                         width,
                         height,
                         &jpeg_data,
                         &jpeg_size) < 0) {
        LOG_ERROR("Adaptive JPEG encoder failed");
        return -1;
    }

    if (jpeg_size > (unsigned long)INT_MAX) {
        LOG_ERROR("Adaptive JPEG rectangle is too large");
        free(jpeg_data);
        return -1;
    }

    rfbFramebufferUpdateMsg update;
    rfbFramebufferUpdateRectHeader rect;
    memset(&update, 0, sizeof(update));
    memset(&rect, 0, sizeof(rect));

    update.type = rfbFramebufferUpdate;
    update.nRects = Swap16IfLE(1);

    rect.r.x = Swap16IfLE((uint16_t)x);
    rect.r.y = Swap16IfLE((uint16_t)y);
    rect.r.w = Swap16IfLE((uint16_t)width);
    rect.r.h = Swap16IfLE((uint16_t)height);
    rect.encoding = Swap32IfLE(ADAPTIVE_RFB_JPEG_ENCODING);

    int ok =
        rfbWriteExact(cl,
                      (const char *)&update,
                      sz_rfbFramebufferUpdateMsg) > 0 &&
        rfbWriteExact(cl,
                      (const char *)&rect,
                      sz_rfbFramebufferUpdateRectHeader) > 0 &&
        rfbWriteExact(cl,
                      (const char *)jpeg_data,
                      (int)jpeg_size) > 0;

    if (encoded_size)
        *encoded_size = jpeg_size;

    free(jpeg_data);
    return ok ? 0 : -1;
}

static void
adaptive_display_finished(rfbClientPtr cl, int result)
{
    AdaptiveRfbState *state = find_state_for_screen(cl ? cl->screen : NULL);
    rfbDisplayFinishedHookPtr previous = NULL;
    int log_repair = 0;
    int tx = 0;
    int ty = 0;
    int pass = 0;
    size_t remaining = 0;

    if (state) {
        pthread_mutex_lock(&state->mutex);
        previous = state->previous_display_finished_hook;

        if (state->repair_inflight && state->jpeg_client == cl) {
            size_t index = state->repair_inflight_index;
            tx = (int)(index % (size_t)state->tiles_x);
            ty = (int)(index / (size_t)state->tiles_x);
            pass = state->repair_inflight_pass;

            if (result && index < state->tile_count &&
                state->repair_bits[index]) {
                repair_clear_index_locked(state, index);
                state->repair_tiles_sent++;
                log_repair = 1;
            }

            state->repair_inflight = 0;
            remaining = state->repair_count;
        }

        pthread_mutex_unlock(&state->mutex);
    }

    if (log_repair) {
        LOG_TRACE("REPAIR exact pass=%d tile=%d,%d remaining=%zu",
                  pass,
                  tx,
                  ty,
                  remaining);
    }

    if (previous)
        previous(cl, result);
}

AdaptiveRfbState *
adaptive_rfb_create(rfbScreenInfoPtr screen, int width, int height)
{
    if (!screen || width <= 0 || height <= 0)
        return NULL;

    AdaptiveRfbState *state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;

    state->screen = screen;
    state->width = width;
    state->height = height;
    state->tiles_x =
        (width + ADAPTIVE_RFB_REPAIR_TILE_SIZE - 1) /
        ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    state->tiles_y =
        (height + ADAPTIVE_RFB_REPAIR_TILE_SIZE - 1) /
        ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    state->tile_count = (size_t)state->tiles_x * (size_t)state->tiles_y;

    state->repair_bits = calloc(state->tile_count, 1);
    if (!state->repair_bits || pthread_mutex_init(&state->mutex, NULL) != 0) {
        free(state->repair_bits);
        free(state);
        return NULL;
    }

    pthread_once(&adaptive_extension_once, register_adaptive_extension);

    state->previous_display_finished_hook = screen->displayFinishedHook;
    screen->displayFinishedHook = adaptive_display_finished;

    pthread_mutex_lock(&adaptive_states_mutex);
    state->next_global = adaptive_states;
    adaptive_states = state;
    pthread_mutex_unlock(&adaptive_states_mutex);

    LOG_DEBUG("Adaptive RFB: JPEG21 quality=%d repair=%dpx budget=%dkbit/s idle=%dms",
              ADAPTIVE_RFB_JPEG_QUALITY,
              ADAPTIVE_RFB_REPAIR_TILE_SIZE,
              ADAPTIVE_RFB_REPAIR_BUDGET_KBPS,
              (int)(ADAPTIVE_RFB_REPAIR_IDLE_SECONDS * 1000.0));

    return state;
}

void
adaptive_rfb_destroy(AdaptiveRfbState *state)
{
    if (!state)
        return;

    if (state->screen &&
        state->screen->displayFinishedHook == adaptive_display_finished) {
        state->screen->displayFinishedHook = state->previous_display_finished_hook;
    }

    pthread_mutex_lock(&adaptive_states_mutex);
    AdaptiveRfbState **cursor = &adaptive_states;
    while (*cursor) {
        if (*cursor == state) {
            *cursor = state->next_global;
            break;
        }
        cursor = &(*cursor)->next_global;
    }
    pthread_mutex_unlock(&adaptive_states_mutex);

    pthread_mutex_destroy(&state->mutex);
    free(state->repair_bits);
    free(state);
}

int
adaptive_rfb_should_queue_jpeg(AdaptiveRfbState *state, double changed_percent)
{
    if (!state)
        return 0;

    pthread_mutex_lock(&state->mutex);
    int result =
        state->pending_jpeg ||
        (state->jpeg_supported &&
         state->jpeg_client &&
         changed_percent >= ADAPTIVE_RFB_JPEG_THRESHOLD_PERCENT);
    pthread_mutex_unlock(&state->mutex);
    return result;
}

void
adaptive_rfb_queue_jpeg_rect(AdaptiveRfbState *state,
                             int x1,
                             int y1,
                             int x2,
                             int y2,
                             uint64_t sequence,
                             double changed_percent)
{
    if (!state)
        return;

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > state->width)
        x2 = state->width;
    if (y2 > state->height)
        y2 = state->height;
    if (x2 <= x1 || y2 <= y1)
        return;

    pthread_mutex_lock(&state->mutex);

    if (!state->pending_jpeg) {
        state->pending_jpeg = 1;
        state->pending_x1 = x1;
        state->pending_y1 = y1;
        state->pending_x2 = x2;
        state->pending_y2 = y2;
        state->pending_sequence = sequence;
        state->pending_source_frames = 1;
        state->pending_changed_percent = changed_percent;
    }
    else {
        if (x1 < state->pending_x1)
            state->pending_x1 = x1;
        if (y1 < state->pending_y1)
            state->pending_y1 = y1;
        if (x2 > state->pending_x2)
            state->pending_x2 = x2;
        if (y2 > state->pending_y2)
            state->pending_y2 = y2;

        if (sequence != state->pending_sequence) {
            state->pending_sequence = sequence;
            state->pending_source_frames++;
        }

        if (changed_percent > state->pending_changed_percent)
            state->pending_changed_percent = changed_percent;
    }

    pthread_mutex_unlock(&state->mutex);
}

int
adaptive_rfb_has_pending_jpeg(AdaptiveRfbState *state)
{
    if (!state)
        return 0;

    pthread_mutex_lock(&state->mutex);
    int result = state->pending_jpeg;
    pthread_mutex_unlock(&state->mutex);
    return result;
}

int
adaptive_rfb_try_send_pending(AdaptiveRfbState *state,
                              int transport_ready,
                              double now)
{
    if (!state || !transport_ready)
        return 0;

    int x1;
    int y1;
    int x2;
    int y2;
    uint64_t sequence;
    uint64_t coalesced_frames;
    double changed_percent;

    pthread_mutex_lock(&state->mutex);
    if (!state->pending_jpeg || !state->jpeg_supported || !state->jpeg_client) {
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    x1 = state->pending_x1;
    y1 = state->pending_y1;
    x2 = state->pending_x2;
    y2 = state->pending_y2;
    sequence = state->pending_sequence;
    coalesced_frames = state->pending_source_frames;
    changed_percent = state->pending_changed_percent;
    pthread_mutex_unlock(&state->mutex);

    rfbClientPtr cl = get_jpeg_client_ref(state);
    if (!cl)
        return 0;

    int reserved_request = 0;

    lock_client_send(cl);
    lock_client_update(cl);
    if (cl->sock != RFB_INVALID_SOCKET &&
        cl->state == RFB_NORMAL &&
        !cl->onHold &&
        !FB_UPDATE_PENDING(cl) &&
        requested_covers_rect_locked(cl, x1, y1, x2, y2)) {
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
    int send_rc = send_jpeg21_rect(cl,
                                   x1,
                                   y1,
                                   x2 - x1,
                                   y2 - y1,
                                   &jpeg_size);
    unlock_client_send(cl);

    if (send_rc < 0) {
        LOG_ERROR("JPEG21 send failed; closing backend client to keep RFB state unambiguous");
        rfbCloseClient(cl);
        rfbDecrClientRef(cl);
        return -1;
    }

    rfbDecrClientRef(cl);

    uint64_t raw_bytes =
        (uint64_t)(x2 - x1) * (uint64_t)(y2 - y1) * 4ULL;
    size_t repair_remaining;

    pthread_mutex_lock(&state->mutex);
    if (state->pending_jpeg && state->pending_sequence == sequence) {
        state->pending_jpeg = 0;
        state->pending_source_frames = 0;
        state->pending_changed_percent = 0.0;

        repair_mark_rect_locked(state, x1, y1, x2, y2);
        state->last_lossy_at = now;
        state->jpeg_updates++;
        state->jpeg_bytes += jpeg_size;
        state->jpeg_raw_bytes += raw_bytes;
    }
    repair_remaining = state->repair_count;
    pthread_mutex_unlock(&state->mutex);

    double ratio = raw_bytes > 0
        ? (double)jpeg_size * 100.0 / (double)raw_bytes
        : 0.0;

    LOG_TRACE("JPEG21 seq=%llu rect=%d,%d %dx%d source-frames=%llu changed<=%.1f%% quality=%d bytes=%lu raw=%llu ratio=%.1f%% repair=%zu",
              (unsigned long long)sequence,
              x1,
              y1,
              x2 - x1,
              y2 - y1,
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
adaptive_rfb_note_lossless_rect(AdaptiveRfbState *state,
                                int x1,
                                int y1,
                                int x2,
                                int y2)
{
    if (!state)
        return;

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > state->width)
        x2 = state->width;
    if (y2 > state->height)
        y2 = state->height;
    if (x2 <= x1 || y2 <= y1)
        return;

    pthread_mutex_lock(&state->mutex);

    int tx1 = x1 / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int ty1 = y1 / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int tx2 = (x2 - 1) / ADAPTIVE_RFB_REPAIR_TILE_SIZE;
    int ty2 = (y2 - 1) / ADAPTIVE_RFB_REPAIR_TILE_SIZE;

    for (int ty = ty1; ty <= ty2; ty++) {
        int tile_y1 = ty * ADAPTIVE_RFB_REPAIR_TILE_SIZE;
        int tile_y2 = tile_y1 + ADAPTIVE_RFB_REPAIR_TILE_SIZE;
        if (tile_y2 > state->height)
            tile_y2 = state->height;

        for (int tx = tx1; tx <= tx2; tx++) {
            int tile_x1 = tx * ADAPTIVE_RFB_REPAIR_TILE_SIZE;
            int tile_x2 = tile_x1 + ADAPTIVE_RFB_REPAIR_TILE_SIZE;
            if (tile_x2 > state->width)
                tile_x2 = state->width;

            if (x1 <= tile_x1 && y1 <= tile_y1 &&
                x2 >= tile_x2 && y2 >= tile_y2) {
                size_t index =
                    (size_t)ty * (size_t)state->tiles_x + (size_t)tx;
                repair_clear_index_locked(state, index);

                if (state->repair_inflight &&
                    state->repair_inflight_index == index) {
                    state->repair_inflight = 0;
                }
            }
        }
    }

    pthread_mutex_unlock(&state->mutex);
}

int
adaptive_rfb_has_repair(AdaptiveRfbState *state)
{
    if (!state)
        return 0;

    pthread_mutex_lock(&state->mutex);
    int result = state->repair_count > 0;
    pthread_mutex_unlock(&state->mutex);
    return result;
}

int
adaptive_rfb_try_schedule_repair(AdaptiveRfbState *state,
                                 int transport_ready,
                                 double now,
                                 double last_source_arrival)
{
    if (!state || !transport_ready)
        return 0;

    pthread_mutex_lock(&state->mutex);
    double quiet_anchor = last_source_arrival > state->last_lossy_at
        ? last_source_arrival
        : state->last_lossy_at;

    int eligible =
        state->repair_count > 0 &&
        !state->pending_jpeg &&
        !state->repair_inflight &&
        now >= state->next_repair_at &&
        quiet_anchor > 0.0 &&
        now - quiet_anchor >= ADAPTIVE_RFB_REPAIR_IDLE_SECONDS;
    pthread_mutex_unlock(&state->mutex);

    if (!eligible)
        return 0;

    rfbClientPtr cl = get_jpeg_client_ref(state);
    if (!cl)
        return 0;

    size_t index = 0;
    int tx = 0;
    int ty = 0;
    int pass = 0;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int scheduled = 0;
    size_t remaining = 0;

    lock_client_update(cl);

    if (cl->sock != RFB_INVALID_SOCKET &&
        cl->state == RFB_NORMAL &&
        !cl->onHold &&
        !FB_UPDATE_PENDING(cl) &&
        !sraRgnEmpty(cl->requestedRegion)) {
        pthread_mutex_lock(&state->mutex);

        if (state->repair_count > 0 &&
            !state->pending_jpeg &&
            !state->repair_inflight &&
            choose_repair_tile_locked(state,
                                      &index,
                                      &tx,
                                      &ty,
                                      &pass)) {
            x1 = tx * ADAPTIVE_RFB_REPAIR_TILE_SIZE;
            y1 = ty * ADAPTIVE_RFB_REPAIR_TILE_SIZE;
            x2 = x1 + ADAPTIVE_RFB_REPAIR_TILE_SIZE;
            y2 = y1 + ADAPTIVE_RFB_REPAIR_TILE_SIZE;

            if (x2 > state->width)
                x2 = state->width;
            if (y2 > state->height)
                y2 = state->height;

            if (requested_covers_rect_locked(cl, x1, y1, x2, y2)) {
                state->repair_inflight = 1;
                state->repair_inflight_index = index;
                state->repair_inflight_pass = pass;

                double raw_bits =
                    (double)(x2 - x1) * (double)(y2 - y1) * 4.0 * 8.0;
                double interval = raw_bits /
                    ((double)ADAPTIVE_RFB_REPAIR_BUDGET_KBPS * 1000.0);

                if (interval < 0.005)
                    interval = 0.005;

                state->next_repair_at = now + interval;
                remaining = state->repair_count;
                scheduled = 1;
            }
        }

        pthread_mutex_unlock(&state->mutex);
    }

    unlock_client_update(cl);
    rfbDecrClientRef(cl);

    if (!scheduled)
        return 0;

    rfbMarkRectAsModified(state->screen, x1, y1, x2, y2);

    LOG_TRACE("REPAIR schedule pass=%d tile=%d,%d rect=%d,%d %dx%d remaining=%zu",
              pass,
              tx,
              ty,
              x1,
              y1,
              x2 - x1,
              y2 - y1,
              remaining);

    return 1;
}

void
adaptive_rfb_print_summary(AdaptiveRfbState *state)
{
    if (!state || !vnc_log_enabled(VNC_LOG_DEBUG))
        return;

    pthread_mutex_lock(&state->mutex);
    uint64_t jpeg_updates = state->jpeg_updates;
    uint64_t jpeg_bytes = state->jpeg_bytes;
    uint64_t jpeg_raw_bytes = state->jpeg_raw_bytes;
    uint64_t repair_tiles = state->repair_tiles_sent;
    size_t repair_remaining = state->repair_count;
    int pending_jpeg = state->pending_jpeg;
    pthread_mutex_unlock(&state->mutex);

    double saved = jpeg_raw_bytes > 0
        ? 100.0 - (double)jpeg_bytes * 100.0 / (double)jpeg_raw_bytes
        : 0.0;

    LOG_DEBUG("Adaptive summary: jpeg-updates=%llu jpeg-bytes=%llu raw-equivalent=%llu saved=%.1f%% repair-tiles=%llu repair-remaining=%zu jpeg-pending=%s",
              (unsigned long long)jpeg_updates,
              (unsigned long long)jpeg_bytes,
              (unsigned long long)jpeg_raw_bytes,
              saved,
              (unsigned long long)repair_tiles,
              repair_remaining,
              pending_jpeg ? "yes" : "no");
}
