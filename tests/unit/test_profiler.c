/**
 * @file test_profiler.c
 * @brief Unit tests for CPU performance profiler
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/cpu/cpu6502.h"
#include "../../include/memory/memory.h"
#include "../../include/utils/profiler.h"
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
        printf("FAIL\n    %s:%d: %lld != %lld\n", __FILE__, __LINE__, \
               (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════ */
/*  Helper: set up a minimal CPU+memory for profiler tests        */
/* ═══════════════════════════════════════════════════════════════ */

static void setup_cpu(cpu6502_t* cpu, memory_t* mem) {
    memory_init(mem);
    cpu_init(cpu, mem);

    /* Place a simple program at $0400:
     *   LDA #$42    (A9 42)  — 2 cycles
     *   TAX         (AA)     — 2 cycles
     *   INX         (E8)     — 2 cycles
     *   NOP         (EA)     — 2 cycles
     *   BRK         (00)     — 7 cycles
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
    cpu->P = 0x24;
    cpu->cycles = 0;
}

static const char* tmpfile_path = "/tmp/phosphoric_test_profiler.txt";

/* ═══════════════════════════════════════════════════════════════ */
/*  Tests                                                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_profiler_init) {
    cpu_profiler_t prof;
    profiler_init(&prof);
    ASSERT_TRUE(!prof.active);
    ASSERT_EQ(prof.total_instructions, 0);
    ASSERT_EQ(prof.total_cycles, 0);
    ASSERT_EQ(prof.addr_hits[0], 0);
    ASSERT_EQ(prof.addr_hits[0xFFFF], 0);
    ASSERT_EQ(prof.opcode_hits[0], 0);
}

TEST(test_profiler_start_stop) {
    cpu_profiler_t prof;
    profiler_init(&prof);
    ASSERT_TRUE(!prof.active);
    profiler_start(&prof);
    ASSERT_TRUE(prof.active);
    profiler_stop(&prof);
    ASSERT_TRUE(!prof.active);
}

TEST(test_profiler_record_instruction) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    /* Record LDA #$42 at $0400 */
    profiler_record_instruction(&prof, &cpu);
    ASSERT_EQ(prof.total_instructions, 1);
    ASSERT_EQ(prof.addr_hits[0x0400], 1);
    ASSERT_EQ(prof.opcode_hits[0xA9], 1);  /* LDA immediate = $A9 */
}

TEST(test_profiler_address_hotspots) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    /* Execute 3 instructions, recording each */
    profiler_record_instruction(&prof, &cpu);
    int step1 = cpu_step(&cpu);
    profiler_record_cycles(&prof, 0x0400, step1);

    profiler_record_instruction(&prof, &cpu);
    int step2 = cpu_step(&cpu);
    profiler_record_cycles(&prof, 0x0402, step2);

    profiler_record_instruction(&prof, &cpu);
    int step3 = cpu_step(&cpu);
    profiler_record_cycles(&prof, 0x0403, step3);

    ASSERT_EQ(prof.total_instructions, 3);
    ASSERT_EQ(prof.addr_hits[0x0400], 1);  /* LDA */
    ASSERT_EQ(prof.addr_hits[0x0402], 1);  /* TAX */
    ASSERT_EQ(prof.addr_hits[0x0403], 1);  /* INX */
    ASSERT_EQ(prof.addr_hits[0x0404], 0);  /* NOP not reached */

    ASSERT_TRUE(prof.addr_cycles[0x0400] > 0);
    ASSERT_TRUE(prof.total_cycles > 0);
    ASSERT_EQ(prof.total_cycles, (uint64_t)(step1 + step2 + step3));
}

TEST(test_profiler_opcode_histogram) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    /* Put two LDA #xx instructions in a row */
    mem.ram[0x0400] = 0xA9;  /* LDA # */
    mem.ram[0x0401] = 0x10;
    mem.ram[0x0402] = 0xA9;  /* LDA # */
    mem.ram[0x0403] = 0x20;
    mem.ram[0x0404] = 0xEA;  /* NOP */
    cpu.PC = 0x0400;

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    profiler_record_instruction(&prof, &cpu);
    cpu_step(&cpu);

    profiler_record_instruction(&prof, &cpu);
    cpu_step(&cpu);

    profiler_record_instruction(&prof, &cpu);

    ASSERT_EQ(prof.opcode_hits[0xA9], 2);  /* LDA immediate x2 */
    ASSERT_EQ(prof.opcode_hits[0xEA], 1);  /* NOP x1 */
    ASSERT_EQ(prof.opcode_hits[0x00], 0);  /* BRK not executed */
}

TEST(test_profiler_cycle_tracking) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    uint16_t pc = cpu.PC;
    profiler_record_instruction(&prof, &cpu);
    int step = cpu_step(&cpu);
    profiler_record_cycles(&prof, pc, step);

    ASSERT_EQ(prof.addr_cycles[0x0400], (uint32_t)step);
    ASSERT_EQ(prof.total_cycles, (uint64_t)step);
}

TEST(test_profiler_report_output) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    /* Profile a few instructions */
    for (int i = 0; i < 3; i++) {
        uint16_t pc = cpu.PC;
        profiler_record_instruction(&prof, &cpu);
        int step = cpu_step(&cpu);
        profiler_record_cycles(&prof, pc, step);
    }
    profiler_stop(&prof);

    ASSERT_TRUE(profiler_report_to_file(&prof, tmpfile_path));

    /* Read back and verify content */
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    ASSERT_TRUE(strstr(buf, "CPU Performance Profile") != NULL);
    ASSERT_TRUE(strstr(buf, "Total instructions") != NULL);
    ASSERT_TRUE(strstr(buf, "Total cycles") != NULL);
    ASSERT_TRUE(strstr(buf, "$0400") != NULL);
    ASSERT_TRUE(strstr(buf, "Opcode frequency") != NULL);
    ASSERT_TRUE(strstr(buf, "$A9") != NULL);  /* LDA immediate */

    unlink(tmpfile_path);
}

TEST(test_profiler_region_aggregate) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);   /* program lives at $0400 → User RAM region */

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);
    for (int i = 0; i < 4; i++) {   /* LDA/TAX/INX/NOP, avoid BRK */
        uint16_t pc = cpu.PC;
        profiler_record_instruction(&prof, &cpu);
        int step = cpu_step(&cpu);
        profiler_record_cycles(&prof, pc, step);
    }
    profiler_stop(&prof);

    ASSERT_TRUE(profiler_report_to_file(&prof, tmpfile_path));
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    ASSERT_TRUE(strstr(buf, "Execution by memory region") != NULL);
    ASSERT_TRUE(strstr(buf, "User RAM") != NULL);
    ASSERT_TRUE(strstr(buf, "RAM total") != NULL);
    ASSERT_TRUE(strstr(buf, "ROM total") != NULL);

    unlink(tmpfile_path);
}

TEST(test_profiler_inactive_noop) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    /* Not started — recording should be no-op */

    profiler_record_instruction(&prof, &cpu);
    profiler_record_cycles(&prof, 0x0400, 2);

    ASSERT_EQ(prof.total_instructions, 0);
    ASSERT_EQ(prof.total_cycles, 0);
    ASSERT_EQ(prof.addr_hits[0x0400], 0);
}

TEST(test_profiler_reset) {
    cpu6502_t cpu;
    memory_t mem;
    setup_cpu(&cpu, &mem);

    cpu_profiler_t prof;
    profiler_init(&prof);
    profiler_start(&prof);

    profiler_record_instruction(&prof, &cpu);
    profiler_record_cycles(&prof, 0x0400, 2);
    ASSERT_EQ(prof.total_instructions, 1);

    profiler_reset(&prof);
    ASSERT_EQ(prof.total_instructions, 0);
    ASSERT_EQ(prof.total_cycles, 0);
    ASSERT_EQ(prof.addr_hits[0x0400], 0);
    ASSERT_TRUE(prof.active);  /* Reset preserves active state */
}

TEST(test_profiler_report_invalid_file) {
    cpu_profiler_t prof;
    profiler_init(&prof);
    ASSERT_TRUE(!profiler_report_to_file(&prof, "/nonexistent/dir/file.txt"));
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Main                                                          */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    log_init(LOG_LEVEL_ERROR);

    printf("═══════════════════════════════════════════════════════\n");
    printf("  CPU Performance Profiler Tests\n");
    printf("═══════════════════════════════════════════════════════\n");

    RUN(test_profiler_init);
    RUN(test_profiler_start_stop);
    RUN(test_profiler_record_instruction);
    RUN(test_profiler_address_hotspots);
    RUN(test_profiler_opcode_histogram);
    RUN(test_profiler_cycle_tracking);
    RUN(test_profiler_report_output);
    RUN(test_profiler_region_aggregate);
    RUN(test_profiler_inactive_noop);
    RUN(test_profiler_reset);
    RUN(test_profiler_report_invalid_file);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed\n", tests_passed, tests_run);
    printf("═══════════════════════════════════════════════════════\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
