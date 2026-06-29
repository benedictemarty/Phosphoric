/**
 * @file export.h
 * @brief Video framebuffer export (PPM, BMP, ASCII)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 */

#ifndef VIDEO_EXPORT_H
#define VIDEO_EXPORT_H

#include <stdio.h>
#include "video/video.h"

/**
 * @brief Export framebuffer as PPM (P6 binary format)
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.ppm)
 * @return true on success
 */
bool video_export_ppm(const video_t* vid, const char* filename);

/**
 * @brief Export framebuffer as BMP (24-bit uncompressed)
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.bmp)
 * @return true on success
 */
bool video_export_bmp(const video_t* vid, const char* filename);

/**
 * @brief Export framebuffer as ANSI true-color text to a file
 * @param vid Video context with rendered framebuffer
 * @param fp Output FILE pointer (e.g. stdout)
 * @param scale_x Horizontal pixel grouping (e.g. 2 = half width)
 * @param scale_y Vertical pixel grouping (e.g. 2 = half height)
 * @return true on success
 */
bool video_export_ascii(const video_t* vid, FILE* fp, unsigned int scale_x, unsigned int scale_y);

/**
 * @brief Auto-detect format from filename extension and export
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.ppm or .bmp)
 * @return true on success
 */
bool video_export_auto(const video_t* vid, const char* filename);

/**
 * @brief Like video_export_auto(), but with the OCULA overscan border
 *        composited around the active area (larger image). Format auto-detected
 *        from the extension (.bmp → BMP, else PPM).
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path
 * @return true on success, false on error / allocation failure
 */
bool video_export_auto_bordered(const video_t* vid, const char* filename);

#endif /* VIDEO_EXPORT_H */
