/**
 * @file test_keyboard.c
 * @brief ORIC keyboard matrix and SDL2 key mapping unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-27
 * @version 1.16.0-alpha
 *
 * Tests: init/reset, press_char ASCII mapping, arrow key SDL mapping
 * (both QWERTY positional and AZERTY symbolic modes).
 */

#include <stdio.h>
#include <string.h>

/* Compiled with -DHAS_SDL2 for SDL2 event simulation */
#include "io/keyboard.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%llX, got 0x%llX\n", __FILE__, __LINE__, \
               (unsigned long long)(b), (unsigned long long)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* Helper: check if a matrix bit is pressed (active low).
 * Convention: matrix[col] has bit `row` cleared when (col, row) is pressed.
 *   - col = hardware column (VIA ORB[0:2])
 *   - row = hardware row    (PSG R14 bit) */
#define KEY_IS_PRESSED(kb, col, row) (((kb).matrix[col] & (1 << (row))) == 0)
#define KEY_IS_RELEASED(kb, col, row) (((kb).matrix[col] & (1 << (row))) != 0)

/* ORIC arrow key positions in matrix.
 * All four arrows share hardware column 4 (the LSHIFT/FUNCT column). */
#define ORIC_UP_COL    4
#define ORIC_UP_ROW    3
#define ORIC_LEFT_COL  4
#define ORIC_LEFT_ROW  5
#define ORIC_DOWN_COL  4
#define ORIC_DOWN_ROW  6
#define ORIC_RIGHT_COL 4
#define ORIC_RIGHT_ROW 7

/* Helper: build a fake SDL_KEYDOWN event */
static SDL_Event make_keydown(SDL_Keycode key, SDL_Scancode sc) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = key;
    ev.key.keysym.scancode = sc;
    return ev;
}

/* Helper: build a fake SDL_KEYUP event */
static SDL_Event make_keyup(SDL_Keycode key, SDL_Scancode sc) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = key;
    ev.key.keysym.scancode = sc;
    return ev;
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Init — all keys released (0xFF)                       */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_keyboard_init) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
    ASSERT_EQ(kb.layout, ORIC_KB_QWERTY);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Reset preserves layout                                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_keyboard_reset_preserves_layout) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);
    oric_keyboard_reset(&kb);

    ASSERT_EQ(kb.layout, ORIC_KB_AZERTY);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: press_char — letters map to correct matrix positions  */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_press_char_letter_A) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* 'A' should map to Col 6, Row 5 */
    ASSERT_TRUE(oric_keyboard_press_char(&kb, 'A'));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 6, 5));
}

TEST(test_press_char_letter_lowercase) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* 'a' same matrix position as 'A' */
    ASSERT_TRUE(oric_keyboard_press_char(&kb, 'a'));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 6, 5));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: press_char — RETURN                                   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_press_char_return) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    ASSERT_TRUE(oric_keyboard_press_char(&kb, '\n'));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 7, 5));  /* RETURN = Col 7, Row 5 */
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: press_char — shifted character (!) sets LSHIFT        */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_press_char_shifted) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* '!' = Shift + 1, key at Col 0 Row 5, Shift at Col 4 Row 4 */
    ASSERT_TRUE(oric_keyboard_press_char(&kb, '!'));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 0, 5));  /* '1' key */
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 4, 4));  /* LSHIFT */
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: release_all clears matrix                             */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_release_all) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    oric_keyboard_press_char(&kb, 'Z');
    oric_keyboard_release_all(&kb);

    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: QWERTY — Arrow UP maps to Col 4 Row 3                */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_arrow_up) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_UP, SDL_SCANCODE_UP);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_UP_COL, ORIC_UP_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: QWERTY — Arrow DOWN maps to Col 4 Row 6              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_arrow_down) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_DOWN, SDL_SCANCODE_DOWN);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_DOWN_COL, ORIC_DOWN_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: QWERTY — Arrow LEFT maps to Col 4 Row 5              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_arrow_left) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_LEFT_COL, ORIC_LEFT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 10: QWERTY — Arrow RIGHT maps to Col 4 Row 7            */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_arrow_right) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_RIGHT_COL, ORIC_RIGHT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 11: QWERTY — Arrow key release clears matrix bit        */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_arrow_release) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* Press UP */
    SDL_Event down = make_keydown(SDLK_UP, SDL_SCANCODE_UP);
    oric_keyboard_handle_sdl_event(&kb, &down);
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_UP_COL, ORIC_UP_ROW));

    /* Release UP */
    SDL_Event up = make_keyup(SDLK_UP, SDL_SCANCODE_UP);
    oric_keyboard_handle_sdl_event(&kb, &up);
    ASSERT_TRUE(KEY_IS_RELEASED(kb, ORIC_UP_COL, ORIC_UP_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 12: AZERTY — Arrow UP via scancode (special_keys table)  */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_azerty_arrow_up) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);

    SDL_Event ev = make_keydown(SDLK_UP, SDL_SCANCODE_UP);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_UP_COL, ORIC_UP_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 13: AZERTY — Arrow DOWN via scancode                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_azerty_arrow_down) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);

    SDL_Event ev = make_keydown(SDLK_DOWN, SDL_SCANCODE_DOWN);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_DOWN_COL, ORIC_DOWN_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 14: AZERTY — Arrow LEFT via scancode                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_azerty_arrow_left) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);

    SDL_Event ev = make_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_LEFT_COL, ORIC_LEFT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 15: AZERTY — Arrow RIGHT via scancode                    */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_azerty_arrow_right) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);

    SDL_Event ev = make_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_RIGHT_COL, ORIC_RIGHT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 16: AZERTY — Arrow release rebuilds matrix correctly     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_azerty_arrow_release) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);

    /* Press LEFT */
    SDL_Event down = make_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT);
    oric_keyboard_handle_sdl_event(&kb, &down);
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_LEFT_COL, ORIC_LEFT_ROW));

    /* Release LEFT */
    SDL_Event up = make_keyup(SDLK_LEFT, SDL_SCANCODE_LEFT);
    oric_keyboard_handle_sdl_event(&kb, &up);
    ASSERT_TRUE(KEY_IS_RELEASED(kb, ORIC_LEFT_COL, ORIC_LEFT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 17: QWERTY — SPACE maps to Col 4 Row 0                  */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_space) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 4, 0));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 18: QWERTY — RETURN maps to Col 7 Row 5                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_qwerty_return) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    SDL_Event ev = make_keydown(SDLK_RETURN, SDL_SCANCODE_RETURN);
    ASSERT_TRUE(oric_keyboard_handle_sdl_event(&kb, &ev));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, 7, 5));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 19: Multiple arrows pressed simultaneously               */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_multiple_arrows) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* Press UP + RIGHT simultaneously */
    SDL_Event ev_up = make_keydown(SDLK_UP, SDL_SCANCODE_UP);
    SDL_Event ev_right = make_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    oric_keyboard_handle_sdl_event(&kb, &ev_up);
    oric_keyboard_handle_sdl_event(&kb, &ev_right);

    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_UP_COL, ORIC_UP_ROW));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_RIGHT_COL, ORIC_RIGHT_ROW));

    /* Release UP, RIGHT still pressed */
    SDL_Event rel_up = make_keyup(SDLK_UP, SDL_SCANCODE_UP);
    oric_keyboard_handle_sdl_event(&kb, &rel_up);
    ASSERT_TRUE(KEY_IS_RELEASED(kb, ORIC_UP_COL, ORIC_UP_ROW));
    ASSERT_TRUE(KEY_IS_PRESSED(kb, ORIC_RIGHT_COL, ORIC_RIGHT_ROW));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 20: press_char — unmapped character returns false        */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_press_char_unmapped) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    ASSERT_FALSE(oric_keyboard_press_char(&kb, '~'));
    /* Matrix untouched */
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("Running ORIC keyboard mapping tests...\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("  Init/Reset:\n");
    RUN(test_keyboard_init);
    RUN(test_keyboard_reset_preserves_layout);

    printf("\n  press_char API:\n");
    RUN(test_press_char_letter_A);
    RUN(test_press_char_letter_lowercase);
    RUN(test_press_char_return);
    RUN(test_press_char_shifted);
    RUN(test_release_all);
    RUN(test_press_char_unmapped);

    printf("\n  QWERTY — Arrow keys:\n");
    RUN(test_qwerty_arrow_up);
    RUN(test_qwerty_arrow_down);
    RUN(test_qwerty_arrow_left);
    RUN(test_qwerty_arrow_right);
    RUN(test_qwerty_arrow_release);

    printf("\n  AZERTY — Arrow keys:\n");
    RUN(test_azerty_arrow_up);
    RUN(test_azerty_arrow_down);
    RUN(test_azerty_arrow_left);
    RUN(test_azerty_arrow_right);
    RUN(test_azerty_arrow_release);

    printf("\n  Other keys:\n");
    RUN(test_qwerty_space);
    RUN(test_qwerty_return);
    RUN(test_multiple_arrows);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
