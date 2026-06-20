/**
 * @file keyboard.h
 * @brief ORIC-1 keyboard matrix emulation with SDL2 mapping
 *
 * Hardware matrix (8 columns x 8 rows):
 *   - Column select: VIA ORB[0:2] → 74LS138 decoder → 1 of 8 column lines
 *   - Row mask:      PSG register 14 (= PSG Port A in output mode), 8 bits,
 *                    active-low (bit r = 0 → row r is being tested)
 *   - Scan result:   VIA PB3 = 1 iff any tested row has a key pressed
 *                    in the selected column
 *
 * STORAGE CONVENTION (matrix[8]):
 *   matrix[col] holds the row-state byte for hardware column `col` (where
 *   `col` is the value driven on VIA ORB[0:2]). Within that byte, bit `row`
 *   corresponds to PSG R14 bit `row`. A key at hardware position (col, row)
 *   is "pressed" when bit `row` of matrix[col] is **clear** (active-low,
 *   matching the hardware bus). matrix[col]=0xFF means no key in column `col`.
 *
 * Supports two mapping modes:
 * - QWERTY: positional mapping (SDL keycode → ORIC matrix position)
 * - AZERTY: symbolic mapping via SDL_TEXTINPUT (character → ORIC key combo)
 *
 * Key mapping table from Oricutron (Peter Gordon, GPL v2).
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef HAS_SDL2
#include <SDL2/SDL.h>
#endif

/** Keyboard layout selection */
typedef enum {
    ORIC_KB_QWERTY = 0,  /**< QWERTY positional layout */
    ORIC_KB_AZERTY        /**< Symbolic mapping (AZERTY/any layout via TEXTINPUT) */
} oric_kb_layout_t;

/** Max simultaneous key presses tracked in symbolic mode */
#define ORIC_KB_MAX_PRESSED 16

/**
 * @brief ORIC keyboard matrix (8 columns x 8 rows)
 *
 * matrix[col] : row-state byte for hardware column `col` (VIA ORB[0:2]).
 *               Bit `row` corresponds to PSG R14 bit `row`. Active-low.
 *               matrix[col]=0xFF → no keys in column `col`.
 *               matrix[col] & (1<<row) == 0 → key at (col, row) is pressed.
 */
typedef struct oric_keyboard_s {
    uint8_t matrix[8];
    oric_kb_layout_t layout;
#ifdef HAS_SDL2
    /* Symbolic mode: track pressed keys for proper release */
    struct {
        uint32_t scancode;
        int8_t col;     /* Hardware column (VIA ORB[0:2]) */
        int8_t row;     /* Hardware row    (PSG R14 bit)  */
        bool shift;     /* This key needs ORIC Shift */
    } pressed[ORIC_KB_MAX_PRESSED];
    int pressed_count;
    uint32_t pending_scancode;  /* Last KEYDOWN scancode awaiting TEXTINPUT */
    bool has_pending;
#endif
} oric_keyboard_t;

void oric_keyboard_init(oric_keyboard_t* kb);
void oric_keyboard_reset(oric_keyboard_t* kb);
void oric_keyboard_set_layout(oric_keyboard_t* kb, oric_kb_layout_t layout);

/**
 * @brief Press a key by ASCII character (for automated input / --type-keys)
 *
 * Sets the appropriate matrix bits for the given character.
 * Call oric_keyboard_release_all() after a delay to release.
 *
 * @return true if the character was mapped
 */
bool oric_keyboard_press_char(oric_keyboard_t* kb, char c);

/**
 * @brief Release all keys (clear matrix to 0xFF)
 */
void oric_keyboard_release_all(oric_keyboard_t* kb);

/**
 * @brief Press the CTRL modifier (held, does not release other keys)
 *
 * Clears the LCTRL matrix bit only. Intended for --type-keys \Cx combos:
 * call after release_all(), then oric_keyboard_press_char() for the
 * companion key. Release with oric_keyboard_release_all().
 */
void oric_keyboard_press_ctrl(oric_keyboard_t* kb);

/**
 * @brief Press the FUNCT modifier (held, does not release other keys)
 *
 * Clears the FUNCT matrix bit only (same usage as oric_keyboard_press_ctrl).
 */
void oric_keyboard_press_funct(oric_keyboard_t* kb);

#ifdef HAS_SDL2
/**
 * @brief Handle SDL2 event and update ORIC keyboard matrix
 *
 * In QWERTY mode: handles SDL_KEYDOWN/SDL_KEYUP (positional mapping).
 * In AZERTY mode: handles SDL_TEXTINPUT + SDL_KEYDOWN/UP (symbolic mapping).
 *
 * @return true if the event was mapped to an ORIC key
 */
bool oric_keyboard_handle_sdl_event(oric_keyboard_t* kb, const SDL_Event* event);
#endif

#endif /* KEYBOARD_H */
