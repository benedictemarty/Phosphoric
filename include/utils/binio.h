/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file binio.h
 * @brief Little-endian binary read/write for .ost sections.
 * @author bmarty <bmarty@mailo.com>
 *
 * Same encoding as the historical helpers of savestate.c; shared by the
 * device modules that serialise their own state (sprint D). Reads return 0 at
 * end of file: the caller checks the section size before reading.
 */
#ifndef UTILS_BINIO_H
#define UTILS_BINIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static inline void bin_w_u8(FILE* fp, uint8_t v) { fwrite(&v, 1, 1, fp); }
static inline void bin_w_bool(FILE* fp, bool v) { bin_w_u8(fp, v ? 1 : 0); }
static inline void bin_w_u16(FILE* fp, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, fp);
}
static inline void bin_w_u32(FILE* fp, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, fp);
}
static inline void bin_w_i16(FILE* fp, int16_t v) { bin_w_u16(fp, (uint16_t)v); }
static inline void bin_w_i32(FILE* fp, int32_t v) { bin_w_u32(fp, (uint32_t)v); }

static inline uint8_t bin_r_u8(FILE* fp) {
    uint8_t v = 0;
    if (fread(&v, 1, 1, fp) != 1) return 0;
    return v;
}
static inline bool bin_r_bool(FILE* fp) { return bin_r_u8(fp) != 0; }
static inline uint16_t bin_r_u16(FILE* fp) {
    uint8_t b[2] = { 0, 0 };
    if (fread(b, 1, 2, fp) != 2) return 0;
    return (uint16_t)(b[0] | (b[1] << 8));
}
static inline uint32_t bin_r_u32(FILE* fp) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (fread(b, 1, 4, fp) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline int16_t bin_r_i16(FILE* fp) { return (int16_t)bin_r_u16(fp); }
static inline int32_t bin_r_i32(FILE* fp) { return (int32_t)bin_r_u32(fp); }

#endif /* UTILS_BINIO_H */
