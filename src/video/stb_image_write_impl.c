/**
 * @file stb_image_write_impl.c
 * @brief Single translation unit hosting the stb_image_write implementation.
 *
 * Both the MJPEG AVI recorder (src/video/avi_recorder.c) and the Chromecast
 * cast server (src/network/cast_server.c, CAST=1) use stb_image_write's
 * `stbi_write_jpg_to_func`. The implementation must be emitted in exactly one
 * TU to avoid duplicate symbols, so it lives here and is always compiled.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb_image_write.h"
