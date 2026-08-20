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

    return
        (uint64_t)(box->x2 - box->x1) *
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
copy_reference_is_valid(
    AdaptiveRfbState *state,
    CopySidecar *sidecar)
{
    rfbClientPtr client = NULL;
    int supported = 0;

    pthread_mutex_lock(&state->mutex);
    client = state->jpeg_client;
    supported = state->jpeg_supported;
    pthread_mutex_unlock(&state->mutex);

    if (
        !supported ||
        !client ||
        !sidecar->reference_valid ||
        sidecar->reference_client != client
    ) {
        sidecar->reference_valid = 0;
        sidecar->reference_client = client;
        return 0;
    }

    return 1;
}

static void
copy_reference_snapshot(
    AdaptiveRfbState *state,
    CopySidecar *sidecar)
{
    if (
        !state ||
        !sidecar ||
        !sidecar->reference ||
        !state->screen ||
        !state->screen->frameBuffer
    )
        return;

    const uint8_t *source =
        (const uint8_t *)state->screen->frameBuffer;

    for (int y = 0; y < state->height; y++) {
        memcpy(
            sidecar->reference +
                (size_t)y * (size_t)state->width * 4,
            source +
                (size_t)y *
                    (size_t)state->screen->paddedWidthInBytes,
            (size_t)state->width * 4
        );
    }

