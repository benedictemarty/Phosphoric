/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_coverage.c
 * @brief Unified coverage test — exercises low-coverage code paths
 *
 * This test file targets functions with low coverage from other test suites:
 * keyboard, microdisc, sedoric, debugger, disk (FDC), memory banking, renderer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "io/keyboard.h"
#include "io/microdisc.h"
#include "storage/sedoric.h"
#include "storage/disk.h"
#include "debugger.h"
#include "emulator.h"
#include "utils/logging.h"

/* ─── Test framework ─── */
static int tests_run = 0, tests_passed = 0;
#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  [%d] %-50s ", ++tests_run, #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)
#define ASSERT_TRUE(x) do { if (!(x)) { printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #x); exit(1); } } while(0)
#define ASSERT_EQ(a,b) do { if ((a)!=(b)) { printf("FAIL (%s:%d: %s != %s → %d != %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b)); exit(1); } } while(0)

/* ═══════════ Keyboard tests ═══════════ */

TEST(test_keyboard_init_reset) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    /* All keys released = all bits 0xFF */
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);

    oric_keyboard_reset(&kb);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
}

TEST(test_keyboard_set_layout) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    oric_keyboard_set_layout(&kb, ORIC_KB_AZERTY);
    ASSERT_EQ(kb.layout, ORIC_KB_AZERTY);
    oric_keyboard_set_layout(&kb, ORIC_KB_QWERTY);
    ASSERT_EQ(kb.layout, ORIC_KB_QWERTY);
}

TEST(test_keyboard_press_char) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* Press 'A' */
    bool ok = oric_keyboard_press_char(&kb, 'A');
    ASSERT_TRUE(ok);
    /* At least one matrix byte should differ from 0xFF */
    bool changed = false;
    for (int i = 0; i < 8; i++)
        if (kb.matrix[i] != 0xFF) changed = true;
    ASSERT_TRUE(changed);

    /* Release all */
    oric_keyboard_release_all(&kb);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(kb.matrix[i], 0xFF);
}

TEST(test_keyboard_press_return) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);
    bool ok = oric_keyboard_press_char(&kb, '\n');
    ASSERT_TRUE(ok);
    bool changed = false;
    for (int i = 0; i < 8; i++)
        if (kb.matrix[i] != 0xFF) changed = true;
    ASSERT_TRUE(changed);
    oric_keyboard_release_all(&kb);
}

TEST(test_keyboard_press_various_chars) {
    oric_keyboard_t kb;
    oric_keyboard_init(&kb);

    /* Test various characters */
    const char* chars = "0123456789abcdefghijklmnopqrstuvwxyz .,;:!?+-*/=<>()";
    for (int i = 0; chars[i]; i++) {
        oric_keyboard_press_char(&kb, chars[i]);
        oric_keyboard_release_all(&kb);
    }
    /* Invalid char */
    bool ok = oric_keyboard_press_char(&kb, (char)200);
    ASSERT_TRUE(!ok);
}

/* ═══════════ Microdisc tests ═══════════ */

TEST(test_microdisc_init_reset) {
    microdisc_t md;
    microdisc_init(&md);
    ASSERT_EQ(md.drq, 0x80);
    ASSERT_EQ(md.intrq, 0x80);
    ASSERT_EQ(md.drive, 0);

    microdisc_reset(&md);
    ASSERT_EQ(md.drq, 0x80);
}

TEST(test_microdisc_read_write_ctrl) {
    microdisc_t md;
    microdisc_init(&md);

    /* Write control register */
    microdisc_write(&md, MICRODISC_CTRL, 0x62); /* drive 1, ROMDIS */
    uint8_t status = microdisc_read(&md, MICRODISC_CTRL);
    (void)status; /* INTRQ status */

    /* Read DRQ: bit 7 = /DRQ (active low), bits 6:0 = 1 */
    uint8_t drq = microdisc_read(&md, MICRODISC_DRQ);
    ASSERT_EQ(drq, 0xFF); /* DRQ inactive: 0x80 | 0x7F = 0xFF */
}

TEST(test_microdisc_fdc_access) {
    microdisc_t md;
    microdisc_init(&md);

    /* Write to FDC registers through Microdisc */
    microdisc_write(&md, MICRODISC_FDC_BASE + FDC_TRACK, 5);
    uint8_t track = microdisc_read(&md, MICRODISC_FDC_BASE + FDC_TRACK);
    ASSERT_EQ(track, 5);

    microdisc_write(&md, MICRODISC_FDC_BASE + FDC_SECTOR, 3);
    uint8_t sector = microdisc_read(&md, MICRODISC_FDC_BASE + FDC_SECTOR);
    ASSERT_EQ(sector, 3);

    microdisc_write(&md, MICRODISC_FDC_BASE + FDC_DATA, 0xAB);
    uint8_t data = microdisc_read(&md, MICRODISC_FDC_BASE + FDC_DATA);
    ASSERT_EQ(data, 0xAB);
}

TEST(test_microdisc_set_disk) {
    microdisc_t md;
    microdisc_init(&md);

    /* Create a small disk image */
    uint8_t disk_data[SEDORIC_SECTOR_SIZE * 17 * 42];
    memset(disk_data, 0, sizeof(disk_data));

    microdisc_set_disk(&md, 0, disk_data, sizeof(disk_data), 42, 17);

    /* Select drive 0 and issue a restore command */
    microdisc_write(&md, MICRODISC_CTRL, 0x00); /* drive 0 */
    microdisc_write(&md, MICRODISC_FDC_BASE + FDC_COMMAND, 0x00); /* RESTORE */

    /* Tick FDC */
    for (int i = 0; i < 100; i++)
        fdc_ticktock(&md.fdc, 1);

    microdisc_cleanup(&md);
}

TEST(test_microdisc_drive_select) {
    microdisc_t md;
    microdisc_init(&md);

    /* Select drive 0 */
    microdisc_write(&md, MICRODISC_CTRL, 0x00);
    ASSERT_EQ(md.drive, 0);

    /* Select drive 1 (bits 5-6 = 01) */
    microdisc_write(&md, MICRODISC_CTRL, 0x20);
    ASSERT_EQ(md.drive, 1);

    /* Select drive 2 */
    microdisc_write(&md, MICRODISC_CTRL, 0x40);
    ASSERT_EQ(md.drive, 2);

    /* Select drive 3 */
    microdisc_write(&md, MICRODISC_CTRL, 0x60);
    ASSERT_EQ(md.drive, 3);

    microdisc_cleanup(&md);
}

/* ═══════════ FDC disk tests ═══════════ */

TEST(test_fdc_seek_step) {
    fdc_t fdc;
    fdc_init(&fdc);

    uint8_t disk_data[SEDORIC_SECTOR_SIZE * 17 * 42];
    memset(disk_data, 0xE5, sizeof(disk_data));
    fdc_set_disk(&fdc, disk_data, sizeof(disk_data));

    /* RESTORE (seek track 0) */
    fdc_write(&fdc, FDC_COMMAND, 0x00);
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);
    ASSERT_EQ(fdc.track, 0);

    /* SEEK to track 10 */
    fdc_write(&fdc, FDC_DATA, 10);
    fdc_write(&fdc, FDC_COMMAND, 0x10); /* SEEK */
    for (int i = 0; i < 5000; i++) fdc_ticktock(&fdc, 1);

    /* STEP IN */
    fdc_write(&fdc, FDC_COMMAND, 0x40); /* STEP IN */
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);

    /* STEP OUT */
    fdc_write(&fdc, FDC_COMMAND, 0x60); /* STEP OUT */
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);

    /* STEP */
    fdc_write(&fdc, FDC_COMMAND, 0x20); /* STEP */
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);
}

TEST(test_fdc_read_sector) {
    fdc_t fdc;
    fdc_init(&fdc);

    /* Create disk with known data */
    uint8_t disk_data[SEDORIC_SECTOR_SIZE * 17 * 42];
    memset(disk_data, 0, sizeof(disk_data));
    /* Write pattern in track 0, sector 1 */
    for (int i = 0; i < 256; i++)
        disk_data[i] = (uint8_t)i;
    fdc_set_disk(&fdc, disk_data, sizeof(disk_data));

    /* Restore to track 0 */
    fdc_write(&fdc, FDC_COMMAND, 0x00);
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);

    /* Read sector 1 */
    fdc_write(&fdc, FDC_SECTOR, 1);
    fdc_write(&fdc, FDC_COMMAND, 0x80); /* READ SECTOR */
    for (int i = 0; i < 5000; i++) {
        fdc_ticktock(&fdc, 1);
        if (!(fdc.status & FDC_ST_BUSY)) break;
        if (fdc.status & FDC_ST_DRQ)
            (void)fdc_read(&fdc, FDC_DATA);
    }
}

TEST(test_fdc_write_sector) {
    fdc_t fdc;
    fdc_init(&fdc);

    uint8_t disk_data[SEDORIC_SECTOR_SIZE * 17 * 42];
    memset(disk_data, 0, sizeof(disk_data));
    fdc_set_disk(&fdc, disk_data, sizeof(disk_data));

    /* Restore to track 0 */
    fdc_write(&fdc, FDC_COMMAND, 0x00);
    for (int i = 0; i < 500; i++) fdc_ticktock(&fdc, 1);

    /* Write sector 1 */
    fdc_write(&fdc, FDC_SECTOR, 1);
    fdc_write(&fdc, FDC_COMMAND, 0xA0); /* WRITE SECTOR */
    for (int i = 0; i < 5000; i++) {
        fdc_ticktock(&fdc, 1);
        if (!(fdc.status & FDC_ST_BUSY)) break;
        if (fdc.status & FDC_ST_DRQ)
            fdc_write(&fdc, FDC_DATA, (uint8_t)(i & 0xFF));
    }
}

/* ═══════════ Sedoric tests ═══════════ */

TEST(test_sedoric_create_destroy) {
    sedoric_disk_t* disk = sedoric_create();
    ASSERT_TRUE(disk != NULL);
    ASSERT_EQ(disk->tracks, SEDORIC_TRACKS);
    ASSERT_EQ(disk->sectors, SEDORIC_SECTORS);

    /* Read/write sector */
    uint8_t buf[256];
    memset(buf, 0xAA, 256);
    bool ok = sedoric_write_sector(disk, 0, 1, buf);
    ASSERT_TRUE(ok);

    uint8_t buf2[256];
    ok = sedoric_read_sector(disk, 0, 1, buf2);
    ASSERT_TRUE(ok);
    ASSERT_EQ(memcmp(buf, buf2, 256), 0);

    /* Get sector pointer */
    uint8_t* ptr = sedoric_get_sector(disk, 0, 1);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_EQ(ptr[0], 0xAA);

    sedoric_destroy(disk);
}

TEST(test_sedoric_save_load) {
    sedoric_disk_t* disk = sedoric_create();
    ASSERT_TRUE(disk != NULL);

    /* Write pattern */
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    sedoric_write_sector(disk, 5, 3, buf);

    /* Save */
    bool ok = sedoric_save(disk, "/tmp/test_cov_sedoric.dsk");
    ASSERT_TRUE(ok);
    sedoric_destroy(disk);

    /* Load */
    sedoric_disk_t* disk2 = sedoric_load("/tmp/test_cov_sedoric.dsk");
    ASSERT_TRUE(disk2 != NULL);

    uint8_t buf2[256];
    ok = sedoric_read_sector(disk2, 5, 3, buf2);
    ASSERT_TRUE(ok);
    ASSERT_EQ(buf2[0], 0);
    ASSERT_EQ(buf2[255], 255);

    sedoric_destroy(disk2);
    remove("/tmp/test_cov_sedoric.dsk");
}

TEST(test_sedoric_invalid_load) {
    /* Non-existent file */
    sedoric_disk_t* disk = sedoric_load("/tmp/nonexistent_coverage_test.dsk");
    ASSERT_TRUE(disk == NULL);
}

/* ═══════════ Debugger tests ═══════════ */

TEST(test_debugger_breakpoints_full) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Add breakpoints up to max */
    for (int i = 0; i < DEBUGGER_MAX_BREAKPOINTS; i++) {
        int idx = debugger_add_breakpoint(&dbg, (uint16_t)(0x1000 + i));
        ASSERT_EQ(idx, i);
    }

    /* Adding one more should fail */
    int idx = debugger_add_breakpoint(&dbg, 0x2000);
    ASSERT_EQ(idx, -1);

    /* Check is_breakpoint */
    ASSERT_TRUE(debugger_is_breakpoint(&dbg, 0x1000));
    ASSERT_TRUE(debugger_is_breakpoint(&dbg, 0x100F));
    ASSERT_TRUE(!debugger_is_breakpoint(&dbg, 0x2000));

    /* Remove breakpoints */
    ASSERT_TRUE(debugger_remove_breakpoint(&dbg, 0));
    ASSERT_TRUE(!debugger_is_breakpoint(&dbg, 0x1000));

    /* Remove invalid index */
    ASSERT_TRUE(!debugger_remove_breakpoint(&dbg, 99));
}

TEST(test_debugger_watchpoints_full) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Add watchpoints up to max */
    for (int i = 0; i < DEBUGGER_MAX_WATCHPOINTS; i++) {
        int idx = debugger_add_watchpoint(&dbg, (uint16_t)(0x200 + i));
        ASSERT_EQ(idx, i);
    }

    /* Adding one more should fail */
    int idx = debugger_add_watchpoint(&dbg, 0x300);
    ASSERT_EQ(idx, -1);

    /* Remove */
    ASSERT_TRUE(debugger_remove_watchpoint(&dbg, 0));
    ASSERT_TRUE(!debugger_remove_watchpoint(&dbg, 99));
}

TEST(test_debugger_should_break) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Create minimal emulator */
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);

    /* No breakpoints, step mode off -> no break */
    dbg.active = true;
    bool brk = debugger_should_break(&dbg, &emu);
    ASSERT_TRUE(!brk);

    /* Step mode -> should break */
    dbg.step_mode = true;
    brk = debugger_should_break(&dbg, &emu);
    ASSERT_TRUE(brk);
    dbg.step_mode = false;

    /* Set breakpoint at PC */
    emu.cpu.PC = 0x1234;
    debugger_add_breakpoint(&dbg, 0x1234);
    brk = debugger_should_break(&dbg, &emu);
    ASSERT_TRUE(brk);
}

/* ═══════════ Memory banking tests ═══════════ */

TEST(test_memory_banking_overlay) {
    memory_t mem;
    memory_init(&mem);

    /* Write to upper RAM (used by overlay) */
    mem.overlay_active = true;
    mem.rom_enabled = true;

    /* ROM read at $C000 with overlay -> should read overlay ROM */
    mem.upper_ram[0] = 0x42;
    uint8_t val = memory_read(&mem, 0xC000);
    /* With overlay, reads go to upper_ram */
    (void)val;

    /* Disable overlay */
    mem.overlay_active = false;
    mem.rom_enabled = true;
    mem.rom[0] = 0x99;
    val = memory_read(&mem, 0xC000);
    ASSERT_EQ(val, 0x99);

    /* Disable ROM — reads go to upper_ram */
    mem.rom_enabled = false;
    mem.upper_ram[0] = 0x55;
    val = memory_read(&mem, 0xC000);
    (void)val;
}

/* ═══════════ CPU additional opcode coverage ═══════════ */

TEST(test_cpu_bcd_operations) {
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);
    cpu_reset(&cpu);

    /* SED + ADC in decimal mode */
    mem.ram[0x0600] = 0xF8; /* SED */
    mem.ram[0x0601] = 0xA9; /* LDA #$15 */
    mem.ram[0x0602] = 0x15;
    mem.ram[0x0603] = 0x69; /* ADC #$27 */
    mem.ram[0x0604] = 0x27;
    mem.ram[0x0605] = 0xD8; /* CLD */
    mem.ram[0x0606] = 0x00; /* BRK */

    cpu.PC = 0x0600;
    for (int i = 0; i < 6; i++) cpu_step(&cpu);
    /* 0x15 + 0x27 = 0x42 in BCD */
    ASSERT_EQ(cpu.A, 0x42);

    /* SBC in decimal mode */
    mem.ram[0x0610] = 0xF8; /* SED */
    mem.ram[0x0611] = 0x38; /* SEC */
    mem.ram[0x0612] = 0xA9; /* LDA #$50 */
    mem.ram[0x0613] = 0x50;
    mem.ram[0x0614] = 0xE9; /* SBC #$18 */
    mem.ram[0x0615] = 0x18;
    mem.ram[0x0616] = 0xD8; /* CLD */

    cpu.PC = 0x0610;
    for (int i = 0; i < 5; i++) cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x32);
}

TEST(test_cpu_stack_operations) {
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);
    cpu_reset(&cpu);

    /* PHA, PLA, PHP, PLP */
    mem.ram[0x0600] = 0xA9; /* LDA #$42 */
    mem.ram[0x0601] = 0x42;
    mem.ram[0x0602] = 0x48; /* PHA */
    mem.ram[0x0603] = 0xA9; /* LDA #$00 */
    mem.ram[0x0604] = 0x00;
    mem.ram[0x0605] = 0x68; /* PLA */
    mem.ram[0x0606] = 0x08; /* PHP */
    mem.ram[0x0607] = 0x28; /* PLP */

    cpu.PC = 0x0600;
    for (int i = 0; i < 7; i++) cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42);
}

TEST(test_cpu_indirect_jmp_bug) {
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);
    cpu_reset(&cpu);

    /* JMP ($10FF) — page boundary bug: reads low from $10FF, high from $1000 */
    mem.ram[0x0600] = 0x6C; /* JMP ($10FF) */
    mem.ram[0x0601] = 0xFF;
    mem.ram[0x0602] = 0x10;
    mem.ram[0x10FF] = 0x34;
    mem.ram[0x1000] = 0x12; /* Bug: wraps within page */

    cpu.PC = 0x0600;
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234);
}

TEST(test_cpu_nmi) {
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);
    cpu_reset(&cpu);

    /* Set NMI vector */
    mem.rom[0x3FFA] = 0x00; /* $FFFA -> $0700 */
    mem.rom[0x3FFB] = 0x07;
    mem.ram[0x0700] = 0xEA; /* NOP at NMI handler */

    cpu.PC = 0x0600;
    mem.ram[0x0600] = 0xEA; /* NOP */
    cpu_nmi(&cpu);
    cpu_step(&cpu); /* Should take NMI */
    /* PC should be at NMI handler or executing from it */
}

/* ═══════════ Main ═══════════ */

int main(void) {
    log_init(LOG_LEVEL_ERROR); /* Suppress info messages */

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Coverage Improvement Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Keyboard */
    RUN(test_keyboard_init_reset);
    RUN(test_keyboard_set_layout);
    RUN(test_keyboard_press_char);
    RUN(test_keyboard_press_return);
    RUN(test_keyboard_press_various_chars);

    /* Microdisc */
    RUN(test_microdisc_init_reset);
    RUN(test_microdisc_read_write_ctrl);
    RUN(test_microdisc_fdc_access);
    RUN(test_microdisc_set_disk);
    RUN(test_microdisc_drive_select);

    /* FDC disk */
    RUN(test_fdc_seek_step);
    RUN(test_fdc_read_sector);
    RUN(test_fdc_write_sector);

    /* Sedoric */
    RUN(test_sedoric_create_destroy);
    RUN(test_sedoric_save_load);
    RUN(test_sedoric_invalid_load);

    /* Debugger */
    RUN(test_debugger_breakpoints_full);
    RUN(test_debugger_watchpoints_full);
    RUN(test_debugger_should_break);

    /* Memory banking */
    RUN(test_memory_banking_overlay);

    /* CPU additional */
    RUN(test_cpu_bcd_operations);
    RUN(test_cpu_stack_operations);
    RUN(test_cpu_indirect_jmp_bug);
    RUN(test_cpu_nmi);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed\n", tests_passed, tests_run);
    printf("═══════════════════════════════════════════════════════\n\n");

    log_cleanup();
    return (tests_passed == tests_run) ? 0 : 1;
}
