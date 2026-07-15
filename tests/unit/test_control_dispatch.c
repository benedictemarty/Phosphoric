/**
 * @file test_control_dispatch.c
 * @brief Unit tests for the transport-agnostic control dispatch (sprint 92).
 *
 * Exercises control_dispatch() through a buffer-mode control_sink_t, asserting
 * that command replies land in memory byte-for-byte identically to the
 * stdout protocol, and that the CONTINUE/RESUME/QUIT results are correct.
 * This is the HTTP-facing entry point for the future REST API (Epic 1), so
 * we test it directly rather than only through the stdin integration script.
 *
 * Author: bmarty <bmarty@mailo.com>
 */

#define _POSIX_C_SOURCE 200809L
#include "control.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ─── tiny test harness (project convention: per-file macros) ─────── */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name)  do { printf("  %-42s", #name); name(); } while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); \
                   tests_failed++; return; } \
} while (0)

#define ASSERT_STR_EQ(got, want) do { \
    if (strcmp((got), (want)) != 0) { \
        printf("[FAIL] %s:%d\n    got : \"%s\"\n    want: \"%s\"\n", \
               __FILE__, __LINE__, (got), (want)); \
        tests_failed++; return; } \
} while (0)

#define PASS() do { printf("[OK]\n"); tests_passed++; } while (0)

/* A zeroed emulator is enough for the commands we drive here (they touch
 * only the CPU registers and RAM/ROM arrays, which live inside the struct).
 * We only need to wire the CPU's memory pointer so reset/read/write route to
 * emu->memory rather than dereferencing a NULL. */
static emulator_t* fresh_emu(void) {
    emulator_t* emu = calloc(1, sizeof(emulator_t));
    cpu_init(&emu->cpu, &emu->memory);
    return emu;
}

/* Dispatch one command line into a fresh buffer sink; caller inspects s.buf. */
static control_result_t run_one(emulator_t* emu, control_sink_t* s,
                                const char* cmd) {
    char line[256];
    snprintf(line, sizeof(line), "%s", cmd);
    control_sink_init_buffer(s);
    return control_dispatch(emu, s, line);
}

/* ─── tests ───────────────────────────────────────────────────────── */

TEST(hello_advertises_caps) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "hello");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    /* Same shape as the stdout path: "OK server=... proto=1 caps=...\n". */
    ASSERT_TRUE(strncmp(s.buf, "OK server=phosphoric/", 21) == 0);
    ASSERT_TRUE(strstr(s.buf, "caps=") != NULL);
    ASSERT_TRUE(strstr(s.buf, "load-disk") != NULL);
    ASSERT_TRUE(s.buf[s.len - 1] == '\n');
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(regs_reflects_cpu_state) {
    emulator_t* emu = fresh_emu();
    emu->cpu.A = 0x12; emu->cpu.X = 0x34; emu->cpu.Y = 0x56;
    emu->cpu.SP = 0x78; emu->cpu.P = 0x9A; emu->cpu.PC = 0xBCDE;
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "regs");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf,
        "OK A=12 X=34 Y=56 SP=78 P=9A PC=BCDE cycles=0\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(set_then_regs_roundtrip) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "set pc 0502");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK\n");
    control_sink_free(&s);
    ASSERT_TRUE(emu->cpu.PC == 0x0502);
    free(emu);
    PASS();
}

TEST(write_then_read_zero_page) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "write 0010 AB CD EF");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK count=3\n");
    control_sink_free(&s);

    r = run_one(emu, &s, "read 0010 3");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK AB CD EF\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(unknown_command_is_error) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "frobnicate 1 2");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "ERR unknown command `frobnicate`\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(bad_arg_is_error) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "read");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "ERR read: usage `read <addr> <len> [cpu|ram|rom|overlay]`\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(continue_signals_resume) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "continue");
    ASSERT_TRUE(r == CONTROL_RESUME);
    ASSERT_STR_EQ(s.buf, "OK\n");
    ASSERT_TRUE(emu->debugger.step_mode == false);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(step_signals_resume_and_sets_mode) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "step");
    ASSERT_TRUE(r == CONTROL_RESUME);
    ASSERT_STR_EQ(s.buf, "OK\n");
    ASSERT_TRUE(emu->debugger.step_mode == true);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(quit_signals_quit) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "quit");
    ASSERT_TRUE(r == CONTROL_QUIT);
    ASSERT_STR_EQ(s.buf, "OK\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(keys_appends_with_escapes) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    /* "AB\nC" — backslash-n becomes RETURN (0x0A); 4 bytes queued. */
    control_result_t r = run_one(emu, &s, "keys AB\\nC");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK queued=4 pending=4\n");
    control_sink_free(&s);
    ASSERT_TRUE(emu->kbd_inject_len == 4);
    ASSERT_TRUE(emu->kbd_inject_buf[0] == 'A');
    ASSERT_TRUE(emu->kbd_inject_buf[1] == 'B');
    ASSERT_TRUE(emu->kbd_inject_buf[2] == '\n');   /* \n → RETURN byte */
    ASSERT_TRUE(emu->kbd_inject_buf[3] == 'C');
    /* A second batch appends (spaces preserved in the payload). */
    r = run_one(emu, &s, "keys X Y");
    ASSERT_STR_EQ(s.buf, "OK queued=3 pending=7\n");
    control_sink_free(&s);
    free(emu->kbd_inject_buf);
    free(emu);
    PASS();
}

TEST(keys_empty_is_error) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "keys");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "ERR keys: empty text\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(blank_line_is_noop) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_TRUE(s.len == 0);          /* nothing written */
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(buffer_sink_accumulates_across_calls) {
    /* The HTTP layer will reuse one buffer sink for a response; verify the
     * sink appends rather than overwrites. */
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_sink_init_buffer(&s);
    char l1[] = "regs";
    char l2[] = "reset";
    control_dispatch(emu, &s, l1);
    control_dispatch(emu, &s, l2);
    /* Two OK lines, back to back. */
    ASSERT_TRUE(strstr(s.buf, "OK A=00") == s.buf);
    ASSERT_TRUE(strstr(s.buf, "\nOK pc=") != NULL);
    control_sink_free(&s);
    free(emu);
    PASS();
}

/* ─── Sprint 97: parité debug REST (couche --control) ──────────────── */

TEST(watch_mode_read) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "watch 2000 r");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK id=0 addr=2000 mode=read\n");
    ASSERT_TRUE(emu->debugger.watchpoints[0].mode == WATCH_READ);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(break_conditional_compound) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "break 1000 if A==5 && X==3");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_TRUE(strncmp(s.buf, "OK id=0 addr=1000 cond=", 23) == 0);
    ASSERT_TRUE(emu->debugger.breakpoints[0].has_cond);
    ASSERT_TRUE(emu->debugger.breakpoints[0].cond.num_terms == 2);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(break_bad_condition_errors) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "break 1000 if bogus");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_TRUE(strncmp(s.buf, "ERR break: bad condition", 24) == 0);
    ASSERT_TRUE(emu->debugger.num_breakpoints == 0);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(set_via_register) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "set via 2 FF");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK via=2 val=FF\n");
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(hunt_seed_then_eq) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    /* Plant a unique byte, seed, then narrow to ==7B. */
    run_one(emu, &s, "write 2000 7B"); control_sink_free(&s);
    control_result_t r = run_one(emu, &s, "hunt");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK candidates=65536\n");
    control_sink_free(&s);
    r = run_one(emu, &s, "hunt eq 7B");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK candidates=1\n");
    control_sink_free(&s);
    run_one(emu, &s, "hunt clear"); control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(break_binary_literal) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    control_result_t r = run_one(emu, &s, "break %100000000000");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_TRUE(emu->debugger.breakpoints[0].addr == 0x800);
    control_sink_free(&s);
    free(emu);
    PASS();
}

TEST(save_load_mem_roundtrip) {
    emulator_t* emu = fresh_emu();
    control_sink_t s;
    run_one(emu, &s, "write 2000 A0 A1 A2 A3"); control_sink_free(&s);
    control_result_t r = run_one(emu, &s, "save-mem /tmp/phos_ctl_region.bin 2000 4");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK wrote=4 addr=2000\n");
    control_sink_free(&s);
    run_one(emu, &s, "write 2000 00 00 00 00"); control_sink_free(&s);
    r = run_one(emu, &s, "load-mem /tmp/phos_ctl_region.bin 2000");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK loaded=4 addr=2000\n");
    control_sink_free(&s);
    r = run_one(emu, &s, "read 2000 4");
    ASSERT_STR_EQ(s.buf, "OK A0 A1 A2 A3\n");
    control_sink_free(&s);
    remove("/tmp/phos_ctl_region.bin");
    free(emu);
    PASS();
}

TEST(read_bank_rom_vs_ram) {
    emulator_t* emu = fresh_emu();
    emu->memory.rom[0x0000] = 0xAA;         /* ROM at $C000 */
    emu->memory.upper_ram[0x0000] = 0x55;   /* RAM behind ROM at $C000 */
    control_sink_t s;

    control_result_t r = run_one(emu, &s, "read C000 1 rom");
    ASSERT_TRUE(r == CONTROL_CONTINUE);
    ASSERT_STR_EQ(s.buf, "OK AA\n");
    control_sink_free(&s);

    run_one(emu, &s, "read C000 1 ram");
    ASSERT_STR_EQ(s.buf, "OK 55\n");
    control_sink_free(&s);

    run_one(emu, &s, "read C000 1 bogus");
    ASSERT_TRUE(strncmp(s.buf, "ERR read: bad bank", 18) == 0);
    control_sink_free(&s);

    free(emu);
    PASS();
}

int main(void) {
    printf("=== control_dispatch unit tests ===\n");
    RUN(hello_advertises_caps);
    RUN(regs_reflects_cpu_state);
    RUN(set_then_regs_roundtrip);
    RUN(write_then_read_zero_page);
    RUN(unknown_command_is_error);
    RUN(bad_arg_is_error);
    RUN(continue_signals_resume);
    RUN(step_signals_resume_and_sets_mode);
    RUN(quit_signals_quit);
    RUN(keys_appends_with_escapes);
    RUN(keys_empty_is_error);
    RUN(blank_line_is_noop);
    RUN(buffer_sink_accumulates_across_calls);
    RUN(watch_mode_read);
    RUN(break_conditional_compound);
    RUN(break_bad_condition_errors);
    RUN(set_via_register);
    RUN(hunt_seed_then_eq);
    RUN(break_binary_literal);
    RUN(save_load_mem_roundtrip);
    RUN(read_bank_rom_vs_ram);
    printf("=== result: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
