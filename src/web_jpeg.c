#include "web_jpeg.h"

#include <jpeglib.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} WebJpegError;

static void
web_jpeg_error_exit(j_common_ptr cinfo)
{
    WebJpegError *error = (WebJpegError *)cinfo->err;
    longjmp(error->jump, 1);
}

int
web_jpeg_encode_bgrx(const uint8_t *pixels,
                     int width,
                     int height,
                     int quality,
                     uint8_t **jpeg_data,
                     size_t *jpeg_size)
{
    if (!pixels || !jpeg_data || !jpeg_size ||
        width <= 0 || height <= 0 ||
        quality < 1 || quality > 100 ||
        (size_t)width > SIZE_MAX / 4u ||
        (size_t)width > SIZE_MAX / 3u ||
        (size_t)height > SIZE_MAX / ((size_t)width * 4u)) {
        return -1;
    }

    *jpeg_data = NULL;
    *jpeg_size = 0;

    struct jpeg_compress_struct cinfo;
    WebJpegError error;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&error, 0, sizeof(error));

    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = web_jpeg_error_exit;

    int created = 0;
    unsigned char *encoded = NULL;
    unsigned long encoded_size = 0;
    uint8_t *row = NULL;

    if (setjmp(error.jump) != 0) {
        free(row);
        free(encoded);
        if (created)
            jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    created = 1;
    jpeg_mem_dest(&cinfo, &encoded, &encoded_size);

    cinfo.image_width = (JDIMENSION)width;
    cinfo.image_height = (JDIMENSION)height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row = malloc((size_t)width * 3u);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        free(encoded);
        return -1;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *src =
            pixels + (size_t)cinfo.next_scanline * (size_t)width * 4u;

        for (int x = 0; x < width; x++) {
            row[(size_t)x * 3u] = src[(size_t)x * 4u + 2u];
            row[(size_t)x * 3u + 1u] = src[(size_t)x * 4u + 1u];
            row[(size_t)x * 3u + 2u] = src[(size_t)x * 4u];
        }

        JSAMPROW rows[1] = { row };
        (void)jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);

    if (!encoded || encoded_size == 0) {
        free(encoded);
        return -1;
    }

    *jpeg_data = encoded;
    *jpeg_size = (size_t)encoded_size;
    return 0;
}
