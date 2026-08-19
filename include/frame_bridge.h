#ifndef VNC_MONITOR_FRAME_BRIDGE_H
#define VNC_MONITOR_FRAME_BRIDGE_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint8_t *pixels;
    int width;
    int height;
    uint64_t sequence;
    int initialized;
} FrameBridge;

int frame_bridge_init(
    FrameBridge *bridge,
    int width,
    int height);

void frame_bridge_clear(FrameBridge *bridge);
void frame_bridge_destroy(FrameBridge *bridge);

int frame_bridge_publish_bgrx(
    FrameBridge *bridge,
    const uint8_t *data,
    int stride,
    int width,
    int height);

int frame_bridge_consume(
    FrameBridge *bridge,
    uint8_t *destination,
    uint64_t *last_sequence);

/*
 * Wait until bridge->sequence differs from observed_sequence or timeout_ms
 * expires. Does not consume/copy the frame. current_sequence receives the
 * latest sequence seen while holding the bridge mutex.
 *
 * Returns: 1 = changed, 0 = timeout/spurious wake, -1 = error.
 */
int frame_bridge_wait_for_change(
    FrameBridge *bridge,
    uint64_t observed_sequence,
    int timeout_ms,
    uint64_t *current_sequence);

/* Wake waiters during shutdown without manufacturing a new frame. */
void frame_bridge_wake_all(FrameBridge *bridge);

#endif
