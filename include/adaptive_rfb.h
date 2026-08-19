#ifndef VNC_MONITOR_ADAPTIVE_RFB_H
#define VNC_MONITOR_ADAPTIVE_RFB_H

#include <stdint.h>

#include <rfb/rfb.h>

#define ADAPTIVE_RFB_JPEG_ENCODING            21
#define ADAPTIVE_RFB_JPEG_QUALITY             80
#define ADAPTIVE_RFB_JPEG_THRESHOLD_PERCENT   25.0
#define ADAPTIVE_RFB_REPAIR_TILE_SIZE         32
#define ADAPTIVE_RFB_REPAIR_BUDGET_KBPS       4096
#define ADAPTIVE_RFB_REPAIR_IDLE_SECONDS      0.250

typedef struct AdaptiveRfbState AdaptiveRfbState;

AdaptiveRfbState *adaptive_rfb_create(
    rfbScreenInfoPtr screen,
    int width,
    int height);

void adaptive_rfb_destroy(AdaptiveRfbState *state);

int adaptive_rfb_should_queue_jpeg(
    AdaptiveRfbState *state,
    double changed_percent);

void adaptive_rfb_queue_jpeg_rect(
    AdaptiveRfbState *state,
    int x1,
    int y1,
    int x2,
    int y2,
    uint64_t sequence,
    double changed_percent);

int adaptive_rfb_has_pending_jpeg(AdaptiveRfbState *state);

int adaptive_rfb_try_send_pending(
    AdaptiveRfbState *state,
    int transport_ready,
    double now);

void adaptive_rfb_note_lossless_rect(
    AdaptiveRfbState *state,
    int x1,
    int y1,
    int x2,
    int y2);

int adaptive_rfb_has_repair(AdaptiveRfbState *state);

int adaptive_rfb_try_schedule_repair(
    AdaptiveRfbState *state,
    int transport_ready,
    double now,
    double last_source_arrival);

void adaptive_rfb_print_summary(AdaptiveRfbState *state);

#endif
