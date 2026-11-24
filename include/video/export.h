/* SPDX-License-Identifier: EUPL-1.2 */
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
 * @brief Export framebuffer as PNG (RGB888, compressed, via stb_image_write)
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.png)
 * @return true on success
 */
bool video_export_png(const video_t* vid, const char* filename);

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
 * @brief Like video_export_ascii() but opens/closes the file itself.
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (text, ANSI escapes)
 * @param scale_x Horizontal pixel grouping (0 => default 2)
 * @param scale_y Vertical pixel grouping (0 => default 2)
 * @return true on success
 */
bool video_export_ascii_file(const video_t* vid, const char* filename,
                             unsigned int scale_x, unsigned int scale_y);

/**
 * @brief Exports the actual TEXT CONTENT of the screen ($BB80, 40x28) as readable ASCII.
 *
 * Reads the ORIC text buffer directly from RAM (not the framebuffer). Each byte
 * is decoded as `byte & 0x7F` (stripping the inverse-video bit); control/attribute
 * codes (< 0x20) become spaces. Each line is written over at most 40 columns,
 * trailing spaces removed, terminated by '\n'.
 *
 * @note Approximation: assumes the standard ORIC character set, for which
 *       0x20-0x7F matches ASCII. Redefined charsets are not resolved.
 * @note Always reads the 28 lines of $BB80, regardless of mode (TEXT/HIRES).
 *
 * @param memory Pointer to the 64KB RAM (index $BB80 read directly)
 * @param fp Output file (e.g. stdout)
 * @return true on success
 */
bool video_export_screen_text(const uint8_t* memory, FILE* fp);

/**
 * @brief Auto-detect format from filename extension and export
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path (.ppm or .bmp)
 * @return true on success
 */
bool video_export_auto(const video_t* vid, const char* filename);

/**
 * @brief Like video_export_auto(), but with the overscan border
 *        composited around the active area (larger image). Format auto-detected
 *        from the extension (.bmp → BMP, else PPM).
 * @param vid Video context with rendered framebuffer
 * @param filename Output file path
 * @return true on success, false on error / allocation failure
 */
bool video_export_auto_bordered(const video_t* vid, const char* filename);

#endif /* VIDEO_EXPORT_H */
