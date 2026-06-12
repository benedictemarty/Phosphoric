/**
 * @file export.c
 * @brief Video framebuffer export - PPM, BMP, ASCII
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 *
 * PPM: P6 binary format - header ASCII + raw RGB888 data
 * BMP: BITMAPFILEHEADER + BITMAPINFOHEADER + bottom-up RGB
 * ASCII: ANSI true-color escape codes for terminal display
 */

#include "video/export.h"
#include <stdio.h>
#include <string.h>

bool video_export_ppm(const video_t* vid, const char* filename) {
    if (!vid || !filename) return false;

    FILE* fp = fopen(filename, "wb");
    if (!fp) return false;

    /* PPM P6 header — native resolution follows the active ULA profile */
    fprintf(fp, "P6\n%d %d\n255\n", vid->native_w, vid->native_h);

    /* Raw RGB data */
    size_t pixels = (size_t)(vid->native_w * vid->native_h * 3);
    size_t written = fwrite(vid->framebuffer, 1, pixels, fp);
    fclose(fp);

    return written == pixels;
}

bool video_export_bmp(const video_t* vid, const char* filename) {
    if (!vid || !filename) return false;

    FILE* fp = fopen(filename, "wb");
    if (!fp) return false;

    unsigned int w = (unsigned int)vid->native_w;
    unsigned int h = (unsigned int)vid->native_h;
    unsigned int row_stride = w * 3;
    unsigned int row_padding = (4 - (row_stride % 4)) % 4;
    unsigned int padded_row = row_stride + row_padding;
    uint32_t pixel_data_size = (uint32_t)(padded_row * h);
    uint32_t file_size = 54 + pixel_data_size;

    /* BITMAPFILEHEADER (14 bytes) */
    uint8_t bfh[14];
    memset(bfh, 0, sizeof(bfh));
    bfh[0] = 'B'; bfh[1] = 'M';
    bfh[2] = (uint8_t)(file_size);
    bfh[3] = (uint8_t)(file_size >> 8);
    bfh[4] = (uint8_t)(file_size >> 16);
    bfh[5] = (uint8_t)(file_size >> 24);
    bfh[10] = 54; /* pixel data offset */

    /* BITMAPINFOHEADER (40 bytes) */
    uint8_t bih[40];
    memset(bih, 0, sizeof(bih));
    bih[0] = 40; /* header size */
    bih[4] = (uint8_t)(w);
    bih[5] = (uint8_t)(w >> 8);
    bih[8] = (uint8_t)(h);
    bih[9] = (uint8_t)(h >> 8);
    bih[12] = 1; /* planes */
    bih[14] = 24; /* bits per pixel */
    bih[20] = (uint8_t)(pixel_data_size);
    bih[21] = (uint8_t)(pixel_data_size >> 8);
    bih[22] = (uint8_t)(pixel_data_size >> 16);
    bih[23] = (uint8_t)(pixel_data_size >> 24);

    fwrite(bfh, 1, 14, fp);
    fwrite(bih, 1, 40, fp);

    /* Pixel data: BMP is bottom-up, BGR order */
    uint8_t pad[3] = {0, 0, 0};
    for (int y = (int)h - 1; y >= 0; y--) {
        for (unsigned int x = 0; x < w; x++) {
            unsigned int off = ((unsigned int)y * w + x) * 3;
            uint8_t bgr[3];
            bgr[0] = vid->framebuffer[off + 2]; /* B */
            bgr[1] = vid->framebuffer[off + 1]; /* G */
            bgr[2] = vid->framebuffer[off + 0]; /* R */
            fwrite(bgr, 1, 3, fp);
        }
        if (row_padding > 0) {
            fwrite(pad, 1, (size_t)row_padding, fp);
        }
    }

    fclose(fp);
    return true;
}

bool video_export_ascii(const video_t* vid, FILE* fp, unsigned int scale_x, unsigned int scale_y) {
    if (!vid || !fp) return false;
    if (scale_x == 0) scale_x = 2;
    if (scale_y == 0) scale_y = 2;

    for (unsigned int y = 0; y < (unsigned int)vid->native_h; y += scale_y) {
        for (unsigned int x = 0; x < (unsigned int)vid->native_w; x += scale_x) {
            int off = (int)(y * (unsigned int)vid->native_w + x) * 3;
            uint8_t r = vid->framebuffer[off];
            uint8_t g = vid->framebuffer[off + 1];
            uint8_t b = vid->framebuffer[off + 2];
            fprintf(fp, "\x1b[48;2;%d;%d;%dm ", r, g, b);
        }
        fprintf(fp, "\x1b[0m\n");
    }
    fprintf(fp, "\x1b[0m");
    return true;
}

bool video_export_auto(const video_t* vid, const char* filename) {
    if (!vid || !filename) return false;

    const char* ext = strrchr(filename, '.');
    if (!ext) {
        /* Default to PPM */
        return video_export_ppm(vid, filename);
    }

    if (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0) {
        return video_export_bmp(vid, filename);
    }

    /* Default to PPM for .ppm or any other extension */
    return video_export_ppm(vid, filename);
}
