/**
 * @file test_rominfo.c
 * @brief Unit tests for ROM analysis tools
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/utils/rominfo.h"
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
/*  Helper: create a minimal test ROM                              */
/* ═══════════════════════════════════════════════════════════════ */

static uint8_t test_rom[ROM_SIZE];

static void setup_rom(void) {
    memset(test_rom, 0xFF, ROM_SIZE);

    /* Put some code at the start ($C000):
     *   LDA #$42     (A9 42)
     *   JSR $C010    (20 10 C0)
     *   JMP $C000    (4C 00 C0)
     */
    test_rom[0x0000] = 0xA9;  /* LDA # */
    test_rom[0x0001] = 0x42;
    test_rom[0x0002] = 0x20;  /* JSR $C010 */
    test_rom[0x0003] = 0x10;
    test_rom[0x0004] = 0xC0;
    test_rom[0x0005] = 0x4C;  /* JMP $C000 */
    test_rom[0x0006] = 0x00;
    test_rom[0x0007] = 0xC0;

    /* Subroutine at $C010: */
    test_rom[0x0010] = 0xEA;  /* NOP */
    test_rom[0x0011] = 0x60;  /* RTS */

    /* Another JSR $C010 at $C012 (right after the RTS so the instruction-aligned
     * linear sweep reaches it; with accurate NMOS opcode sizes the surrounding
     * $FF padding now decodes as 3-byte ISC ops and would otherwise swallow a
     * JSR placed mid-padding). */
    test_rom[0x0012] = 0x20;  /* JSR $C010 */
    test_rom[0x0013] = 0x10;
    test_rom[0x0014] = 0xC0;

    /* ASCII string "HELLO WORLD" at $C100 (ROM offset $0100) */
    const char* hello = "HELLO WORLD";
    memcpy(&test_rom[0x0100], hello, strlen(hello));
    test_rom[0x0100 + strlen(hello)] = 0x00;  /* null terminate (non-printable) */

    /* Hardware vectors at end of ROM:
     * $FFFA (NMI) = $C010
     * $FFFC (RESET) = $C000
     * $FFFE (IRQ) = $C010
     */
    test_rom[0x3FFA] = 0x10;  /* NMI low */
    test_rom[0x3FFB] = 0xC0;  /* NMI high */
    test_rom[0x3FFC] = 0x00;  /* RESET low */
    test_rom[0x3FFD] = 0xC0;  /* RESET high */
    test_rom[0x3FFE] = 0x10;  /* IRQ low */
    test_rom[0x3FFF] = 0xC0;  /* IRQ high */
}

static const char* tmpfile_path = "/tmp/phosphoric_test_rominfo.txt";

/* ═══════════════════════════════════════════════════════════════ */
/*  Tests                                                         */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_rominfo_init) {
    rom_analysis_t analysis;
    rominfo_init(&analysis);
    ASSERT_TRUE(!analysis.valid);
    ASSERT_EQ(analysis.target_count, 0);
    ASSERT_EQ(analysis.string_count, 0);
    ASSERT_EQ(analysis.vectors.reset, 0);
}

TEST(test_rominfo_vector_detection) {
    setup_rom();
    rom_analysis_t analysis;
    ASSERT_TRUE(rominfo_analyze(&analysis, test_rom, ROM_SIZE));
    ASSERT_TRUE(analysis.valid);

    ASSERT_EQ(analysis.vectors.reset, 0xC000);
    ASSERT_EQ(analysis.vectors.irq, 0xC010);
    ASSERT_EQ(analysis.vectors.nmi, 0xC010);
}

TEST(test_rominfo_jsr_targets) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    /* Should find $C010 as a JSR target with ref_count >= 2 */
    bool found_c010 = false;
    for (int i = 0; i < analysis.target_count; i++) {
        if (analysis.targets[i].address == 0xC010 && analysis.targets[i].is_jsr) {
            found_c010 = true;
            ASSERT_TRUE(analysis.targets[i].ref_count >= 2);
            break;
        }
    }
    ASSERT_TRUE(found_c010);
}

TEST(test_rominfo_jmp_targets) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    /* Should find $C000 as a JMP target */
    bool found_c000 = false;
    for (int i = 0; i < analysis.target_count; i++) {
        if (analysis.targets[i].address == 0xC000 && analysis.targets[i].is_jmp) {
            found_c000 = true;
            break;
        }
    }
    ASSERT_TRUE(found_c000);
}

TEST(test_rominfo_string_detection) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    /* Should find "HELLO WORLD" at $C100 */
    bool found_hello = false;
    for (int i = 0; i < analysis.string_count; i++) {
        if (analysis.strings[i].address == 0xC100) {
            found_hello = true;
            ASSERT_EQ(analysis.strings[i].length, 11);  /* "HELLO WORLD" = 11 chars */
            break;
        }
    }
    ASSERT_TRUE(found_hello);
}

TEST(test_rominfo_pattern_search) {
    setup_rom();

    /* Search for the pattern A9 42 (LDA #$42) */
    uint8_t pattern[] = {0xA9, 0x42};
    uint16_t results[8];
    int count = rominfo_find_pattern(test_rom, ROM_SIZE, pattern, 2, results, 8);

    ASSERT_TRUE(count >= 1);
    ASSERT_EQ(results[0], 0xC000);  /* Found at ROM base */
}

TEST(test_rominfo_usage_stats) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    ASSERT_EQ(analysis.usage.total_bytes, ROM_SIZE);
    ASSERT_TRUE(analysis.usage.ff_bytes > 0);   /* Most of test ROM is $FF */
    ASSERT_TRUE(analysis.usage.code_bytes > 0); /* Some instructions present */
}

TEST(test_rominfo_report_output) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    ASSERT_TRUE(rominfo_report_to_file(&analysis, test_rom, ROM_SIZE, tmpfile_path));

    /* Read back and verify content */
    FILE* fp = fopen(tmpfile_path, "r");
    ASSERT_TRUE(fp != NULL);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    ASSERT_TRUE(strstr(buf, "ROM Analysis Report") != NULL);
    ASSERT_TRUE(strstr(buf, "RESET: $C000") != NULL);
    ASSERT_TRUE(strstr(buf, "IRQ:   $C010") != NULL);
    ASSERT_TRUE(strstr(buf, "NMI:   $C010") != NULL);
    ASSERT_TRUE(strstr(buf, "HELLO WORLD") != NULL);
    ASSERT_TRUE(strstr(buf, "$C010") != NULL);

    unlink(tmpfile_path);
}

TEST(test_rominfo_empty_rom) {
    rom_analysis_t analysis;
    ASSERT_TRUE(!rominfo_analyze(&analysis, NULL, 0));
    ASSERT_TRUE(!analysis.valid);
}

TEST(test_rominfo_cross_references) {
    setup_rom();
    rom_analysis_t analysis;
    rominfo_analyze(&analysis, test_rom, ROM_SIZE);

    /* $C010 should be referenced by both JSR and as IRQ/NMI vector */
    /* At minimum, 2 JSR references from $C002 and $C012 */
    for (int i = 0; i < analysis.target_count; i++) {
        if (analysis.targets[i].address == 0xC010) {
            ASSERT_TRUE(analysis.targets[i].ref_count >= 2);
            ASSERT_TRUE(analysis.targets[i].is_jsr);
            break;
        }
    }

    /* Targets should be sorted by address */
    for (int i = 1; i < analysis.target_count; i++) {
        ASSERT_TRUE(analysis.targets[i].address >= analysis.targets[i-1].address);
    }
}

/* ═══════════════════════════════════════════════════════════════ */
/*  Main                                                          */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    log_init(LOG_LEVEL_ERROR);

    printf("═══════════════════════════════════════════════════════\n");
    printf("  ROM Analysis Tools Tests\n");
    printf("═══════════════════════════════════════════════════════\n");

    RUN(test_rominfo_init);
    RUN(test_rominfo_vector_detection);
    RUN(test_rominfo_jsr_targets);
    RUN(test_rominfo_jmp_targets);
    RUN(test_rominfo_string_detection);
    RUN(test_rominfo_pattern_search);
    RUN(test_rominfo_usage_stats);
    RUN(test_rominfo_report_output);
    RUN(test_rominfo_empty_rom);
    RUN(test_rominfo_cross_references);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed\n", tests_passed, tests_run);
    printf("═══════════════════════════════════════════════════════\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
