/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_dormann.c
 * @brief Klaus Dormann's 6502 functional test (V2-S1)
 * @author bmarty <bmarty@mailo.com>
 *
 * Runs `6502_functional_test.bin` (Klaus2m5/6502_65C02_functional_tests):
 * ~30 million cycles exercising the 151 official opcodes, all addressing
 * modes, the flags, decimal mode and software interrupts.
 *
 * Complement to the 65x02 oracle (`test_cpu_cycles.c`): the latter measures
 * **cycle-by-cycle** conformance instruction by instruction, this one checks
 * the **functional** consistency of the core on a real, long program. Both
 * are V2 instruments (docs/specs/V2_CYCLE_ACCURACY.md).
 *
 * Test convention: the image loads at $0000, starts at $0400, and always
 * ends with a `jmp *` (jump to itself). The address of this trap tells
 * everything: $3469 = full success, any other address = the test caught
 * an error at that location (the .lst listing gives the exact line).
 *
 * The binary is not versioned: `tools/fetch_vectors.sh dormann`. When it is
 * absent, the suite goes to SKIP.
 *
 * Environment variables:
 *   DORMANN_DIR        directory of the binary (default third_party/vectors/dormann)
 *   DORMANN_MAX_CYCLES safety cap              (default 120 000 000)
 *   DORMANN_ENGINE     "legacy" (default) or "microseq" -- which core runs
 */

#include "cpu/cpu6502.h"
#include "cpu/microseq.h"
#include "memory/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Address of the success `jmp *`, read from 6502_functional_test.lst:
 *   3469 : 4c6934   >   jmp *      ;test passed, no errors
 * The test prints it on failure, so a change in the upstream binary is
 * diagnosed immediately instead of producing an opaque failure. */
#define DORMANN_SUCCESS_PC  0x3469
#define DORMANN_START_PC    0x0400

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  Running %s...\n", #name); name(); } while (0)
#define ASSERT_TRUE(cond) do { \
    if (cond) { tests_passed++; printf("    PASS: %s\n", #cond); } \
    else { tests_failed++; printf("    FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); } \
} while (0)
#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { tests_passed++; printf("    PASS: %s == %s\n", #a, #b); } \
    else { tests_failed++; printf("    FAIL: %s == %s ($%04llX != $%04llX) (%s:%d)\n", \
           #a, #b, _a, _b, __FILE__, __LINE__); } \
} while (0)

static memory_t  mem;
static cpu6502_t cpu;

/* Flat 64 KB: see test_cpu_cycles.c -- ROM disabled to make
 * $C000-$FFFF writable, and I/O $0300-$03FF redirected to RAM so that
 * it is not swallowed by the ORIC bus. */
static uint8_t flat_io_read(uint16_t addr, void* ud) {
    return ((memory_t*)ud)->ram[addr];
}
static void flat_io_write(uint16_t addr, uint8_t v, void* ud) {
    ((memory_t*)ud)->ram[addr] = v;
}

static bool load_image(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    static uint8_t img[65536];
    size_t n = fread(img, 1, sizeof(img), fp);
    fclose(fp);
    if (n != 65536) {
        printf("    image de taille inattendue : %zu octets (65536 attendus)\n", n);
        return false;
    }
    memory_init(&mem);
    mem.rom_enabled = false;
    mem.io_read = flat_io_read;
    mem.io_write = flat_io_write;
    mem.io_userdata = &mem;
    memcpy(mem.ram, img, 0xC000);
    memcpy(mem.rom, img + 0xC000, 0x4000);
    cpu_init(&cpu, &mem);
    {
        const char* engine = getenv("DORMANN_ENGINE");
        if (engine && strcmp(engine, "microseq") == 0) {
            cpu_set_microseq(&cpu, true);
            printf("  Cœur : micro-séquencé (V2-E1)\n");
        }
    }
    cpu.PC = DORMANN_START_PC;
    cpu.SP = 0xFD;
    cpu.P = FLAG_UNUSED | FLAG_INTERRUPT;
    return true;
}

static bool g_have_image = false;
static uint16_t g_trap_pc = 0xFFFF;
static uint64_t g_cycles = 0;
static bool g_trapped = false;

/* Runs until the `jmp *` (PC unchanged after an instruction) or until the
 * cycle cap. */
static void run_until_trap(uint64_t max_cycles) {
    uint16_t prev_pc = cpu.PC;
    while (cpu.cycles < max_cycles && !cpu.halted) {
        cpu_step(&cpu);
        if (cpu.PC == prev_pc) { g_trapped = true; break; }
        prev_pc = cpu.PC;
    }
    g_trap_pc = cpu.PC;
    g_cycles = cpu.cycles;
}

TEST(test_functional_test_reaches_success_trap) {
    ASSERT_TRUE(g_trapped);
    if (!g_trapped) {
        printf("    le test n'a atteint aucun piège en %llu cycles "
               "(PC=$%04X) — plafond trop bas, ou boucle inattendue\n",
               (unsigned long long)g_cycles, g_trap_pc);
        return;
    }
    if (g_trap_pc != DORMANN_SUCCESS_PC)
        printf("    piège atteint en $%04X : le test a détecté une erreur à cet\n"
               "    endroit (chercher cette adresse dans 6502_functional_test.lst)\n",
               g_trap_pc);
    ASSERT_EQ(g_trap_pc, DORMANN_SUCCESS_PC);
    printf("    succès en %llu cycles (%.1f s de temps ORIC simulé)\n",
           (unsigned long long)g_cycles, (double)g_cycles / 1000000.0);
}

TEST(test_cpu_did_not_halt) {
    /* A JAM hit here would indicate opcode decoding gone haywire. */
    ASSERT_TRUE(!cpu.halted);
}

int main(void) {
    printf("=== Test fonctionnel 6502 de Klaus Dormann ===\n\n");

    const char* dir = getenv("DORMANN_DIR");
    if (!dir || !*dir) dir = "third_party/vectors/dormann";
    const char* maxs = getenv("DORMANN_MAX_CYCLES");
    uint64_t max_cycles = maxs ? strtoull(maxs, NULL, 10) : 120000000ULL;

    char path[1024];
    snprintf(path, sizeof(path), "%s/6502_functional_test.bin", dir);
    g_have_image = load_image(path);

    if (!g_have_image) {
        printf("  SKIP : image absente (%s)\n", path);
        printf("  → tools/fetch_vectors.sh dormann   (~800 Ko)\n");
        printf("\n---\nTests passed: %d\nTests failed: %d\n", tests_passed, tests_failed);
        return 0;
    }

    printf("  Image : %s, départ $%04X, plafond %llu cycles\n",
           path, DORMANN_START_PC, (unsigned long long)max_cycles);
    run_until_trap(max_cycles);

    RUN(test_functional_test_reaches_success_trap);
    RUN(test_cpu_did_not_halt);

    printf("\n---\nTests passed: %d\nTests failed: %d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
