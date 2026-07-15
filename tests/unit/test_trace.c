/**
 * @file test_trace.c
 * @brief Unit tests for CPU trace logging
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/cpu/cpu6502.h"
#include "../../include/memory/memory.h"
#include "../../include/utils/trace.h"
#include "../../include/utils/logging.h"

/* ═══════════════════════════════════════════════════════════════ */
/*  Test framework macros                                         */
/* ═══════════════════════════════════════════════════════════════ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  [%d] %-50s ", ++tests_run, #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %d != %d\n", __FILE__, __LINE__, (int)(a), (int)(b)); \
        exit(1); \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════ */
/*  Helper: set up a minimal CPU+memory for trace tests           */
/* ═══════════════════════════════════════════════════════════════ */

static void setup_cpu(cpu6502_t* cpu, memory_t* mem) {
    memory_init(mem);
    cpu_init(cpu, mem);

    /* Place a simple program at $0400:
     *   LDA #$42    (A9 42)
     *   TAX         (AA)
     *   INX         (E8)
     *   NOP         (EA)
     *   BRK         (00)
     */
    mem->ram[0x0400] = 0xA9;  /* LDA # */
    mem->ram[0x0401] = 0x42;
    mem->ram[0x0402] = 0xAA;  /* TAX */
    mem->ram[0x0403] = 0xE8;  /* INX */
    mem->ram[0x0404] = 0xEA;  /* NOP */
    mem->ram[0x0405] = 0x00;  /* BRK */

    cpu->PC = 0x0400;
    cpu->A = 0;
    cpu->X = 0;
    cpu->Y = 0;
    cpu->SP = 0xFD;
    cpu->P = 0x24;  /* I flag set, unused set */
    cpu->cycles = 0;
}

static const char* tmpfile_path = "/tmp/phosphoric_test_trace.log";

/* ═══════════════════════════════════════════════════════════════ */
/*  Tests                                                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_trace_init) {
    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(!trace.active);
    ASSERT_TRUE(trace.fp == NULL);
    ASSERT_EQ(trace.count, 0);
    ASSERT_EQ(trace.max_count, 0);
    ASSERT_TRUE(!trace.owns_fp);
}

TEST(test_trace_open_close) {
    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));
    ASSERT_TRUE(trace.active);
    ASSERT_TRUE(trace.fp != NULL);
    ASSERT_TRUE(trace.owns_fp);
    trace_close(&trace);
    ASSERT_TRUE(!trace.active);
    ASSERT_TRUE(trace.fp == NULL);
    unlink(tmpfile_path);
}

TEST(test_trace_open_invalid) {
    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(!trace_open(&trace, "/nonexistent/dir/file.log"));
    ASSERT_TRUE(!trace.active);
}

TEST(test_trace_attach) {
    cpu_trace_t trace;
    trace_init(&trace);

    FILE* fp = fopen(tmpfile_path, "w");
    ASSERT_TRUE(fp != NULL);

    trace_attach(&trace, fp);
    ASSERT_TRUE(trace.active);
    ASSERT_TRUE(!trace.owns_fp);  /* We own it, not the trace */

    trace_close(&trace);
    ASSERT_TRUE(!trace.active);
    /* fp is still valid — we own it */
    fclose(fp);
    unlink(tmpfile_path);
}

TEST(test_trace_log_instruction) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));

    /* Log one instruction: LDA #$42 at $0400 */
    trace_log_instruction(&trace, &cpu);
    ASSERT_EQ(trace.count, 1);

    trace_close(&trace);

    /* Read back and verify content */
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    char line[256];
    ASSERT_TRUE(fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);

    /* Line should contain PC=0400, opcode A9 42, LDA, and register values */
    ASSERT_TRUE(strstr(line, "0400") != NULL);
    ASSERT_TRUE(strstr(line, "A9 42") != NULL);
    ASSERT_TRUE(strstr(line, "LDA") != NULL);
    ASSERT_TRUE(strstr(line, "A=00") != NULL);
    ASSERT_TRUE(strstr(line, "SP=FD") != NULL);

    unlink(tmpfile_path);
}

TEST(test_trace_multiple_instructions) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));

    /* Execute and trace 3 instructions */
    trace_log_instruction(&trace, &cpu);
    cpu_step(&cpu);  /* LDA #$42 */

    trace_log_instruction(&trace, &cpu);
    cpu_step(&cpu);  /* TAX */

    trace_log_instruction(&trace, &cpu);
    cpu_step(&cpu);  /* INX */

    ASSERT_EQ(trace.count, 3);
    trace_close(&trace);

    /* Count lines in output */
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    int line_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) line_count++;
    fclose(fp);
    ASSERT_EQ(line_count, 3);

    unlink(tmpfile_path);
}

TEST(test_trace_max_count) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_trace_t trace;
    trace_init(&trace);
    trace_set_max(&trace, 2);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));

    /* Try to trace 4 instructions — only 2 should be logged */
    trace_log_instruction(&trace, &cpu);
    cpu_step(&cpu);

    trace_log_instruction(&trace, &cpu);
    cpu_step(&cpu);

    /* These should be silently ignored (trace closed after max) */
    trace_log_instruction(&trace, &cpu);
    trace_log_instruction(&trace, &cpu);

    /* Trace should have auto-closed */
    ASSERT_TRUE(!trace.active);

    /* Count lines */
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    int line_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) line_count++;
    fclose(fp);
    ASSERT_EQ(line_count, 2);

    unlink(tmpfile_path);
}

TEST(test_trace_register_state) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    /* Set specific register values */
    cpu.A = 0xAB;
    cpu.X = 0xCD;
    cpu.Y = 0xEF;
    cpu.SP = 0x80;
    cpu.P = 0x65;

    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));

    trace_log_instruction(&trace, &cpu);
    trace_close(&trace);

    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    char line[256];
    ASSERT_TRUE(fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);

    /* Verify register values appear in output */
    ASSERT_TRUE(strstr(line, "A=AB") != NULL);
    ASSERT_TRUE(strstr(line, "X=CD") != NULL);
    ASSERT_TRUE(strstr(line, "Y=EF") != NULL);
    ASSERT_TRUE(strstr(line, "SP=80") != NULL);
    ASSERT_TRUE(strstr(line, "P=65") != NULL);

    unlink(tmpfile_path);
}

TEST(test_trace_inactive_noop) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_trace_t trace;
    trace_init(&trace);

    /* Logging when inactive should be a no-op */
    trace_log_instruction(&trace, &cpu);
    ASSERT_EQ(trace.count, 0);

    /* Close when already inactive should be safe */
    trace_close(&trace);
    ASSERT_TRUE(!trace.active);
}

TEST(test_trace_cycle_count) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);
    cpu.cycles = 12345678;

    cpu_trace_t trace;
    trace_init(&trace);
    ASSERT_TRUE(trace_open(&trace, tmpfile_path));

    trace_log_instruction(&trace, &cpu);
    trace_close(&trace);

    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    char line[256];
    ASSERT_TRUE(fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);

    /* Cycle count should appear at beginning of line */
    ASSERT_TRUE(strstr(line, "12345678") != NULL);

    unlink(tmpfile_path);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Conditional tracing (Epic 6 / US 1)                           */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_trace_start_pc_trigger) {
    cpu6502_t cpu; memory_t mem; setup_cpu(&cpu, &mem);
    cpu_trace_t t; trace_init(&t);
    trace_arm(&t, TRACE_START_PC, 0x0403, TRACE_STOP_NONE, 0, 0, 16, false);
    ASSERT_TRUE(!t.active && t.armed);

    cpu.PC = 0x0400; trace_log_instruction(&t, &cpu);  /* before trigger: ignored */
    ASSERT_EQ(trace_ring_count(&t), 0u);
    cpu.PC = 0x0403; trace_log_instruction(&t, &cpu);  /* trigger: starts here */
    cpu.PC = 0x0404; trace_log_instruction(&t, &cpu);
    ASSERT_TRUE(t.active);
    ASSERT_EQ(trace_ring_count(&t), 2u);
    trace_reset(&t); memory_cleanup(&mem);
}

TEST(test_trace_stop_cycle) {
    cpu6502_t cpu; memory_t mem; setup_cpu(&cpu, &mem);
    cpu_trace_t t; trace_init(&t);
    trace_arm(&t, TRACE_START_NOW, 0, TRACE_STOP_CYCLE, 0, 10, 16, false);
    cpu.cycles = 0;  trace_log_instruction(&t, &cpu);   /* start_cycle=0 */
    cpu.cycles = 5;  trace_log_instruction(&t, &cpu);   /* 5<10: keep going */
    ASSERT_TRUE(t.active);
    cpu.cycles = 10; trace_log_instruction(&t, &cpu);   /* 10>=10: stop after */
    ASSERT_TRUE(!t.active && t.stop_hit);
    ASSERT_EQ(trace_ring_count(&t), 3u);
    trace_reset(&t); memory_cleanup(&mem);
}

TEST(test_trace_stop_brk) {
    cpu6502_t cpu; memory_t mem; setup_cpu(&cpu, &mem);
    cpu_trace_t t; trace_init(&t);
    trace_arm(&t, TRACE_START_NOW, 0, TRACE_STOP_BRK, 0, 0, 16, false);
    cpu.PC = 0x0400; trace_log_instruction(&t, &cpu);   /* LDA: not BRK */
    ASSERT_TRUE(t.active);
    cpu.PC = 0x0405; trace_log_instruction(&t, &cpu);   /* BRK $00: stop */
    ASSERT_TRUE(!t.active && t.stop_hit);
    trace_reset(&t); memory_cleanup(&mem);
}

TEST(test_trace_stop_write) {
    cpu6502_t cpu; memory_t mem; setup_cpu(&cpu, &mem);
    cpu_trace_t t; trace_init(&t);
    trace_arm(&t, TRACE_START_NOW, 0, TRACE_STOP_WRITE, 0x1234, 0, 16, false);
    trace_note_mem_access(&t, 0x1234, 0);   /* a read: no stop */
    ASSERT_TRUE(t.active);
    trace_note_mem_access(&t, 0x1235, 1);   /* write to other addr: no stop */
    ASSERT_TRUE(t.active);
    trace_note_mem_access(&t, 0x1234, 1);   /* write to target: stop */
    ASSERT_TRUE(!t.active && t.stop_hit);
    trace_reset(&t); memory_cleanup(&mem);
}

TEST(test_trace_ring_wrap_and_save) {
    cpu6502_t cpu; memory_t mem; setup_cpu(&cpu, &mem);
    cpu_trace_t t; trace_init(&t);
    trace_arm(&t, TRACE_START_NOW, 0, TRACE_STOP_NONE, 0, 0, 3, false);
    for (int i = 0; i < 5; i++) { cpu.PC = 0x0400; cpu.cycles = i;
                                  trace_log_instruction(&t, &cpu); }
    ASSERT_EQ(trace_ring_count(&t), 3u);   /* capped at ring size */
    ASSERT_TRUE(trace_save_ring(&t, tmpfile_path));
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    int lines = 0; char line[256];
    while (fgets(line, sizeof(line), fp)) lines++;
    fclose(fp); unlink(tmpfile_path);
    ASSERT_EQ(lines, 3);
    trace_reset(&t); memory_cleanup(&mem);
}

TEST(test_trace_parse_spec) {
    trace_start_t sc; uint16_t spc; trace_stop_t st; uint16_t sa;
    uint64_t scy; uint32_t ring; bool sym;
    ASSERT_TRUE(trace_parse_spec("pc:E000 stop:write:BB80 ring:100 sym",
                                 &sc, &spc, &st, &sa, &scy, &ring, &sym));
    ASSERT_EQ(sc, TRACE_START_PC);
    ASSERT_EQ(spc, 0xE000);
    ASSERT_EQ(st, TRACE_STOP_WRITE);
    ASSERT_EQ(sa, 0xBB80);
    ASSERT_EQ(ring, 100u);
    ASSERT_TRUE(sym);
    ASSERT_TRUE(trace_parse_spec("stop:cycle:5000", &sc, &spc, &st, &sa, &scy, &ring, &sym));
    ASSERT_EQ(st, TRACE_STOP_CYCLE);
    ASSERT_EQ((unsigned)scy, 5000u);
    ASSERT_TRUE(!trace_parse_spec("bogustoken", &sc, &spc, &st, &sa, &scy, &ring, &sym));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Main                                                          */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    log_init(LOG_LEVEL_ERROR);  /* Suppress info/debug during tests */

    printf("═══════════════════════════════════════════════════════\n");
    printf("  CPU Trace Logging Tests\n");
    printf("═══════════════════════════════════════════════════════\n");

    RUN(test_trace_init);
    RUN(test_trace_open_close);
    RUN(test_trace_open_invalid);
    RUN(test_trace_attach);
    RUN(test_trace_log_instruction);
    RUN(test_trace_multiple_instructions);
    RUN(test_trace_max_count);
    RUN(test_trace_register_state);
    RUN(test_trace_inactive_noop);
    RUN(test_trace_cycle_count);
    RUN(test_trace_start_pc_trigger);
    RUN(test_trace_stop_cycle);
    RUN(test_trace_stop_brk);
    RUN(test_trace_stop_write);
    RUN(test_trace_ring_wrap_and_save);
    RUN(test_trace_parse_spec);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed\n", tests_passed, tests_run);
    printf("═══════════════════════════════════════════════════════\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
