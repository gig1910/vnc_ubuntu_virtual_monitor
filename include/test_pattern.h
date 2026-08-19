#ifndef VNC_MONITOR_TEST_PATTERN_H
#define VNC_MONITOR_TEST_PATTERN_H

#include <stdint.h>

#include "runtime_config.h"

typedef struct {
    int dirty_x1;
    int dirty_y1;
    int dirty_x2;
    int dirty_y2;
    int square_size;
    const char *stage;
} TestPatternFrame;

void test_pattern_reset(void);

void test_pattern_init(
    uint8_t *fb,
    const RuntimeConfig *cfg);

void test_pattern_render(
    uint8_t *fb,
    const RuntimeConfig *cfg,
    double elapsed,
    unsigned int frame,
    TestPatternFrame *out);

#endif
