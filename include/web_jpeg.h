#ifndef VNC_MONITOR_WEB_JPEG_H
#define VNC_MONITOR_WEB_JPEG_H

#include <stddef.h>
#include <stdint.h>

int web_jpeg_encode_bgrx(const uint8_t *pixels,
                         int width,
                         int height,
                         int quality,
                         uint8_t **jpeg_data,
                         size_t *jpeg_size);

#endif
