/**
 * @file test_gdbstub.c
 * @brief GDB Remote Serial Protocol stub — protocol unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * Exercises gdb_dispatch() (the pure, socket-free protocol core) against an
 * in-memory emulator: checksum, register read/write (g/G/p/P), memory
 * read/write (m/M), breakpoints (Z0/z0), continue/step actions, and the
 * qSupported / target.xml / vCont queries.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "network/gdbstub.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "debugger.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-52s", #name); \
    int _before = tests_failed; \
    name(); \
    if (tests_failed == _before) { tests_passed++; printf("PASS\n"); } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
                tests_failed++; return; } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((long)(a) != (long)(b)) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, \
               (long)(b), (long)(a)); tests_failed++; return; } \
} while(0)

#define ASSERT_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, \
               (b), (a)); tests_failed++; return; } \
} while(0)

/* Shared fixture. */
static emulator_t g_emu;
static gdb_stub_t g_stub;

static void setup(void) {
    memset(&g_emu, 0, sizeof(g_emu));
    memory_init(&g_emu.memory);
    cpu_init(&g_emu.cpu, &g_emu.memory);
    debugger_init(&g_emu.debugger);
    memset(&g_stub, 0, sizeof(g_stub));
    g_stub.stop_signal = 5;
}

static gdb_action_t run(const char* pkt, char* resp, size_t n) {
    return gdb_dispatch(&g_stub, &g_emu, pkt, resp, n);
}

/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_checksum) {
    /* 'OK' = 0x4F + 0x4B = 0x9A */
    ASSERT_EQ(gdb_checksum("OK", 2), 0x9A);
    ASSERT_EQ(gdb_checksum("", 0), 0);
}

TEST(test_halt_reason) {
    setup();
    char r[64];
    run("?", r, sizeof(r));
    ASSERT_STR(r, "S05");
    g_stub.stop_signal = 2;
    run("?", r, sizeof(r));
    ASSERT_STR(r, "S02");
}

TEST(test_read_registers) {
    setup();
    g_emu.cpu.A = 0x12; g_emu.cpu.X = 0x34; g_emu.cpu.Y = 0x56;
    g_emu.cpu.SP = 0x78; g_emu.cpu.PC = 0xABCD; g_emu.cpu.P = 0x9E;
    char r[64];
    run("g", r, sizeof(r));
    /* A X Y SP PClo PChi P → 12 34 56 78 CD AB 9E */
    ASSERT_STR(r, "12345678cdab9e");
}

TEST(test_write_registers) {
    setup();
    char r[64];
    run("G11223344ddcc55", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.cpu.A, 0x11);
    ASSERT_EQ(g_emu.cpu.X, 0x22);
    ASSERT_EQ(g_emu.cpu.Y, 0x33);
    ASSERT_EQ(g_emu.cpu.SP, 0x44);
    ASSERT_EQ(g_emu.cpu.PC, 0xCCDD);   /* dd cc little-endian */
    ASSERT_EQ(g_emu.cpu.P, 0x55);
}

TEST(test_single_register) {
    setup();
    g_emu.cpu.PC = 0x1234;
    char r[64];
    run("p4", r, sizeof(r));            /* PC */
    ASSERT_STR(r, "3412");              /* little-endian */
    run("P0=ab", r, sizeof(r));         /* A = 0xAB */
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.cpu.A, 0xAB);
}

TEST(test_memory_read_write) {
    setup();
    char r[64];
    run("M0400,3:a9418d", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(memory_read(&g_emu.memory, 0x0400), 0xA9);
    ASSERT_EQ(memory_read(&g_emu.memory, 0x0401), 0x41);
    ASSERT_EQ(memory_read(&g_emu.memory, 0x0402), 0x8D);
    run("m0400,3", r, sizeof(r));
    ASSERT_STR(r, "a9418d");
}

TEST(test_breakpoints) {
    setup();
    char r[64];
    ASSERT_EQ(g_emu.debugger.num_breakpoints, 0);
    run("Z0,c000,1", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.num_breakpoints, 1);
    ASSERT_EQ(g_emu.debugger.breakpoints[0].addr, 0xC000);
    /* Remove */
    run("z0,c000,1", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.num_breakpoints, 0);
}

TEST(test_watchpoint) {
    setup();
    char r[64];
    run("Z2,1234,1", r, sizeof(r));     /* write watchpoint */
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.num_watchpoints, 1);
    run("z2,1234,1", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.num_watchpoints, 0);

    /* Z3 = read watchpoint, Z4 = access watchpoint (new). */
    run("Z3,2000,1", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.watchpoints[0].mode, WATCH_READ);
    run("Z4,2100,1", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_EQ(g_emu.debugger.watchpoints[1].mode, WATCH_ACCESS);
    ASSERT_EQ(g_emu.debugger.num_watchpoints, 2);
}

TEST(test_continue_and_step_actions) {
    setup();
    char r[64];
    ASSERT_EQ(run("c", r, sizeof(r)), GDB_ACT_CONTINUE);
    ASSERT_EQ(run("s", r, sizeof(r)), GDB_ACT_STEP);
    /* c with address sets PC */
    run("cABCD", r, sizeof(r));
    ASSERT_EQ(g_emu.cpu.PC, 0xABCD);
}

TEST(test_detach_and_kill) {
    setup();
    char r[64];
    ASSERT_EQ(run("D", r, sizeof(r)), GDB_ACT_DETACH);
    ASSERT_STR(r, "OK");
    g_emu.running = true;
    ASSERT_EQ(run("k", r, sizeof(r)), GDB_ACT_DETACH);
    ASSERT_TRUE(!g_emu.running);         /* kill stops the main loop */
}

TEST(test_queries) {
    setup();
    char r[256];
    run("qSupported:multiprocess+", r, sizeof(r));
    ASSERT_TRUE(strstr(r, "PacketSize=") != NULL);
    ASSERT_TRUE(strstr(r, "qXfer:features:read+") != NULL);
    run("qAttached", r, sizeof(r));
    ASSERT_STR(r, "1");
    run("QStartNoAckMode", r, sizeof(r));
    ASSERT_STR(r, "OK");
    ASSERT_TRUE(g_stub.no_ack_mode);
}

TEST(test_target_xml) {
    setup();
    char r[1100];
    run("qXfer:features:read:target.xml:0,1000", r, sizeof(r));
    ASSERT_TRUE(r[0] == 'l');             /* whole doc fits → last chunk */
    ASSERT_TRUE(strstr(r, "mos6502") != NULL);
    ASSERT_TRUE(strstr(r, "<reg name=\"pc\"") != NULL);
}

TEST(test_vcont) {
    setup();
    char r[64];
    run("vCont?", r, sizeof(r));
    ASSERT_TRUE(strstr(r, "vCont;c") != NULL);
    ASSERT_EQ(run("vCont;c", r, sizeof(r)), GDB_ACT_CONTINUE);
    ASSERT_EQ(run("vCont;s:1", r, sizeof(r)), GDB_ACT_STEP);
}

TEST(test_unknown_packet_empty) {
    setup();
    char r[64];
    run("XYZ", r, sizeof(r));
    ASSERT_STR(r, "");                    /* unsupported → empty reply */
}

int main(void) {
    printf("\nGDB stub protocol tests\n\n");
    RUN(test_checksum);
    RUN(test_halt_reason);
    RUN(test_read_registers);
    RUN(test_write_registers);
    RUN(test_single_register);
    RUN(test_memory_read_write);
    RUN(test_breakpoints);
    RUN(test_watchpoint);
    RUN(test_continue_and_step_actions);
    RUN(test_detach_and_kill);
    RUN(test_queries);
    RUN(test_target_xml);
    RUN(test_vcont);
    RUN(test_unknown_packet_empty);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
