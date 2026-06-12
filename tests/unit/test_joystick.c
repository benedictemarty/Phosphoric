/**
 * @file test_joystick.c
 * @brief IJK joystick interface unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.6.0-alpha
 */

#include <stdio.h>
#include <string.h>
#include "io/joystick.h"
#include "io/via6522.h"

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

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 1: Init state — all released                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_init) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);

    /* All bits HIGH (active low = released) */
    ASSERT_EQ(joy.port_a_mask, 0xFF);
    ASSERT_EQ(joy.mode, ORIC_JOY_DISABLED);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 2: Disabled mode returns 0xFF                             */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_disabled) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);

    /* Even if we press, disabled mode returns 0xFF */
    joy.port_a_mask = 0x00;
    ASSERT_EQ(oric_joystick_read(&joy), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 3: Press single direction                                 */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_press_direction) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    /* Press UP: bit 4 should go LOW */
    oric_joystick_press(&joy, IJK_UP);
    ASSERT_EQ(oric_joystick_read(&joy), (uint8_t)~IJK_UP);

    /* Release UP */
    oric_joystick_release(&joy, IJK_UP);
    ASSERT_EQ(oric_joystick_read(&joy), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 4: Press fire button                                      */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_fire) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    oric_joystick_press(&joy, IJK_FIRE);
    /* Bit 5 should be LOW */
    ASSERT_EQ(joy.port_a_mask & IJK_FIRE, 0);
    ASSERT_EQ(oric_joystick_read(&joy), (uint8_t)~IJK_FIRE);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 5: Multiple simultaneous presses                          */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_simultaneous) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    /* Press UP + RIGHT + FIRE simultaneously */
    oric_joystick_press(&joy, IJK_UP);
    oric_joystick_press(&joy, IJK_RIGHT);
    oric_joystick_press(&joy, IJK_FIRE);

    uint8_t expected = (uint8_t)~(IJK_UP | IJK_RIGHT | IJK_FIRE);
    ASSERT_EQ(oric_joystick_read(&joy), expected);

    /* Release only FIRE */
    oric_joystick_release(&joy, IJK_FIRE);
    expected = (uint8_t)~(IJK_UP | IJK_RIGHT);
    ASSERT_EQ(oric_joystick_read(&joy), expected);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 6: Release all                                            */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_release_all) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    oric_joystick_press(&joy, IJK_UP | IJK_DOWN | IJK_LEFT | IJK_RIGHT | IJK_FIRE);
    ASSERT_TRUE(oric_joystick_read(&joy) != 0xFF);

    oric_joystick_release_all(&joy);
    ASSERT_EQ(oric_joystick_read(&joy), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 7: IJK bit layout matches spec                            */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_ijk_bit_layout) {
    /* Real-hardware layout (validated against Oricutron after the
     * v1.16 PSG-blend model was proven wrong on a real interface) */
    ASSERT_EQ(IJK_RIGHT,    0x01);  /* Bit 0 */
    ASSERT_EQ(IJK_LEFT,     0x02);  /* Bit 1 */
    ASSERT_EQ(IJK_FIRE,     0x04);  /* Bit 2 */
    ASSERT_EQ(IJK_DOWN,     0x08);  /* Bit 3 */
    ASSERT_EQ(IJK_UP,       0x10);  /* Bit 4 */
    ASSERT_EQ(IJK_PRESENCE, 0x20);  /* Bit 5 — 0 = interface present */
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Protocol tests — oric_joystick_port_a_pins()                   */
/*  Enable = PB4 output low ; select = Port A pins 6-7             */
/* ═══════════════════════════════════════════════════════════════ */

/* Enabled VIA state: PB4 output+low, ORA bit6 driven high (stick A
 * selected), bit7 low, bits 0-5 inputs. */
#define IJK_DDRA_SEL  0xC0
#define IJK_ORA_SELA  0x40
#define IJK_DDRB_EN   0x10
#define IJK_ORB_EN    0x00

TEST(test_ijk_protocol_disabled_pb4_input) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    oric_joystick_press(&joy, IJK_FIRE);
    /* PB4 not configured as output → interface transparent */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, IJK_ORA_SELA, IJK_DDRA_SEL,
                                        IJK_ORB_EN, 0x00), 0xFF);
}

TEST(test_ijk_protocol_disabled_pb4_high) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    oric_joystick_press(&joy, IJK_FIRE);
    /* PB4 output but driven high → interface transparent */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, IJK_ORA_SELA, IJK_DDRA_SEL,
                                        0x10, IJK_DDRB_EN), 0xFF);
}

TEST(test_ijk_protocol_presence_only) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    oric_joystick_press(&joy, IJK_FIRE);
    /* Enabled but BOTH select pins high → presence bit only */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, 0xC0, IJK_DDRA_SEL,
                                        IJK_ORB_EN, IJK_DDRB_EN),
              (uint8_t)~IJK_PRESENCE);
}

TEST(test_ijk_protocol_stick_a_read) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    /* Idle stick, stick A selected → presence low, rest high */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, IJK_ORA_SELA, IJK_DDRA_SEL,
                                        IJK_ORB_EN, IJK_DDRB_EN),
              (uint8_t)~IJK_PRESENCE);
    /* Up + Fire pressed → their bits AND presence go low */
    oric_joystick_press(&joy, IJK_UP | IJK_FIRE);
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, IJK_ORA_SELA, IJK_DDRA_SEL,
                                        IJK_ORB_EN, IJK_DDRB_EN),
              (uint8_t)~(IJK_PRESENCE | IJK_UP | IJK_FIRE));
}

TEST(test_ijk_protocol_select_pins_float_high) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    oric_joystick_press(&joy, IJK_LEFT);
    /* DDRA all-input: select pins float high = both selected = none.
     * Only the presence line shows. */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, 0x00, 0x00,
                                        IJK_ORB_EN, IJK_DDRB_EN),
              (uint8_t)~IJK_PRESENCE);
}

TEST(test_ijk_protocol_disabled_mode) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);   /* mode = DISABLED */
    /* Even with the right VIA state, no interface plugged → 0xFF */
    ASSERT_EQ(oric_joystick_port_a_pins(&joy, IJK_ORA_SELA, IJK_DDRA_SEL,
                                        IJK_ORB_EN, IJK_DDRB_EN), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  End-to-end via6522 integration — replays the exact 6502 read   */
/*  sequence used by game code (e.g. Asteroids input.s):           */
/*    DDRB=$F7 ; ORB&=~$10 ; DDRA=$C0 ; ORA=$40 ; LDA ORA          */
/* ═══════════════════════════════════════════════════════════════ */

static oric_joystick_t* g_e2e_joy;
static via6522_t g_e2e_via;

static uint8_t e2e_porta_read(void* userdata) {
    (void)userdata;
    return oric_joystick_port_a_pins(g_e2e_joy,
                                     g_e2e_via.ora, g_e2e_via.ddra,
                                     g_e2e_via.orb, g_e2e_via.ddrb);
}

TEST(test_ijk_via_read_sequence) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);
    oric_joystick_press(&joy, IJK_FIRE | IJK_LEFT);

    via_init(&g_e2e_via);
    via_reset(&g_e2e_via);
    g_e2e_joy = &joy;
    g_e2e_via.porta_read = e2e_porta_read;

    /* Replay the game's read sequence (register-level) */
    via_write(&g_e2e_via, VIA_DDRB, 0xF7);          /* PB3 in, PB4 out */
    via_write(&g_e2e_via, VIA_ORB,
              (uint8_t)(via_read(&g_e2e_via, VIA_ORB) & ~0x10)); /* PB4=0 */
    via_write(&g_e2e_via, VIA_DDRA, 0xC0);          /* 6-7 out, 0-5 in */
    via_write(&g_e2e_via, VIA_ORA, 0x40);           /* select stick A */

    uint8_t v = via_read(&g_e2e_via, VIA_ORA);
    /* Bits 6-7 read back the driven select value ($40) ; bits 0-5 :
     * presence + fire + left low, rest high */
    ASSERT_EQ(v, (uint8_t)(0x40 |
              (0x3F & (uint8_t)~(IJK_PRESENCE | IJK_FIRE | IJK_LEFT))));

    /* Disable (PB4 back high) → lines released, inputs read $3F high */
    via_write(&g_e2e_via, VIA_ORB,
              (uint8_t)(via_read(&g_e2e_via, VIA_ORB) | 0x10));
    v = via_read(&g_e2e_via, VIA_ORA);
    ASSERT_EQ(v, (uint8_t)(0x40 | 0x3F));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 8: Mode switching resets state                            */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_mode_switch) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    oric_joystick_press(&joy, IJK_FIRE);
    ASSERT_TRUE(oric_joystick_read(&joy) != 0xFF);

    /* Switching mode should reset state */
    oric_joystick_set_mode(&joy, ORIC_JOY_SDL_GAMEPAD);
    ASSERT_EQ(oric_joystick_read(&joy), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 9: All four directions                                    */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_all_directions) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    /* Test each direction individually */
    uint8_t dirs[] = { IJK_UP, IJK_DOWN, IJK_LEFT, IJK_RIGHT };
    for (int i = 0; i < 4; i++) {
        oric_joystick_press(&joy, dirs[i]);
        ASSERT_EQ(joy.port_a_mask & dirs[i], 0);  /* Bit should be LOW */
        oric_joystick_release(&joy, dirs[i]);
        ASSERT_TRUE((joy.port_a_mask & dirs[i]) != 0);  /* Bit should be HIGH */
    }
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEST 10: Reset clears state                                    */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_joystick_reset) {
    oric_joystick_t joy;
    oric_joystick_init(&joy);
    oric_joystick_set_mode(&joy, ORIC_JOY_KEYBOARD);

    oric_joystick_press(&joy, IJK_UP | IJK_FIRE);
    oric_joystick_reset(&joy);
    ASSERT_EQ(joy.port_a_mask, 0xFF);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  MAIN                                                           */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  IJK Joystick Interface Tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    RUN(test_joystick_init);
    RUN(test_joystick_disabled);
    RUN(test_joystick_press_direction);
    RUN(test_joystick_fire);
    RUN(test_joystick_simultaneous);
    RUN(test_joystick_release_all);
    RUN(test_ijk_bit_layout);
    RUN(test_ijk_protocol_disabled_pb4_input);
    RUN(test_ijk_protocol_disabled_pb4_high);
    RUN(test_ijk_protocol_presence_only);
    RUN(test_ijk_protocol_stick_a_read);
    RUN(test_ijk_protocol_select_pins_float_high);
    RUN(test_ijk_protocol_disabled_mode);
    RUN(test_ijk_via_read_sequence);
    RUN(test_joystick_mode_switch);
    RUN(test_joystick_all_directions);
    RUN(test_joystick_reset);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
