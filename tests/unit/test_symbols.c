/**
 * @file test_symbols.c
 * @brief Unit tests for symbol table loader
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/utils/symbols.h"
#include "../../include/utils/logging.h"

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
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", \
               __FILE__, __LINE__, _b, _a); \
        exit(1); \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (!a || strcmp(a, b) != 0) { \
        printf("FAIL\n    %s:%d: expected \"%s\", got \"%s\"\n", \
               __FILE__, __LINE__, b, a ? a : "(null)"); \
        exit(1); \
    } \
} while(0)

/* Returns a newly-allocated path (caller frees). Each call yields a
 * distinct file so callers can stack multiple temps. */
static char* write_tmp(const char* content) {
    char* path = malloc(64);
    strcpy(path, "/tmp/phosphoric_symtest_XXXXXX");
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    ssize_t wn = write(fd, content, strlen(content));
    (void)wn;
    close(fd);
    return path;
}

/* ── Format A: $XXXX NAME ──────────────────────────────────────── */
TEST(test_format_addr_first_dollar) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("$F900 RESET\n$E5BD RDBYTE\n");
    ASSERT_EQ(symbol_table_load(&t, path), 2);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF900), "RESET");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE5BD), "RDBYTE");
    unlink(path); free(path);
}

/* ── Format A with C/Python `0x` / `0X` prefix ─────────────────────
 * Regression check : sprint 34d1 fix. hex_parse used to return only the
 * digit count, while Format A's `after` calc added +1 for `$` but missed
 * the +2 for `0x`. Result : "0xF000 NAME" was silently dropped. */
TEST(test_format_addr_first_0x_prefix) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("0xF000 RESET\n0XE800 LOWER\n");
    ASSERT_EQ(symbol_table_load(&t, path), 2);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF000), "RESET");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE800), "LOWER");
    unlink(path); free(path);
}

/* ── Format A without dollar: XXXX NAME ────────────────────────── */
TEST(test_format_addr_first_no_dollar) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("F4D4 WAITKEY\n");
    ASSERT_EQ(symbol_table_load(&t, path), 1);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF4D4), "WAITKEY");
    unlink(path); free(path);
}

/* ── Format B: NAME = $XXXX ─────────────────────────────────────── */
TEST(test_format_equ) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("CLOAD_END = $E502\nFOO=$1234\n");
    ASSERT_EQ(symbol_table_load(&t, path), 2);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE502), "CLOAD_END");
    ASSERT_STR_EQ(symbol_lookup(&t, 0x1234), "FOO");
    unlink(path); free(path);
}

/* ── Format C: VICE / xa65 .lab ─────────────────────────────────── */
TEST(test_format_vice_lab) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("al C E59F .GETSYNC\nal D 1234 .ANOTHER\n");
    ASSERT_EQ(symbol_table_load(&t, path), 2);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE59F), "GETSYNC");
    ASSERT_STR_EQ(symbol_lookup(&t, 0x1234), "ANOTHER");
    unlink(path); free(path);
}

/* ── Mixed formats in same file ─────────────────────────────────── */
TEST(test_mixed_formats) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp(
        "; This is a comment\n"
        "$F900 RESET\n"
        "WAITKEY = $F4D4\n"
        "# hash comment too\n"
        "al C E5BD .RDBYTE\n"
        "\n"
        "  CSAVE_END  =  $E50A\n");
    ASSERT_EQ(symbol_table_load(&t, path), 4);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF900), "RESET");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF4D4), "WAITKEY");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE5BD), "RDBYTE");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE50A), "CSAVE_END");
    unlink(path); free(path);
}

/* ── Resolve by name (case-insensitive, leading dot/underscore) ── */
TEST(test_resolve_case_insensitive) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("$F900 RESET\n");
    symbol_table_load(&t, path);
    uint16_t addr = 0;
    ASSERT_TRUE(symbol_resolve(&t, "RESET", &addr)); ASSERT_EQ(addr, 0xF900);
    ASSERT_TRUE(symbol_resolve(&t, "reset", &addr)); ASSERT_EQ(addr, 0xF900);
    ASSERT_TRUE(symbol_resolve(&t, ".RESET", &addr)); ASSERT_EQ(addr, 0xF900);
    ASSERT_TRUE(symbol_resolve(&t, "_reset", &addr)); ASSERT_EQ(addr, 0xF900);
    ASSERT_TRUE(!symbol_resolve(&t, "NOPE", &addr));
    unlink(path); free(path);
}

/* ── Greedy hex bug regression: CLOAD_END must not become $C ────── */
TEST(test_regression_greedy_hex_C_prefix) {
    symbol_table_t t; symbol_table_init(&t);
    char* path = write_tmp("CLOAD_END = $E502\n");
    symbol_table_load(&t, path);
    /* If parser was greedy, addr would be $000C with name "LOAD_END" */
    ASSERT_TRUE(symbol_lookup(&t, 0x000C) == NULL);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE502), "CLOAD_END");
    unlink(path); free(path);
}

/* ── Missing file returns -1 ────────────────────────────────────── */
TEST(test_missing_file) {
    symbol_table_t t; symbol_table_init(&t);
    ASSERT_EQ(symbol_table_load(&t, "/tmp/does_not_exist_xyz.sym"), -1);
    ASSERT_EQ(t.count, 0);
}

/* ── Lookup miss returns NULL ───────────────────────────────────── */
TEST(test_lookup_miss) {
    symbol_table_t t; symbol_table_init(&t);
    ASSERT_TRUE(symbol_lookup(&t, 0x1234) == NULL);
}

/* ── Accumulation: multiple loads append ────────────────────────── */
TEST(test_multiple_loads_accumulate) {
    symbol_table_t t; symbol_table_init(&t);
    char* p1 = write_tmp("$F900 RESET\n");
    char* p2 = write_tmp("$E5BD RDBYTE\n");
    symbol_table_load(&t, p1);
    symbol_table_load(&t, p2);
    ASSERT_EQ(t.count, 2);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF900), "RESET");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE5BD), "RDBYTE");
    unlink(p1); unlink(p2); free(p1); free(p2);
}

/* US 4 — symbol groups: scope symbols on/off by group. */
TEST(test_symbol_groups) {
    symbol_table_t t; symbol_table_init(&t);
    char* pa = write_tmp("$F900 RESET\n");     /* group 1 (BASIC ROM-ish) */
    char* pb = write_tmp("$E000 DISKENTRY\n"); /* group 2 (overlay-ish) */
    ASSERT_EQ(symbol_table_load_group(&t, pa, 1), 1);
    ASSERT_EQ(symbol_table_load_group(&t, pb, 2), 1);
    ASSERT_EQ(t.count, 2);
    ASSERT_EQ(symbol_group_count(&t, 1), 1);
    ASSERT_EQ(symbol_group_count(&t, 2), 1);

    /* Both enabled by default. */
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF900), "RESET");
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE000), "DISKENTRY");

    /* Disable group 2 → its symbol disappears, group 1 stays. */
    symbol_set_group_enabled(&t, 2, false);
    ASSERT_TRUE(symbol_lookup(&t, 0xE000) == NULL);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xF900), "RESET");
    uint16_t a;
    ASSERT_TRUE(!symbol_resolve(&t, "DISKENTRY", &a));   /* resolve also filtered */
    ASSERT_TRUE(symbol_resolve(&t, "RESET", &a) && a == 0xF900);

    /* Re-enable. */
    symbol_set_group_enabled(&t, 2, true);
    ASSERT_STR_EQ(symbol_lookup(&t, 0xE000), "DISKENTRY");

    unlink(pa); unlink(pb); free(pa); free(pb);
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);
    printf("\n");
    printf("===========================================================\n");
    printf("  Phosphoric Symbol Table Tests\n");
    printf("===========================================================\n\n");

    RUN(test_format_addr_first_dollar);
    RUN(test_format_addr_first_0x_prefix);
    RUN(test_format_addr_first_no_dollar);
    RUN(test_format_equ);
    RUN(test_format_vice_lab);
    RUN(test_mixed_formats);
    RUN(test_resolve_case_insensitive);
    RUN(test_regression_greedy_hex_C_prefix);
    RUN(test_missing_file);
    RUN(test_lookup_miss);
    RUN(test_multiple_loads_accumulate);
    RUN(test_symbol_groups);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_run - tests_passed, tests_run);
    printf("===========================================================\n\n");

    return tests_passed == tests_run ? 0 : 1;
}
