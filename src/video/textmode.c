/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file textmode.c
 * @brief Text mode helpers (40x28)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.4.0-alpha
 */

#include <stdint.h>
#include <string.h>

#define TEXT_BASE 0xBB80
#define TEXT_COLS 40
#define TEXT_ROWS 28

void textmode_clear(uint8_t* memory, uint8_t fill) {
    memset(memory + TEXT_BASE, fill, TEXT_COLS * TEXT_ROWS);
}

void textmode_putchar(uint8_t* memory, int col, int row, uint8_t ch) {
    if (col >= 0 && col < TEXT_COLS && row >= 0 && row < TEXT_ROWS)
        memory[TEXT_BASE + row * TEXT_COLS + col] = ch;
}

uint8_t textmode_getchar(const uint8_t* memory, int col, int row) {
    if (col >= 0 && col < TEXT_COLS && row >= 0 && row < TEXT_ROWS)
        return memory[TEXT_BASE + row * TEXT_COLS + col];
    return 0x20;
}

void textmode_scroll_up(uint8_t* memory) {
    memmove(memory + TEXT_BASE, memory + TEXT_BASE + TEXT_COLS,
            TEXT_COLS * (TEXT_ROWS - 1));
    memset(memory + TEXT_BASE + TEXT_COLS * (TEXT_ROWS - 1), 0x20, TEXT_COLS);
}
