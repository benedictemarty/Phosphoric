/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file hires.c
 * @brief HIRES mode helpers (240x200)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.4.0-alpha
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define HIRES_BASE 0xA000
#define HIRES_W    240
#define HIRES_H    200
#define HIRES_COLS 40

void hires_clear(uint8_t* memory, uint8_t fill) {
    memset(memory + HIRES_BASE, fill, HIRES_COLS * HIRES_H);
}

void hires_set_pixel(uint8_t* memory, int x, int y, bool on) {
    if (x < 0 || x >= HIRES_W || y < 0 || y >= HIRES_H) return;
    int col = x / 6;
    int bit = 5 - (x % 6);
    uint16_t addr = HIRES_BASE + (uint16_t)(y * HIRES_COLS + col);
    if (on) memory[addr] |= (uint8_t)(1 << bit);
    else memory[addr] &= (uint8_t)~(1 << bit);
    memory[addr] |= 0x40;
}

bool hires_get_pixel(const uint8_t* memory, int x, int y) {
    if (x < 0 || x >= HIRES_W || y < 0 || y >= HIRES_H) return false;
    int col = x / 6;
    int bit = 5 - (x % 6);
    uint16_t addr = HIRES_BASE + (uint16_t)(y * HIRES_COLS + col);
    return (memory[addr] & (1 << bit)) != 0;
}
