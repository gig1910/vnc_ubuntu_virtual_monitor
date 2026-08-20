#include "frame_bridge.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
frame_bridge_init(
    FrameBridge *bridge,
    int width,
    int height)
{
    if (
        !bridge ||
        width <= 0 ||
        height <= 0
    )
        return -1;

    memset(bridge, 0, sizeof(*bridge));

    bridge->pixels =
        calloc(
            (size_t)width *
            (size_t)height,
            4
        );

    if (!bridge->pixels)
        return -1;

    if (
        pthread_mutex_init(
            &bridge->mutex,
            NULL
        ) != 0
    ) {
        free(bridge->pixels);
        memset(bridge, 0, sizeof(*bridge));
        return -1;
    }

    if (
        pthread_cond_init(
            &bridge->cond,
            NULL
        ) != 0
    ) {
        pthread_mutex_destroy(&bridge->mutex);
        free(bridge->pixels);
        memset(bridge, 0, sizeof(*bridge));
        return -1;
    }

    bridge->width = width;
    bridge->height = height;
    bridge->initialized = 1;

    return 0;
}

int
frame_bridge_resize(
    FrameBridge *bridge,
    int width,
    int height)
{
    if (!bridge || !bridge->initialized || width <= 0 || height <= 0)
        return -1;

    pthread_mutex_lock(&bridge->mutex);
    if (bridge->width == width && bridge->height == height) {
        pthread_mutex_unlock(&bridge->mutex);
        return 0;
    }
    pthread_mutex_unlock(&bridge->mutex);

    uint8_t *new_pixels =
        calloc((size_t)width * (size_t)height, 4);
    if (!new_pixels)
        return -1;

    pthread_mutex_lock(&bridge->mutex);

    uint8_t *old_pixels = bridge->pixels;
    bridge->pixels = new_pixels;
    bridge->width = width;
    bridge->height = height;
    bridge->sequence++;
    pthread_cond_broadcast(&bridge->cond);

    pthread_mutex_unlock(&bridge->mutex);

    free(old_pixels);
    return 0;
}

void
frame_bridge_clear(FrameBridge *bridge)
{
    if (!bridge || !bridge->initialized)
        return;

    pthread_mutex_lock(&bridge->mutex);

    memset(
        bridge->pixels,
        0,
        (size_t)bridge->width *
        (size_t)bridge->height *
        4
    );

    bridge->sequence++;
    pthread_cond_broadcast(&bridge->cond);

    pthread_mutex_unlock(&bridge->mutex);
}

void
frame_bridge_destroy(FrameBridge *bridge)
{
    if (!bridge || !bridge->initialized)
        return;

    pthread_cond_destroy(&bridge->cond);
    pthread_mutex_destroy(&bridge->mutex);
    free(bridge->pixels);
    memset(bridge, 0, sizeof(*bridge));
}

int
frame_bridge_publish_bgrx(
    FrameBridge *bridge,
    const uint8_t *data,
    int stride,
    int width,
    int height)
{
    if (!bridge || !bridge->initialized || !data || stride < width * 4)
        return -1;

    pthread_mutex_lock(&bridge->mutex);

    if (width != bridge->width || height != bridge->height) {
        pthread_mutex_unlock(&bridge->mutex);
        return -1;
    }

    size_t row_bytes = (size_t)width * 4;

    for (int y = 0; y < height; y++) {
        memcpy(
            bridge->pixels + (size_t)y * row_bytes,
            data + (size_t)y * (size_t)stride,
            row_bytes
        );
    }

    bridge->sequence++;
    pthread_cond_broadcast(&bridge->cond);

    pthread_mutex_unlock(&bridge->mutex);
    return 0;
}

int
frame_bridge_consume(
    FrameBridge *bridge,
    uint8_t *destination,
    uint64_t *last_sequence)
{
    if (
        !bridge ||
        !bridge->initialized ||
        !destination ||
        !last_sequence
    )
        return -1;

    int changed = 0;

    pthread_mutex_lock(&bridge->mutex);

    if (*last_sequence != bridge->sequence) {
        memcpy(
            destination,
            bridge->pixels,
            (size_t)bridge->width *
            (size_t)bridge->height *
            4
        );

        *last_sequence = bridge->sequence;
        changed = 1;
    }

    pthread_mutex_unlock(&bridge->mutex);
    return changed;
}

int
frame_bridge_wait_for_change(
    FrameBridge *bridge,
    uint64_t observed_sequence,
    int timeout_ms,
    uint64_t *current_sequence)
{
    if (
        !bridge ||
        !bridge->initialized ||
        timeout_ms < 0
    )
        return -1;

    pthread_mutex_lock(&bridge->mutex);

    if (bridge->sequence != observed_sequence) {
        if (current_sequence)
            *current_sequence = bridge->sequence;
        pthread_mutex_unlock(&bridge->mutex);
        return 1;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        pthread_mutex_unlock(&bridge->mutex);
        return -1;
    }

    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    int rc =
        pthread_cond_timedwait(
            &bridge->cond,
            &bridge->mutex,
            &deadline
        );

    uint64_t sequence = bridge->sequence;

    if (current_sequence)
        *current_sequence = sequence;

    pthread_mutex_unlock(&bridge->mutex);

    if (sequence != observed_sequence)
        return 1;

    if (rc == 0 || rc == ETIMEDOUT)
        return 0;

    return -1;
}

void
frame_bridge_wake_all(FrameBridge *bridge)
{
    if (!bridge || !bridge->initialized)
        return;

    pthread_mutex_lock(&bridge->mutex);
    pthread_cond_broadcast(&bridge->cond);
    pthread_mutex_unlock(&bridge->mutex);
}
