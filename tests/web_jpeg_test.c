#include "web_jpeg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
jpeg_has_legacy_profile(const uint8_t *jpeg, size_t size)
{
    if (!jpeg || size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
        return 0;

    int jfif = 0;
    int baseline = 0;
    int progressive = 0;
    size_t pos = 2;

    while (pos + 1 < size) {
        while (pos < size && jpeg[pos] == 0xff)
            pos++;
        if (pos >= size)
            break;

        uint8_t marker = jpeg[pos++];
        if (marker == 0xd9 || marker == 0xda)
            break;
        if (marker == 0x00 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }

        if (pos + 2 > size)
            return 0;

        size_t segment_length =
            ((size_t)jpeg[pos] << 8) | (size_t)jpeg[pos + 1];
        if (segment_length < 2 || segment_length > size - pos)
            return 0;

        const uint8_t *payload = jpeg + pos + 2;
        size_t payload_length = segment_length - 2;

        if (marker == 0xe0 && payload_length >= 5 &&
            memcmp(payload, "JFIF\0", 5) == 0) {
            jfif = 1;
        }
        else if (marker == 0xc0) {
            if (payload_length < 6 ||
                payload[0] != 8 ||
                payload[5] != 3) {
                return 0;
            }
            baseline = 1;
        }
        else if (marker == 0xc2) {
            progressive = 1;
        }

        pos += segment_length;
    }

    return jfif && baseline && !progressive;
}

int
main(void)
{
    uint8_t pixels[4 * 3 * 4];
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = 0x20;
        pixels[i + 1] = 0x70;
        pixels[i + 2] = 0xc0;
        pixels[i + 3] = 0xff;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;

    if (web_jpeg_encode_bgrx(pixels, 4, 3, 65, &jpeg, &jpeg_size) < 0) {
        fprintf(stderr, "web_jpeg_test: encode failed\n");
        return 1;
    }

    int ok = jpeg_size >= 4 &&
             jpeg[0] == 0xff && jpeg[1] == 0xd8 &&
             jpeg[jpeg_size - 2] == 0xff &&
             jpeg[jpeg_size - 1] == 0xd9 &&
             jpeg_has_legacy_profile(jpeg, jpeg_size);
    free(jpeg);

    if (!ok) {
        fprintf(stderr, "web_jpeg_test: invalid baseline JFIF JPEG\n");
        return 1;
    }

    printf("web_jpeg_test: OK\n");
    return 0;
}
