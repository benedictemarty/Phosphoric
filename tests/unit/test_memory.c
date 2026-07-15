/**
 * @file test_memory.c
 * @brief Comprehensive memory subsystem unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-alpha
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "memory/memory.h"

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
/*  BASIC READ/WRITE TESTS                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_memory_init) {
    memory_t mem;
    ASSERT_TRUE(memory_init(&mem));
    ASSERT_TRUE(mem.rom_enabled);
}

TEST(test_ram_read_write) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0x0100, 0x42);
    ASSERT_EQ(memory_read(&mem, 0x0100), 0x42);
}

TEST(test_zero_page) {
    memory_t mem;
    memory_init(&mem);
    for (uint16_t i = 0; i < 256; i++) {
        memory_write(&mem, i, (uint8_t)i);
    }
    for (uint16_t i = 0; i < 256; i++) {
        ASSERT_EQ(memory_read(&mem, i), (uint8_t)i);
    }
}

TEST(test_stack_page) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0x01FF, 0xAA);
    memory_write(&mem, 0x0100, 0xBB);
    ASSERT_EQ(memory_read(&mem, 0x01FF), 0xAA);
    ASSERT_EQ(memory_read(&mem, 0x0100), 0xBB);
}

TEST(test_word_read_write) {
    memory_t mem;
    memory_init(&mem);
    memory_write_word(&mem, 0x0050, 0x1234);
    ASSERT_EQ(memory_read(&mem, 0x0050), 0x34); /* Low byte first */
    ASSERT_EQ(memory_read(&mem, 0x0051), 0x12);
    ASSERT_EQ(memory_read_word(&mem, 0x0050), 0x1234);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ROM BANKING TESTS                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_rom_read) {
    memory_t mem;
    memory_init(&mem);
    /* Write to ROM area directly */
    mem.rom[0] = 0xAB; /* $C000 */
    ASSERT_EQ(memory_read(&mem, 0xC000), 0xAB);
}

TEST(test_rom_write_ignored) {
    memory_t mem;
    memory_init(&mem);
    mem.rom[0] = 0xAB;
    /* Write to ROM area should be ignored when ROM is enabled */
    memory_write(&mem, 0xC000, 0xFF);
    ASSERT_EQ(memory_read(&mem, 0xC000), 0xAB); /* Still ROM value */
}

TEST(test_rom_disabled_ram_overlay) {
    memory_t mem;
    memory_init(&mem);
    mem.rom[0] = 0xAB;
    /* When ROM disabled, writes go to rom[] overlay */
    mem.rom_enabled = false;
    memory_write(&mem, 0xC000, 0xFF);
    ASSERT_EQ(memory_read(&mem, 0xC000), 0xFF);
    /* Note: re-enabling ROM reads same array, so value is now 0xFF */
    /* This is by design - save/restore ROM before disabling if needed */
}

TEST(test_vectors_always_rom) {
    memory_t mem;
    memory_init(&mem);
    /* Set vector in ROM */
    mem.rom[0x3FFC] = 0x00; /* $FFFC */
    mem.rom[0x3FFD] = 0x02; /* $FFFD */
    /* Even with ROM disabled, vectors should come from ROM */
    mem.rom_enabled = false;
    ASSERT_EQ(memory_read(&mem, 0xFFFC), 0x00);
    ASSERT_EQ(memory_read(&mem, 0xFFFD), 0x02);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  I/O CALLBACK TESTS                                                */
/* ═══════════════════════════════════════════════════════════════════ */

static uint8_t test_io_val = 0;
static uint16_t test_io_addr = 0;

static uint8_t test_io_read(uint16_t addr, void* userdata) {
    (void)userdata;
    test_io_addr = addr;
    return test_io_val;
}

static void test_io_write(uint16_t addr, uint8_t val, void* userdata) {
    (void)userdata;
    test_io_addr = addr;
    test_io_val = val;
}

TEST(test_io_callbacks) {
    memory_t mem;
    memory_init(&mem);
    memory_set_io_callbacks(&mem, test_io_read, test_io_write, NULL);

    /* Write to I/O space */
    test_io_val = 0;
    memory_write(&mem, 0x0300, 0x42);
    ASSERT_EQ(test_io_val, 0x42);
    ASSERT_EQ(test_io_addr, 0x0300);

    /* Read from I/O space */
    test_io_val = 0xBE;
    ASSERT_EQ(memory_read(&mem, 0x0300), 0xBE);
}

TEST(test_io_range) {
    memory_t mem;
    memory_init(&mem);
    memory_set_io_callbacks(&mem, test_io_read, test_io_write, NULL);

    /* Test I/O range $0300-$03FF */
    test_io_val = 0;
    memory_write(&mem, 0x03FF, 0x99);
    ASSERT_EQ(test_io_val, 0x99);
    ASSERT_EQ(test_io_addr, 0x03FF);
}

TEST(test_no_io_callback) {
    memory_t mem;
    memory_init(&mem);
    /* Without I/O callbacks, reads from I/O space should return something (not crash) */
    uint8_t val = memory_read(&mem, 0x0300);
    (void)val; /* Just verify no crash */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TRACING TESTS                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

static int trace_count = 0;
static uint16_t trace_last_addr = 0;
static uint8_t trace_last_val = 0;
static mem_access_type_t trace_last_type = MEM_READ;

static void test_trace_cb(uint16_t addr, uint8_t val, mem_access_type_t type) {
    trace_count++;
    trace_last_addr = addr;
    trace_last_val = val;
    trace_last_type = type;
}

TEST(test_trace_read) {
    memory_t mem;
    memory_init(&mem);
    trace_count = 0;
    memory_set_trace(&mem, true, test_trace_cb);
    memory_write(&mem, 0x0050, 0xAA);
    memory_read(&mem, 0x0050);
    ASSERT_TRUE(trace_count >= 2);
    ASSERT_EQ(trace_last_addr, 0x0050);
    ASSERT_EQ(trace_last_val, 0xAA);
    ASSERT_EQ(trace_last_type, MEM_READ);
}

TEST(test_trace_write) {
    memory_t mem;
    memory_init(&mem);
    trace_count = 0;
    memory_set_trace(&mem, true, test_trace_cb);
    memory_write(&mem, 0x0060, 0xBB);
    ASSERT_EQ(trace_last_addr, 0x0060);
    ASSERT_EQ(trace_last_val, 0xBB);
    ASSERT_EQ(trace_last_type, MEM_WRITE);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  UTILITY TESTS                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_clear_ram) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0x0100, 0x42);
    memory_clear_ram(&mem, 0xFF);
    ASSERT_EQ(memory_read(&mem, 0x0100), 0xFF);
    ASSERT_EQ(memory_read(&mem, 0x0000), 0xFF);
}

TEST(test_get_ptr) {
    memory_t mem;
    memory_init(&mem);
    uint8_t* ptr = memory_get_ptr(&mem, 0x0100);
    ASSERT_TRUE(ptr != NULL);
    *ptr = 0x42;
    ASSERT_EQ(memory_read(&mem, 0x0100), 0x42);
}

TEST(test_get_ptr_invalid) {
    memory_t mem;
    memory_init(&mem);
    uint8_t* ptr = memory_get_ptr(&mem, 0xFFFF);
    ASSERT_TRUE(ptr == NULL);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MEMORY MAP BOUNDARY TESTS                                        */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_ram_boundary) {
    memory_t mem;
    memory_init(&mem);
    /* Write to last RAM address before I/O */
    memory_write(&mem, 0x02FF, 0xAA);
    ASSERT_EQ(memory_read(&mem, 0x02FF), 0xAA);
    /* Write just after I/O space */
    memory_write(&mem, 0x0400, 0xBB);
    ASSERT_EQ(memory_read(&mem, 0x0400), 0xBB);
}

TEST(test_screen_ram) {
    memory_t mem;
    memory_init(&mem);
    /* ORIC screen RAM at $BB80 (text) / $A000 (hires) */
    memory_write(&mem, 0xBB80, 0x41); /* 'A' */
    ASSERT_EQ(memory_read(&mem, 0xBB80), 0x41);
    memory_write(&mem, 0xA000, 0xFF);
    ASSERT_EQ(memory_read(&mem, 0xA000), 0xFF);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

/* US 6 — RAM stuck-bit fault injection. */
TEST(test_ram_stuck_bits) {
    memory_t mem;
    memory_init(&mem);
    memory_write(&mem, 0x1000, 0x00);

    /* bit0 stuck at 1 → reads back 0x01. */
    memory_set_stuck_bits(&mem, 0x00, 0x01);
    ASSERT_EQ(memory_read(&mem, 0x1000), 0x01);

    /* bit7 stuck at 0 → 0xFF reads back 0x7F. */
    memory_write(&mem, 0x1000, 0xFF);
    memory_set_stuck_bits(&mem, 0x80, 0x00);
    ASSERT_EQ(memory_read(&mem, 0x1000), 0x7F);

    /* Disabled → clean read. */
    memory_set_stuck_bits(&mem, 0x00, 0x00);
    ASSERT_EQ(memory_read(&mem, 0x1000), 0xFF);
}

int main(void) {
    printf("Running Memory tests...\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  Basic Read/Write:\n");
    RUN(test_memory_init);
    RUN(test_ram_read_write);
    RUN(test_zero_page);
    RUN(test_stack_page);
    RUN(test_word_read_write);

    printf("\n  ROM Banking:\n");
    RUN(test_rom_read);
    RUN(test_rom_write_ignored);
    RUN(test_rom_disabled_ram_overlay);
    RUN(test_vectors_always_rom);

    printf("\n  I/O Callbacks:\n");
    RUN(test_io_callbacks);
    RUN(test_io_range);
    RUN(test_no_io_callback);

    printf("\n  Tracing:\n");
    RUN(test_trace_read);
    RUN(test_trace_write);

    printf("\n  Utilities:\n");
    RUN(test_clear_ram);
    RUN(test_get_ptr);
    RUN(test_get_ptr_invalid);

    printf("\n  Memory Map:\n");
    RUN(test_ram_boundary);
    RUN(test_screen_ram);
    RUN(test_ram_stuck_bits);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
