/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_tape_patches.c
 * @brief Native tape loading (CLOAD through the patched ROM): sync,
 *        byte read, exhausted tape
 * @author bmarty <bmarty@mailo.com>
 *
 * Without ROM: the PC is put on the patched entry points as the ROM would
 * during a CLOAD, and tape_patches() acts on the tape buffer.
 */
#include "emulator.h"
#include "rom_patches.h"
#include "io/tape_patches.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { int b = tests_failed; printf("  %-52s", #name); name(); \
    if (tests_failed == b) { tests_passed++; printf("PASS\n"); } } while (0)
#define ASSERT_TRUE(x) do { if (!(x)) { \
    printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #x); tests_failed++; return; } } while (0)

static emulator_t emu;
static uint8_t tape[64];

/* Tape of @p len bytes in @p tape, model @p model. */
static const rom_patches_t* load(oric_model_t model, int len) {
    const rom_patches_t* p = get_rom_patches(model);
    emu.rom_patches = p;
    emu.tapebuf = tape;
    emu.tapelen = len;
    emu.tapeoffs = 0;
    emu.tape_loaded = true;
    emu.tape_syncstack = -1;
    emu.cpu.SP = 0xF0;
    return p;
}

TEST(test_getsync_skips_noise_to_first_sync) {
    const uint8_t t[] = { 0x00, 0xAA, 0x55, 0x16, 0x16, 0x24 };
    memcpy(tape, t, sizeof t);
    const rom_patches_t* p = load(ORIC_MODEL_ATMOS, (int)sizeof t);
    emu.cpu.PC = p->getsync_entry;
    tape_patches(&emu);
    ASSERT_TRUE(emu.tapeoffs == 3);                 /* on the first $16 */
    ASSERT_TRUE(emu.cpu.PC == p->getsync_end);
    ASSERT_TRUE(emu.tape_syncstack == 0xF0);
}

TEST(test_readbyte_feeds_rom) {
    const uint8_t t[] = { 0x00, 0x42 };
    for (int m = 0; m < 2; m++) {
        memcpy(tape, t, sizeof t);
        const oric_model_t model = m ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1;
        const rom_patches_t* p = load(model, (int)sizeof t);
        emu.cpu.PC = p->readbyte_entry;
        tape_patches(&emu);
        ASSERT_TRUE(emu.cpu.A == 0x00 && (emu.cpu.P & FLAG_ZERO));
        ASSERT_TRUE(((emu.cpu.P & FLAG_CARRY) != 0) == p->readbyte_setcarry);
        ASSERT_TRUE(emu.cpu.PC == p->readbyte_end);
        emu.cpu.PC = p->readbyte_entry;
        tape_patches(&emu);
        ASSERT_TRUE(emu.cpu.A == 0x42 && !(emu.cpu.P & FLAG_ZERO));
        ASSERT_TRUE(memory_read(&emu.memory, p->readbyte_store) == 0x42);
        /* Exhausted: the ROM routine is no longer short-circuited. */
        emu.cpu.PC = p->readbyte_entry;
        tape_patches(&emu);
        ASSERT_TRUE(emu.cpu.PC == p->readbyte_entry);
        ASSERT_TRUE(emu.tapeoffs == 2);
    }
}

/* Exhausted tape: a $16 placed JUST AFTER the end of the buffer must not be
 * taken for a sync (before 2.11.2, tapebuf[tapelen] was read). */
TEST(test_exhausted_tape_ignores_byte_past_end) {
    memset(tape, 0x16, sizeof tape);                /* $16 everywhere, including after */
    const rom_patches_t* p = load(ORIC_MODEL_ATMOS, 12);
    emu.tapeoffs = 12;                              /* everything has been read */
    emu.cpu.PC = p->getsync_entry;
    tape_patches(&emu);
    ASSERT_TRUE(emu.cpu.PC == p->getsync_entry);    /* no invented sync */
    /* Sync recovery on an exhausted tape: the tape is over. */
    emu.tape_syncstack = 0xE0;
    emu.cpu.PC = p->getsync_loop;
    tape_patches(&emu);
    ASSERT_TRUE(!emu.tape_loaded);
    ASSERT_TRUE(emu.cpu.SP == 0xE0);
}

int main(void) {
    printf("=== Chargement cassette natif (tape_patches) ===\n");
    log_init(LOG_LEVEL_ERROR);
    memory_init(&emu.memory);
    RUN(test_getsync_skips_noise_to_first_sync);
    RUN(test_readbyte_feeds_rom);
    RUN(test_exhausted_tape_ignores_byte_past_end);
    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
