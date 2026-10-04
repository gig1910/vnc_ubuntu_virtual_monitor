#include "web_jpeg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
             jpeg[jpeg_size - 1] == 0xd9;
    free(jpeg);

    if (!ok) {
        fprintf(stderr, "web_jpeg_test: invalid JPEG envelope\n");
        return 1;
    }

    printf("web_jpeg_test: OK\n");
    return 0;
}
