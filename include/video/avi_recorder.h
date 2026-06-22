/**
 * @file avi_recorder.h
 * @brief Motion-JPEG AVI video recorder
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Records a stream of RGB888 frames to a self-contained AVI file using
 * Motion-JPEG (MJPG) video compression. Each frame is JPEG-encoded
 * (via stb_image_write) and stored as a `00dc` chunk in the `movi` list.
 * A trailing `idx1` index is written on close, and the RIFF/movi/header
 * sizes are back-patched so the file is playable by VLC, ffmpeg, mpv, etc.
 *
 * No external dependency (parity with Oricutron's AVI export). Works in
 * both SDL2 and headless builds.
 */

#ifndef VIDEO_AVI_RECORDER_H
#define VIDEO_AVI_RECORDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/** @brief One entry of the AVI `idx1` index (per frame). */
typedef struct {
    uint32_t offset;  /**< chunk header offset, relative to `movi` fourcc */
    uint32_t size;    /**< JPEG payload size (excluding pad byte) */
} avi_index_entry_t;

/** @brief MJPEG AVI recorder state. */
typedef struct {
    FILE*    fp;             /**< output file (open while recording) */
    int      width;          /**< frame width in pixels */
    int      height;         /**< frame height in pixels */
    int      fps;            /**< frames per second */
    int      quality;        /**< JPEG quality 1..100 */
    uint32_t frame_count;    /**< frames written so far */
    uint32_t max_jpeg_size;  /**< largest JPEG payload (suggested buffer) */

    long     movi_pos;       /**< file offset of the `movi` fourcc */

    avi_index_entry_t* index;     /**< growable index entries */
    uint32_t           index_len; /**< number of valid entries */
    uint32_t           index_cap; /**< allocated capacity */

    /* Reusable JPEG encode scratch buffer. */
    uint8_t* jpeg_buf;
    size_t   jpeg_len;
    size_t   jpeg_cap;
} avi_recorder_t;

/**
 * @brief Open an AVI file and write placeholder headers.
 * @param rec      Recorder to initialize (zeroed on entry not required)
 * @param filename Output path (.avi)
 * @param width    Frame width
 * @param height   Frame height
 * @param fps      Frames per second (e.g. 50 for PAL ORIC)
 * @param quality  JPEG quality 1..100 (clamped)
 * @return true on success
 */
bool avi_recorder_open(avi_recorder_t* rec, const char* filename,
                       int width, int height, int fps, int quality);

/**
 * @brief Append one RGB888 frame (top-down, width*height*3 bytes).
 * @return true on success
 */
bool avi_recorder_add_frame(avi_recorder_t* rec, const uint8_t* rgb);

/**
 * @brief Finalize: write `idx1`, back-patch sizes, close the file.
 *
 * Safe to call on a NULL/unopened recorder (no-op). After this call the
 * recorder is reset and must be re-opened before reuse.
 * @return true on success
 */
bool avi_recorder_close(avi_recorder_t* rec);

#endif /* VIDEO_AVI_RECORDER_H */
