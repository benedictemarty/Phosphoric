/**
 * @file test_debugger.c
 * @brief Debugger module unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-24
 * @version 1.1.0-alpha
 */

#define _POSIX_C_SOURCE 200809L   /* fileno, dup, dup2 under -std=c11 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "debugger.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
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
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
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

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 1: INIT STATE                                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_debugger_init) {
    debugger_t dbg;
    debugger_init(&dbg);

    ASSERT_FALSE(dbg.active);
    ASSERT_FALSE(dbg.step_mode);
    ASSERT_EQ(dbg.num_breakpoints, 0);
    ASSERT_EQ(dbg.num_watchpoints, 0);
    ASSERT_FALSE(dbg.watch_triggered);
    ASSERT_FALSE(dbg.has_temp_breakpoint);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 2: ADD/REMOVE BREAKPOINT                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_add_remove_breakpoint) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Add breakpoints */
    int idx0 = debugger_add_breakpoint(&dbg, 0x1234);
    ASSERT_EQ(idx0, 0);
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_EQ(dbg.breakpoints[0].addr, 0x1234);

    int idx1 = debugger_add_breakpoint(&dbg, 0xF42D);
    ASSERT_EQ(idx1, 1);
    ASSERT_EQ(dbg.num_breakpoints, 2);

    /* Duplicate returns existing index */
    int idx_dup = debugger_add_breakpoint(&dbg, 0x1234);
    ASSERT_EQ(idx_dup, 0);
    ASSERT_EQ(dbg.num_breakpoints, 2);

    /* Remove first breakpoint */
    ASSERT_TRUE(debugger_remove_breakpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_EQ(dbg.breakpoints[0].addr, 0xF42D);

    /* Remove remaining */
    ASSERT_TRUE(debugger_remove_breakpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_breakpoints, 0);

    /* Remove from empty list */
    ASSERT_FALSE(debugger_remove_breakpoint(&dbg, 0));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 3: BREAKPOINT LIMIT                                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_limit) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Fill all slots */
    for (int i = 0; i < DEBUGGER_MAX_BREAKPOINTS; i++) {
        int idx = debugger_add_breakpoint(&dbg, (uint16_t)(0x1000 + i));
        ASSERT_EQ(idx, i);
    }
    ASSERT_EQ(dbg.num_breakpoints, DEBUGGER_MAX_BREAKPOINTS);

    /* Try to add one more - should fail */
    int idx = debugger_add_breakpoint(&dbg, 0xFFFF);
    ASSERT_EQ(idx, -1);
    ASSERT_EQ(dbg.num_breakpoints, DEBUGGER_MAX_BREAKPOINTS);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 4: ADD/REMOVE WATCHPOINT                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_add_remove_watchpoint) {
    debugger_t dbg;
    debugger_init(&dbg);

    int idx0 = debugger_add_watchpoint(&dbg, 0x0400);
    ASSERT_EQ(idx0, 0);
    ASSERT_EQ(dbg.num_watchpoints, 1);
    ASSERT_EQ(dbg.watchpoints[0].addr, 0x0400);
    ASSERT_EQ(dbg.watchpoints[0].mode, WATCH_WRITE);

    int idx1 = debugger_add_watchpoint(&dbg, 0xBB80);
    ASSERT_EQ(idx1, 1);

    /* Duplicate (same addr + same mode) */
    int idx_dup = debugger_add_watchpoint(&dbg, 0x0400);
    ASSERT_EQ(idx_dup, 0);
    ASSERT_EQ(dbg.num_watchpoints, 2);

    /* Remove */
    ASSERT_TRUE(debugger_remove_watchpoint(&dbg, 0));
    ASSERT_EQ(dbg.num_watchpoints, 1);
    ASSERT_EQ(dbg.watchpoints[0].addr, 0xBB80);

    /* Watchpoint limit */
    debugger_init(&dbg);
    for (int i = 0; i < DEBUGGER_MAX_WATCHPOINTS; i++) {
        debugger_add_watchpoint(&dbg, (uint16_t)(0x200 + i));
    }
    int idx_full = debugger_add_watchpoint(&dbg, 0xFFFF);
    ASSERT_EQ(idx_full, -1);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 5: BREAKPOINT HIT                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_hit) {
    debugger_t dbg;
    debugger_init(&dbg);

    debugger_add_breakpoint(&dbg, 0xF42D);

    ASSERT_TRUE(debugger_is_breakpoint(&dbg, 0xF42D));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xF42E));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0000));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 6: BREAKPOINT MISS                                           */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_breakpoint_miss) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* No breakpoints set - nothing should match */
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0000));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xFFFF));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0xF42D));

    /* Add one breakpoint, check others don't match */
    debugger_add_breakpoint(&dbg, 0x1000);
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x1001));
    ASSERT_FALSE(debugger_is_breakpoint(&dbg, 0x0FFF));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 7: STEP MODE                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_step_mode) {
    debugger_t dbg;
    debugger_init(&dbg);

    /* Use a real emulator_t struct */
    emulator_t fake_emu;
    memset(&fake_emu, 0, sizeof(fake_emu));
    memory_init(&fake_emu.memory);
    cpu_init(&fake_emu.cpu, &fake_emu.memory);
    fake_emu.cpu.PC = 0x1000;

    /* Without step mode, no break (no breakpoints) */
    ASSERT_FALSE(debugger_should_break(&dbg, &fake_emu));

    /* With step mode, always break */
    dbg.step_mode = true;
    ASSERT_TRUE(debugger_should_break(&dbg, &fake_emu));

    memory_cleanup(&fake_emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 8: DISASSEMBLE AT PC                                         */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_disassemble_at_pc) {
    /* Setup CPU + memory with a known instruction */
    memory_t mem;
    cpu6502_t cpu;
    memory_init(&mem);
    cpu_init(&cpu, &mem);

    /* Write LDA #$42 at $1000 */
    memory_write(&mem, 0x1000, 0xA9);  /* LDA immediate */
    memory_write(&mem, 0x1001, 0x42);

    /* Write JMP $F42D at $1002 */
    memory_write(&mem, 0x1002, 0x4C);  /* JMP absolute */
    memory_write(&mem, 0x1003, 0x2D);
    memory_write(&mem, 0x1004, 0xF4);

    char buf[64];
    int bytes;

    /* Disassemble LDA #$42 */
    bytes = cpu_disassemble(&cpu, 0x1000, buf, sizeof(buf));
    ASSERT_EQ(bytes, 2);

    /* Disassemble JMP $F42D */
    bytes = cpu_disassemble(&cpu, 0x1002, buf, sizeof(buf));
    ASSERT_EQ(bytes, 3);

    memory_cleanup(&mem);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INLINE ASSEMBLER (a addr MNEMONIC [operand])                       */
/* ═══════════════════════════════════════════════════════════════════ */

/* Assemble one line and assert the emitted bytes at `addr`. */
static void asm_check(emulator_t* emu, debugger_t* dbg, const char* line,
                      uint16_t addr, const uint8_t* expect, int n) {
    debugger_repl_run_line(dbg, emu, line);
    for (int i = 0; i < n; i++) {
        uint8_t got = memory_read(&emu->memory, (uint16_t)(addr + i));
        if (got != expect[i]) {
            printf("FAIL\n    asm '%s': byte %d = 0x%02X, expected 0x%02X\n",
                   line, i, got, expect[i]);
            tests_failed++;
            return;
        }
    }
}

TEST(test_asm_addressing_modes) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* Immediate */
    asm_check(&emu, &dbg, "a 0400 LDA #$41", 0x0400, (uint8_t[]){0xA9, 0x41}, 2);
    /* Absolute (3+ hex digits or value > $FF) */
    asm_check(&emu, &dbg, "a 0402 STA $BB80", 0x0402, (uint8_t[]){0x8D, 0x80, 0xBB}, 3);
    /* Zero page (2 hex digits, ZP form exists) */
    asm_check(&emu, &dbg, "a 0405 LDA $42", 0x0405, (uint8_t[]){0xA5, 0x42}, 2);
    /* Zero page,X */
    asm_check(&emu, &dbg, "a 0407 LDA $42,X", 0x0407, (uint8_t[]){0xB5, 0x42}, 2);
    /* Absolute,X */
    asm_check(&emu, &dbg, "a 0409 LDA $1234,X", 0x0409, (uint8_t[]){0xBD, 0x34, 0x12}, 3);
    /* Absolute,Y */
    asm_check(&emu, &dbg, "a 040C LDA $1234,Y", 0x040C, (uint8_t[]){0xB9, 0x34, 0x12}, 3);
    /* Indexed indirect ($nn,X) */
    asm_check(&emu, &dbg, "a 040F LDA ($40,X)", 0x040F, (uint8_t[]){0xA1, 0x40}, 2);
    /* Indirect indexed ($nn),Y */
    asm_check(&emu, &dbg, "a 0411 LDA ($80),Y", 0x0411, (uint8_t[]){0xB1, 0x80}, 2);
    /* Indirect (JMP) */
    asm_check(&emu, &dbg, "a 0413 JMP ($FFFC)", 0x0413, (uint8_t[]){0x6C, 0xFC, 0xFF}, 3);
    /* Implicit */
    asm_check(&emu, &dbg, "a 0416 INX", 0x0416, (uint8_t[]){0xE8}, 1);
    /* Accumulator */
    asm_check(&emu, &dbg, "a 0417 ASL A", 0x0417, (uint8_t[]){0x0A}, 1);
    asm_check(&emu, &dbg, "a 0418 ASL", 0x0418, (uint8_t[]){0x0A}, 1);

    memory_cleanup(&emu.memory);
}

TEST(test_asm_branch_relative) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* Backward branch: BNE $0400 at $0405 → offset = 0400 - (0405+2) = -7 = 0xF9 */
    asm_check(&emu, &dbg, "a 0405 BNE $0400", 0x0405, (uint8_t[]){0xD0, 0xF9}, 2);
    /* Forward branch: BEQ $0410 at $0407 → offset = 0410 - (0407+2) = +7 = 0x07 */
    asm_check(&emu, &dbg, "a 0407 BEQ $0410", 0x0407, (uint8_t[]){0xF0, 0x07}, 2);

    memory_cleanup(&emu.memory);
}

TEST(test_asm_rejects_bad_input) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* Pre-fill target bytes; bad assembly must leave them untouched. */
    memory_write(&emu.memory, 0x0500, 0xEE);
    memory_write(&emu.memory, 0x0501, 0xEE);

    /* Invalid mnemonic */
    debugger_repl_run_line(&dbg, &emu, "a 0500 XYZ #$10");
    ASSERT_EQ(memory_read(&emu.memory, 0x0500), 0xEE);
    /* Branch out of range: target too far from PC */
    debugger_repl_run_line(&dbg, &emu, "a 0500 BNE $9000");
    ASSERT_EQ(memory_read(&emu.memory, 0x0500), 0xEE);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY SEARCH (find)                                               */
/* ═══════════════════════════════════════════════════════════════════ */

/* Run a command capturing stdout into `out` (fd save/restore — headless-safe). */
static void capture_cmd(emulator_t* emu, debugger_t* dbg,
                        const char* line, char* out, size_t out_size) {
    const char* tmp = "/tmp/phosphoric_find_test.txt";
    out[0] = '\0';
    fflush(stdout);
    int saved = dup(fileno(stdout));
    FILE* f = freopen(tmp, "w+", stdout);
    if (f) {
        debugger_repl_run_line(dbg, emu, line);
        fflush(stdout);
    }
    /* Restore the original stdout. */
    if (saved >= 0) {
        dup2(saved, fileno(stdout));
        close(saved);
        clearerr(stdout);
    }
    FILE* rf = fopen(tmp, "r");
    if (rf) {
        size_t got = fread(out, 1, out_size - 1, rf);
        out[got] = '\0';
        fclose(rf);
    }
    remove(tmp);
}

TEST(test_find_byte_pattern) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* Plant a unique 3-byte pattern in RAM. */
    memory_write(&emu.memory, 0x2000, 0xDE);
    memory_write(&emu.memory, 0x2001, 0xAD);
    memory_write(&emu.memory, 0x2002, 0xBE);

    char out[4096];
    capture_cmd(&emu, &dbg, "find DE AD BE", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "$2000") != NULL);
    ASSERT_TRUE(strstr(out, "match") != NULL);

    /* A pattern that does not occur. */
    capture_cmd(&emu, &dbg, "find 11 22 33 44 55", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "not found") != NULL);

    memory_cleanup(&emu.memory);
}

TEST(test_find_ascii_string) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    const char* s = "ORIC";
    for (int i = 0; s[i]; i++)
        memory_write(&emu.memory, (uint16_t)(0x3000 + i), (uint8_t)s[i]);

    char out[4096];
    capture_cmd(&emu, &dbg, "find \"ORIC\"", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "$3000") != NULL);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 1: READ / CHANGE WATCHPOINTS                                  */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_watchpoint_read_and_change) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    emu.control_mode = true;   /* suppress "*** WATCHPOINT ***" printf */
    debugger_t dbg;
    debugger_init(&dbg);

    /* Read watchpoint: a plain read must trigger, a write must not. */
    ASSERT_EQ(debugger_add_watchpoint_mode(&dbg, 0x2000, WATCH_READ), 0);
    debugger_install_watchpoint_trace(&dbg, &emu);

    (void)memory_read(&emu.memory, 0x2000);
    ASSERT_TRUE(dbg.watch_triggered);
    dbg.watch_triggered = false;

    memory_write(&emu.memory, 0x2000, 0x11);
    ASSERT_FALSE(dbg.watch_triggered);

    /* Change watchpoint: only a value change triggers. */
    debugger_init(&dbg);
    ASSERT_EQ(debugger_add_watchpoint_mode(&dbg, 0x2500, WATCH_CHANGE), 0);
    debugger_install_watchpoint_trace(&dbg, &emu);

    memory_write(&emu.memory, 0x2500, 0x42);   /* first write: baseline hit */
    ASSERT_TRUE(dbg.watch_triggered);
    dbg.watch_triggered = false;

    memory_write(&emu.memory, 0x2500, 0x42);   /* same value: no trigger */
    ASSERT_FALSE(dbg.watch_triggered);

    memory_write(&emu.memory, 0x2500, 0x43);   /* changed: trigger */
    ASSERT_TRUE(dbg.watch_triggered);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 2: COMPOUND (&&/||) BREAKPOINT CONDITIONS                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_compound_condition) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    emu.control_mode = true;
    debugger_t dbg;
    debugger_init(&dbg);

    char out[4096];
    capture_cmd(&emu, &dbg, "b 1000 if A==5 && X==3", out, sizeof(out));
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_TRUE(dbg.breakpoints[0].has_cond);
    ASSERT_EQ(dbg.breakpoints[0].cond.num_terms, 2);

    emu.cpu.PC = 0x1000;
    emu.cpu.A = 5; emu.cpu.X = 3;
    ASSERT_TRUE(debugger_should_break(&dbg, &emu));   /* both true */

    emu.cpu.X = 4;
    ASSERT_FALSE(debugger_should_break(&dbg, &emu));  /* AND fails */

    /* OR variant */
    debugger_init(&dbg);
    capture_cmd(&emu, &dbg, "b 1000 if A==9 || Y==7", out, sizeof(out));
    emu.cpu.A = 0; emu.cpu.Y = 7;
    ASSERT_TRUE(debugger_should_break(&dbg, &emu));   /* second term true */

    emu.cpu.Y = 0;
    ASSERT_FALSE(debugger_should_break(&dbg, &emu));

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 3: ITERATIVE MEMORY SEARCH (hunt)                             */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_hunt_narrow) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* RAM is zeroed; plant a unique value so only one cell matches. */
    memory_write(&emu.memory, 0x2000, 0x7B);

    char out[8192];
    capture_cmd(&emu, &dbg, "hunt", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "Hunt started") != NULL);

    capture_cmd(&emu, &dbg, "hunt 7B", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "candidate") != NULL);

    capture_cmd(&emu, &dbg, "hunt list", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "$2000") != NULL);

    capture_cmd(&emu, &dbg, "hunt clear", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "cleared") != NULL);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 4: MEMORY ⇄ FILE (save / load)                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_region) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    for (int i = 0; i < 4; i++)
        memory_write(&emu.memory, (uint16_t)(0x2000 + i), (uint8_t)(0xA0 + i));

    char out[4096];
    capture_cmd(&emu, &dbg, "save /tmp/phos_region_test.bin 2000 4", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "Wrote 4") != NULL);

    /* Wipe, then reload from the file. */
    for (int i = 0; i < 4; i++)
        memory_write(&emu.memory, (uint16_t)(0x2000 + i), 0);
    capture_cmd(&emu, &dbg, "load /tmp/phos_region_test.bin 2000", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "Loaded 4") != NULL);

    for (int i = 0; i < 4; i++)
        ASSERT_EQ(memory_read(&emu.memory, (uint16_t)(0x2000 + i)), (uint8_t)(0xA0 + i));

    remove("/tmp/phos_region_test.bin");
    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 6: SET VIA REGISTER                                           */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_set_via_register) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    via_init(&emu.via);
    debugger_t dbg;
    debugger_init(&dbg);

    /* DDRB (register 2) stores the written value directly. */
    char out[4096];
    capture_cmd(&emu, &dbg, "set via 2 FF", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "VIA[$2]") != NULL);
    ASSERT_EQ(via_read(&emu.via, 2), 0xFF);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  GAP 7: BINARY (%) NUMERIC LITERALS                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_binary_literal) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    char out[4096];
    /* %100000000000 == 0x800 */
    capture_cmd(&emu, &dbg, "b %100000000000", out, sizeof(out));
    ASSERT_EQ(dbg.num_breakpoints, 1);
    ASSERT_EQ(dbg.breakpoints[0].addr, 0x800);

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  EPIC 6 / US 2: BANK-AWARE INSPECTION                              */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_peek_bank_layers) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    debugger_t dbg;
    debugger_init(&dbg);

    /* Distinct values in the three layers behind $C000. */
    emu.memory.rom[0x0000] = 0xAA;         /* BASIC ROM at $C000 */
    emu.memory.upper_ram[0x0000] = 0x55;   /* RAM behind ROM at $C000 */

    /* ROM and RAM layers are read independently of current paging. */
    ASSERT_EQ(debugger_peek_bank(&emu, 0xC000, PEEK_ROM), 0xAA);
    ASSERT_EQ(debugger_peek_bank(&emu, 0xC000, PEEK_RAM), 0x55);

    /* RAM bank below $C000 reads the main RAM array. */
    memory_write(&emu.memory, 0x0200, 0x3C);
    ASSERT_EQ(debugger_peek_bank(&emu, 0x0200, PEEK_RAM), 0x3C);

    /* ROM bank has no meaning below $C000. */
    ASSERT_EQ(debugger_peek_bank(&emu, 0x0200, PEEK_ROM), 0x00);

    /* Overlay maps $E000-$FFFF; absent overlay reads back 0xFF there, 0x00 below. */
    ASSERT_EQ(debugger_peek_bank(&emu, 0xE000, PEEK_OVERLAY), 0xFF);
    ASSERT_EQ(debugger_peek_bank(&emu, 0xC000, PEEK_OVERLAY), 0x00);

    /* Parser + names round-trip. */
    peek_bank_t b;
    ASSERT_TRUE(debugger_parse_bank("rom", &b) && b == PEEK_ROM);
    ASSERT_TRUE(debugger_parse_bank("RAM", &b) && b == PEEK_RAM);
    ASSERT_TRUE(debugger_parse_bank("overlay", &b) && b == PEEK_OVERLAY);
    ASSERT_FALSE(debugger_parse_bank("bogus", &b));

    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Debugger Unit Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_debugger_init);
    RUN(test_add_remove_breakpoint);
    RUN(test_breakpoint_limit);
    RUN(test_add_remove_watchpoint);
    RUN(test_breakpoint_hit);
    RUN(test_breakpoint_miss);
    RUN(test_step_mode);
    RUN(test_disassemble_at_pc);
    RUN(test_asm_addressing_modes);
    RUN(test_asm_branch_relative);
    RUN(test_asm_rejects_bad_input);
    RUN(test_find_byte_pattern);
    RUN(test_find_ascii_string);
    RUN(test_watchpoint_read_and_change);
    RUN(test_compound_condition);
    RUN(test_hunt_narrow);
    RUN(test_save_load_region);
    RUN(test_set_via_register);
    RUN(test_binary_literal);
    RUN(test_peek_bank_layers);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
