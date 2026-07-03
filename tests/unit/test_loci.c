/**
 * @file test_loci.c
 * @brief Unit tests for LOCI emulation (Sprint 34y skeleton)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/emulator.h"   /* EMU_VERSION (test_uname release) */
#include "../../include/io/loci.h"
#include "../../include/io/loci_internal.h"   /* loci_tap_open/close (36f) */
#include "../../include/utils/logging.h"
#include "../../include/cpu/cpu6502.h"
#include "../../include/memory/memory.h"

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

/* ── lifecycle ───────────────────────────────────────────────── */

TEST(test_init_zeroes_state) {
    loci_t l; memset(&l, 0xFF, sizeof(l));
    ASSERT_TRUE(loci_init(&l));
    /* $03A0-$03AF cleared, $03B0-$03B9 seeded with released no-op stub
     * (matches firmware api_run() : released stub with A=X=SREG=0). */
    for (size_t i = 0; i < 0x10; i++) ASSERT_EQ(l.regs[i], 0);
    /* Seeded stub: CLV ; BVC +0 ; LDA #$00 ; LDX #$00 ; RTS ; sreg=0 */
    ASSERT_EQ(l.regs[0x10], 0xB8);   /* CLV */
    ASSERT_EQ(l.regs[0x11], 0x50);   /* BVC */
    ASSERT_EQ(l.regs[0x12], 0x00);   /* operand +0 / BUSY=0 */
    ASSERT_EQ(l.regs[0x13], 0xA9);   /* LDA # */
    ASSERT_EQ(l.regs[0x14], 0x00);   /* A */
    ASSERT_EQ(l.regs[0x15], 0xA2);   /* LDX # */
    ASSERT_EQ(l.regs[0x16], 0x00);   /* X */
    ASSERT_EQ(l.regs[0x17], 0x60);   /* RTS */
    ASSERT_EQ(l.regs[0x18], 0x00);   /* SREG lo */
    ASSERT_EQ(l.regs[0x19], 0x00);   /* SREG hi */
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

TEST(test_addr_in_mia_filter) {
    ASSERT_TRUE(loci_addr_in_mia(0x03A0));
    ASSERT_TRUE(loci_addr_in_mia(0x03AF));
    ASSERT_TRUE(loci_addr_in_mia(0x03BF));
    ASSERT_TRUE(!loci_addr_in_mia(0x039F));
    ASSERT_TRUE(!loci_addr_in_mia(0x03C0));
}

/* ── bus interface — disabled ────────────────────────────────── */

TEST(test_disabled_read_returns_ff) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    ASSERT_EQ(loci_read(&l, 0x03A0), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03AF), 0xFF);
}

TEST(test_disabled_write_is_noop) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.regs[LOCI_REG_API_OP], 0);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 0);
}

/* ── register R/W ────────────────────────────────────────────── */

TEST(test_write_read_passthrough) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* $03A0 is live since Sprint 36f (console flags) — use plain
     * storage registers for the passthrough check. */
    loci_write(&l, 0x03B8, 0x42);
    ASSERT_EQ(loci_read(&l, 0x03B8), 0x42);
    loci_write(&l, 0x03B4, 0xAB);
    ASSERT_EQ(loci_read(&l, 0x03B4), 0xAB);
}

TEST(test_out_of_range_read_ff) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(loci_read(&l, 0x0399), 0xFF);
    ASSERT_EQ(loci_read(&l, 0x03C0), 0xFF);
}

/* ── API dispatch ────────────────────────────────────────────── */

TEST(test_api_op_dispatches_and_sets_enosys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Sprint 34au has implemented OEM_CODEPAGE, so use an undefined op
     * code that hits the default-ENOSYS branch instead. $07 is not in
     * the LOCI ABI. */
    loci_write(&l, 0x03AF, 0x07);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
    ASSERT_EQ(l.op_count[0x07], 1);
}

TEST(test_api_op_none_does_not_dispatch) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_NONE);
    ASSERT_EQ(l.op_count[LOCI_OP_NONE], 0);
}

TEST(test_op_count_increments) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    for (int i = 0; i < 3; i++)
        loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    ASSERT_EQ(l.op_count[LOCI_OP_RNG_LRAND], 3);
}

/* ── xstack ─────────────────────────────────────────────────── */

TEST(test_xstack_push_pop_roundtrip) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AC, 0x10);  /* push 0x10 */
    loci_write(&l, 0x03AC, 0x20);  /* push 0x20 */
    /* Read returns top of stack — 0x20 */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x20);
}

/* ── 34z: system / RTC / RNG ─────────────────────────────────── */

TEST(test_op_rng_lrand_returns_axsreg) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_RNG_LRAND);
    /* errno should not be set; high bit cleared (firmware masks 0x7FFFFFFF) */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    uint8_t sreg_hi = l.regs[LOCI_REG_API_SREG_HI];
    ASSERT_TRUE((sreg_hi & 0x80) == 0);
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
}

TEST(test_op_clock_returns_uptime) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Sleep briefly so uptime isn't 0. */
    struct timespec ts = {0, 20 * 1000 * 1000};   /* 20 ms */
    nanosleep(&ts, NULL);
    loci_write(&l, 0x03AF, LOCI_OP_CLOCK);
    /* Read AXSREG (32-bit) — should be >= 1 (10 ms unit) */
    uint32_t v = l.regs[LOCI_REG_API_A] |
                 ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                 ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                 ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_TRUE(v >= 1);
}

TEST(test_op_clk_getres_pushes_one_sec) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0;   /* CLOCK_REALTIME */
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETRES);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);   /* ax = 0 (OK) */
    /* xstack must hold: uint32 sec=1, int32 nsec=0 — 8 bytes total */
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 8);
    /* Top of stack is the lo byte of sec (1). */
    ASSERT_EQ(l.xstack[l.xstack_ptr], 1);
}

TEST(test_op_clk_gettime_pushes_time) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETTIME);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 8);   /* uint32+int32 */
}

TEST(test_op_clk_getres_bad_id_returns_einval) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_CLK_GETRES);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EINVAL);
}

TEST(test_op_pix_xreg_empty_xstack_einval) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* No args on xstack — should return EINVAL (Sprint 34ag enforces shape). */
    loci_write(&l, 0x03AF, LOCI_OP_PIX_XREG);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EINVAL);
}

TEST(test_xstack_read_pops_byte) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AC, 0x10);
    loci_write(&l, 0x03AC, 0x20);
    /* Top is 0x20; reading pops it. */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x20);
    /* Next read returns 0x10. */
    ASSERT_EQ(loci_read(&l, 0x03AC), 0x10);
    /* Empty now. */
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

TEST(test_unimplemented_op_still_enosys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Sprint 34au implemented CPU_PHI2 etc. Use a genuinely undefined
     * opcode ($07) to verify the default-ENOSYS path is still reachable. */
    loci_write(&l, 0x03AF, 0x07);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
}

/* ── 34aa: File I/O POSIX subset ─────────────────────────────── */

#include <unistd.h>
#include <sys/stat.h>

/* Helper: push a null-terminated string onto the xstack (the way the 6502
 * does before calling open/unlink/rename). */
static void push_path(loci_t* l, const char* s) {
    size_t len = strlen(s);
    /* Push terminator first, then bytes in reverse so the string reads
     * forward starting at xstack[xstack_ptr]. */
    loci_write(l, 0x03AC, 0);   /* terminator */
    for (size_t i = len; i > 0; i--) {
        loci_write(l, 0x03AC, (uint8_t)s[i - 1]);
    }
}

/* Helper: push a uint16 (little-endian) onto the xstack. */
static void push_u16(loci_t* l, uint16_t v) {
    loci_write(l, 0x03AC, (uint8_t)(v >> 8));
    loci_write(l, 0x03AC, (uint8_t)(v & 0xFF));
}

static char* make_tmpdir(void) {
    char* d = malloc(64);
    strcpy(d, "/tmp/loci_test_XXXXXX");
    if (!mkdtemp(d)) { free(d); return NULL; }
    return d;
}

TEST(test_op_open_close_creates_file) {
    char* tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    push_path(&l, "foo.txt");
    l.regs[LOCI_REG_API_A] = LOCI_O_CREAT | LOCI_O_TRUNC | 1;  /* WRONLY+CREAT+TRUNC */
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* File should now exist on disk. */
    char path[300];
    snprintf(path, sizeof(path), "%s/foo.txt", tmpdir);
    ASSERT_EQ(access(path, F_OK), 0);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);

    unlink(path);
    rmdir(tmpdir);
    loci_cleanup(&l);
    free(tmpdir);
}

TEST(test_op_open_missing_returns_enoent) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "nope.bin");
    l.regs[LOCI_REG_API_A] = 0;   /* RDONLY */
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EFATFS(LOCI_FR_NO_FILE));   /* firmware: 32+FR_NO_FILE */
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_open_path_traversal_rejected) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, "/tmp");
    push_path(&l, "../etc/passwd");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EACCES);
    loci_cleanup(&l);
}

TEST(test_write_read_roundtrip) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    /* Open for writing. */
    push_path(&l, "data.bin");
    l.regs[LOCI_REG_API_A] = LOCI_O_CREAT | LOCI_O_TRUNC | 1;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* The 6502 writes a string by pushing it in REVERSE order so that
     * reading forward from xstack_ptr yields the string in order.
     * To write "HELLO" the 6502 pushes O,L,L,E,H. Unlike read_xstack,
     * write_xstack does NOT take an explicit count -- the firmware derives
     * it from how many bytes are on the xstack (XSTACK_SIZE - xstack_ptr),
     * per std_api_write_xstack in sodiumlb/loci-firmware std.c. (xahmol PR #19) */
    loci_write(&l, 0x03AC, 'O');
    loci_write(&l, 0x03AC, 'L');
    loci_write(&l, 0x03AC, 'L');
    loci_write(&l, 0x03AC, 'E');
    loci_write(&l, 0x03AC, 'H');

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_WRITE_XSTACK);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 5);

    /* Close. */
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);

    /* Re-open for reading. */
    push_path(&l, "data.bin");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* Request 5 bytes. */
    push_u16(&l, 5);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_READ_XSTACK);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 5);

    /* Pop and verify "HELLO". */
    char buf[6] = {0};
    for (int i = 0; i < 5; i++) buf[i] = (char)loci_read(&l, 0x03AC);
    ASSERT_TRUE(strcmp(buf, "HELLO") == 0);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);

    char path[300];
    snprintf(path, sizeof(path), "%s/data.bin", tmpdir);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_lseek_set_then_read) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    /* Pre-create file with known content using POSIX. */
    char path[300];
    snprintf(path, sizeof(path), "%s/seek.bin", tmpdir);
    FILE* fp = fopen(path, "wb");
    fwrite("0123456789", 1, 10, fp);
    fclose(fp);

    push_path(&l, "seek.bin");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* Firmware order (std.c, Sprint 36e): push int32 offset=5 MSB-first
     * (little-endian in xstack memory), then uint8 whence=2 (SEEK_SET)
     * last so it sits on top of the stack. */
    int32_t offset = 5;
    loci_write(&l, 0x03AC, (uint8_t)((offset >> 24) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)((offset >> 16) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)((offset >>  8) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)(offset & 0xFF));
    loci_write(&l, 0x03AC, 2);   /* whence on top: 2 = SEEK_SET */

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    /* Returns position in AXSREG. */
    uint32_t pos = l.regs[LOCI_REG_API_A] |
                   ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                   ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                   ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_EQ(pos, 5);

    /* Read 3 bytes → should be "567". */
    push_u16(&l, 3);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_READ_XSTACK);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 3);
    char buf[4] = {0};
    for (int i = 0; i < 3; i++) buf[i] = (char)loci_read(&l, 0x03AC);
    ASSERT_TRUE(strcmp(buf, "567") == 0);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Sprint 36e — firmware-conformance lseek/xstack tests. */

/* Helper: open an existing file, return its fd (or -1). */
static int open_existing(loci_t* l, const char* name) {
    push_path(l, name);
    l->regs[LOCI_REG_API_A] = 0;
    loci_write(l, 0x03AF, LOCI_OP_OPEN);
    int fd = l->regs[LOCI_REG_API_A];
    return (fd >= LOCI_FD_OFFSET) ? fd : -1;
}

static uint32_t read_axsreg(loci_t* l) {
    return l->regs[LOCI_REG_API_A] |
           ((uint32_t)l->regs[LOCI_REG_API_X]       << 8) |
           ((uint32_t)l->regs[LOCI_REG_API_SREG]    << 16) |
           ((uint32_t)l->regs[LOCI_REG_API_SREG_HI] << 24);
}

TEST(test_lseek_whence_cur_and_end_firmware_mapping) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/w.bin", tmpdir);
    FILE* fp = fopen(path, "wb");
    fwrite("0123456789", 1, 10, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    int fd = open_existing(&l, "w.bin");
    ASSERT_TRUE(fd > 0);

    /* whence=1 (SEEK_END), offset short-stacked to ZERO bytes:
     * only the whence byte is pushed → position = file size = 10. */
    loci_write(&l, 0x03AC, 1);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    ASSERT_EQ(read_axsreg(&l), 10);

    /* whence=0 (SEEK_CUR), offset=-4 short-stacked on ONE byte (0xFC,
     * sign-extended like api_pop_int32_end) → 10 - 4 = 6. */
    loci_write(&l, 0x03AC, 0xFC);
    loci_write(&l, 0x03AC, 0);   /* whence on top */
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    ASSERT_EQ(read_axsreg(&l), 6);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_lseek_short_stack_one_byte_offset) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/s.bin", tmpdir);
    FILE* fp = fopen(path, "wb");
    fwrite("abcdefgh", 1, 8, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    int fd = open_existing(&l, "s.bin");
    ASSERT_TRUE(fd > 0);

    /* offset=3 pushed as a single byte + whence=2 (SEEK_SET) on top. */
    loci_write(&l, 0x03AC, 3);
    loci_write(&l, 0x03AC, 2);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    ASSERT_EQ(read_axsreg(&l), 3);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_xstack_512_bytes_capacity) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(LOCI_XSTACK_SIZE, 512);
    /* Push 400 bytes — would silently drop past 256 before Sprint 36e. */
    for (int i = 0; i < 400; i++) {
        loci_write(&l, 0x03AC, (uint8_t)(i & 0xFF));
    }
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE - 400);
    /* Pop everything back: last pushed (399) comes out first. */
    for (int i = 399; i >= 0; i--) {
        ASSERT_EQ(loci_read(&l, 0x03AC), (uint8_t)(i & 0xFF));
    }
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
    loci_cleanup(&l);
}

TEST(test_read_xstack_count_above_256) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/big.bin", tmpdir);
    FILE* fp = fopen(path, "wb");
    for (int i = 0; i < 300; i++) fputc(i & 0xFF, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    int fd = open_existing(&l, "big.bin");
    ASSERT_TRUE(fd > 0);

    push_u16(&l, 300);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_READ_XSTACK);
    uint16_t got = l.regs[LOCI_REG_API_A] |
                   ((uint16_t)l.regs[LOCI_REG_API_X] << 8);
    ASSERT_EQ(got, 300);
    ASSERT_EQ(loci_read(&l, 0x03AC), 0);   /* first byte of the file */
    ASSERT_EQ(loci_read(&l, 0x03AC), 1);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_unlink_removes_file) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/gone.tmp", tmpdir);
    FILE* fp = fopen(path, "wb"); fputc('!', fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "gone.tmp");
    loci_write(&l, 0x03AF, LOCI_OP_UNLINK);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(access(path, F_OK) != 0);

    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

/* UNLINK on an empty directory must succeed (FatFs f_unlink() removes both
 * files and empty directories on real LOCI firmware). (xahmol PR #10) */
TEST(test_unlink_removes_empty_directory) {
    char* tmpdir = make_tmpdir();
    char subdir[300];
    snprintf(subdir, sizeof(subdir), "%s/empty_sub", tmpdir);
    mkdir(subdir, 0755);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "empty_sub");
    loci_write(&l, 0x03AF, LOCI_OP_UNLINK);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(access(subdir, F_OK) != 0);

    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_close_bad_fd_returns_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 42;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EBADF);
}

TEST(test_fd_exhaustion_emfile) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    /* Pre-create file. */
    char path[300];
    snprintf(path, sizeof(path), "%s/a.bin", tmpdir);
    FILE* fp = fopen(path, "wb"); fclose(fp);

    /* Open the same file LOCI_FD_MAX times to fill the table. */
    int opened = 0;
    for (int i = 0; i < LOCI_FD_MAX; i++) {
        push_path(&l, "a.bin");
        l.regs[LOCI_REG_API_A] = 0;
        loci_write(&l, 0x03AF, LOCI_OP_OPEN);
        if ((l.regs[LOCI_REG_API_A] >= LOCI_FD_OFFSET) &&
            (l.regs[LOCI_REG_API_ERRNO_LO] | l.regs[LOCI_REG_API_ERRNO_HI]) == 0) {
            opened++;
        }
    }
    ASSERT_EQ(opened, LOCI_FD_MAX);

    /* One more should fail with EMFILE. */
    push_path(&l, "a.bin");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EMFILE);

    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* ── 34ab: xram window + mount/umount/getcwd ──────────────────── */

TEST(test_xram_window0_read_advances) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Seed xram. */
    for (int i = 0; i < 8; i++) l.xram[i] = (uint8_t)(0x10 + i);
    /* Set step=1, addr=0. */
    loci_write(&l, 0x03A5, 1);
    loci_write(&l, 0x03A6, 0x00);
    loci_write(&l, 0x03A7, 0x00);
    /* Reads of $03A4 should yield xram[0], [1], [2], ... */
    ASSERT_EQ(loci_read(&l, 0x03A4), 0x10);
    ASSERT_EQ(loci_read(&l, 0x03A4), 0x11);
    ASSERT_EQ(loci_read(&l, 0x03A4), 0x12);
    ASSERT_EQ(loci_read(&l, 0x03A4), 0x13);
}

TEST(test_xram_window0_write_advances) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03A5, 1);
    loci_write(&l, 0x03A6, 0x10);
    loci_write(&l, 0x03A7, 0x20);   /* addr = $2010 */
    loci_write(&l, 0x03A4, 0xAA);
    loci_write(&l, 0x03A4, 0xBB);
    loci_write(&l, 0x03A4, 0xCC);
    ASSERT_EQ(l.xram[0x2010], 0xAA);
    ASSERT_EQ(l.xram[0x2011], 0xBB);
    ASSERT_EQ(l.xram[0x2012], 0xCC);
}

TEST(test_xram_window_step_negative) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    for (int i = 0; i < 8; i++) l.xram[i] = (uint8_t)(0xA0 + i);
    loci_write(&l, 0x03A5, (uint8_t)((int8_t)(-1)));
    loci_write(&l, 0x03A6, 0x03);
    loci_write(&l, 0x03A7, 0x00);   /* addr = $0003 */
    ASSERT_EQ(loci_read(&l, 0x03A4), 0xA3);
    ASSERT_EQ(loci_read(&l, 0x03A4), 0xA2);
    ASSERT_EQ(loci_read(&l, 0x03A4), 0xA1);
}

TEST(test_xram_window1_independent) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    for (int i = 0; i < 4; i++) l.xram[0x0500 + i] = (uint8_t)(0x50 + i);
    loci_write(&l, 0x03A9, 1);
    loci_write(&l, 0x03AA, 0x00);
    loci_write(&l, 0x03AB, 0x05);
    ASSERT_EQ(loci_read(&l, 0x03A8), 0x50);
    ASSERT_EQ(loci_read(&l, 0x03A8), 0x51);
}

TEST(test_mount_records_path) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/disk0.dsk", tmpdir);
    FILE* fp = fopen(path, "wb"); fputc('!', fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "disk0.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(l.mnt_mounted[0]);
    ASSERT_TRUE(strcmp(l.mnt_paths[0], "disk0.dsk") == 0);

    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_mount_missing_file_enoent) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "nope.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EFATFS(LOCI_FR_NO_FILE));   /* firmware: 32+FR_NO_FILE */
    ASSERT_TRUE(!l.mnt_mounted[0]);
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_mount_bad_drive_einval) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/x", tmpdir);
    FILE* fp = fopen(path, "wb"); fclose(fp);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x");
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EINVAL);
    unlink(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_umount_clears_slot) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.mnt_mounted[2] = true;
    strcpy(l.mnt_paths[2], "x.dsk");
    l.regs[LOCI_REG_API_A] = 2;
    loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);
    ASSERT_TRUE(!l.mnt_mounted[2]);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
}

/* Sprint 34c R3 — raw .dsk mounts must flush their in-RAM image back
 * to the host file at umount / cleanup so SAVEs survive an emulator
 * exit. (MFM_DISK input stays session-only — covered by a separate
 * limitation note in the CHANGELOG, not testable without a re-encoder.) */
TEST(test_dsk_raw_writes_persisted_on_umount) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/raw.dsk", tmpdir);

    /* Build a small raw flat .dsk (8 sectors, no MFM_DISK header). */
    const uint32_t sz = 8 * 256;
    uint8_t* original = (uint8_t*)calloc(sz, 1);
    for (uint32_t i = 0; i < sz; i++) original[i] = (uint8_t)(i & 0xFF);
    FILE* fp = fopen(path, "wb"); fwrite(original, 1, sz, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    /* Mount as drive 0. */
    push_path(&l, "raw.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(l.dsk_image[0] != NULL);
    ASSERT_TRUE(!l.dsk_is_mfm[0]);   /* raw, no MFM_DISK magic */

    /* Simulate a FDC write : flip the first byte of sector 0. */
    l.dsk_image[0][0] = 0xAB;
    l.dsk_image[0][sz - 1] = 0xCD;

    /* UMOUNT triggers dsk_close → dsk_flush. */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);

    /* Re-read host file and verify the modifications hit disk. */
    uint8_t* round = (uint8_t*)malloc(sz);
    FILE* in = fopen(path, "rb");
    ASSERT_TRUE(in != NULL);
    size_t n = fread(round, 1, sz, in);
    fclose(in);
    ASSERT_EQ(n, sz);
    ASSERT_EQ(round[0], 0xAB);
    ASSERT_EQ(round[sz - 1], 0xCD);

    free(round); free(original);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_read_xram_from_file) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/blob.bin", tmpdir);
    FILE* fp = fopen(path, "wb");
    for (int i = 0; i < 32; i++) fputc(i, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    push_path(&l, "blob.bin");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* Push (xram_addr=0x1000) then (count=32). Firmware pops count first then
     * xram_addr; firmware-side push order is xram_addr first then count. */
    /* push xram_addr u16: hi=0x10, lo=0x00. */
    loci_write(&l, 0x03AC, 0x10);
    loci_write(&l, 0x03AC, 0x00);
    /* push count u16: hi=0x00, lo=0x20. */
    loci_write(&l, 0x03AC, 0x00);
    loci_write(&l, 0x03AC, 0x20);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_READ_XRAM);
    /* AXSREG should hold 32. */
    uint32_t br = l.regs[LOCI_REG_API_A] |
                  ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                  ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                  ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_EQ(br, 32);
    /* xram[0x1000..0x101F] should hold 0..31. */
    for (int i = 0; i < 32; i++)
        ASSERT_EQ(l.xram[0x1000 + i], i);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_getcwd_returns_path) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.mnt_mounted[1] = true;
    strcpy(l.mnt_paths[1], "USB:foo/bar.dsk");
    l.regs[LOCI_REG_API_A] = 1;
    loci_write(&l, 0x03AF, LOCI_OP_GETCWD);
    /* Length should be position of last '/' + 1 = 8 ("USB:foo/" = 8). */
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 8);
    char buf[16] = {0};
    for (int i = 0; i < 8; i++) buf[i] = (char)loci_read(&l, 0x03AC);
    ASSERT_TRUE(strcmp(buf, "USB:foo/") == 0);
}

TEST(test_getcwd_255_derives_rom) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.mnt_mounted[LOCI_MNT_ROM] = true;
    strcpy(l.mnt_paths[LOCI_MNT_ROM], "loci.rom");
    l.regs[LOCI_REG_API_A] = 255;
    loci_write(&l, 0x03AF, LOCI_OP_GETCWD);
    /* No '/' in "loci.rom" → len = strlen = 8. */
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 8);
}

/* ── 34ac: Dir API + uname ───────────────────────────────────── */

TEST(test_opendir_closedir) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    push_path(&l, ".");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    int dfd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(dfd >= LOCI_DIR_OFFSET);

    l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);

    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_opendir_missing_enoent) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "nope_dir");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EFATFS(LOCI_FR_NO_PATH));   /* firmware: 32+FR_NO_PATH */
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_readdir_lists_entries_skipping_dotdot) {
    char* tmpdir = make_tmpdir();
    /* Create two files. */
    char p1[300], p2[300];
    snprintf(p1, sizeof(p1), "%s/alpha.txt", tmpdir);
    snprintf(p2, sizeof(p2), "%s/beta.bin", tmpdir);
    FILE* fp = fopen(p1, "wb"); fwrite("hi", 1, 2, fp); fclose(fp);
    fp = fopen(p2, "wb"); fwrite("xx", 1, 2, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, ".");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    int dfd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(dfd >= LOCI_DIR_OFFSET);

    int seen = 0;
    bool seen_alpha = false, seen_beta = false;
    for (int round = 0; round < 4; round++) {
        l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
        loci_write(&l, 0x03AF, LOCI_OP_READDIR);
        /* Dirent at xstack[ptr..ptr+71]. d_name at offset 2. */
        uint16_t base = l.xstack_ptr;
        char name[65] = {0};
        for (int i = 0; i < 64; i++) name[i] = (char)l.xstack[base + 2 + i];
        if (!name[0]) break;
        if (strcmp(name, "alpha.txt") == 0) seen_alpha = true;
        if (strcmp(name, "beta.bin")  == 0) seen_beta  = true;
        /* Should NOT be "." or "..". */
        ASSERT_TRUE(strcmp(name, ".") != 0);
        ASSERT_TRUE(strcmp(name, "..") != 0);
        seen++;
    }
    ASSERT_TRUE(seen_alpha);
    ASSERT_TRUE(seen_beta);
    ASSERT_EQ(seen, 2);

    l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    unlink(p1); unlink(p2); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Sprint 34c R1 — readdir on a sub-directory must stat() via the
 * sub-dir's path, not flash_root. Previously the sub-entry "inner.txt"
 * came back with d_size = 0 because stat() was called on
 * "<root>/inner.txt" instead of "<root>/sub/inner.txt". */
TEST(test_readdir_subdir_uses_correct_base) {
    char* tmpdir = make_tmpdir();
    char subdir[300], inner_path[400];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    mkdir(subdir, 0755);
    snprintf(inner_path, sizeof(inner_path), "%s/inner.txt", subdir);
    FILE* fp = fopen(inner_path, "wb"); fwrite("hello", 1, 5, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "sub");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    int dfd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(dfd >= LOCI_DIR_OFFSET);

    bool seen = false;
    for (int round = 0; round < 4; round++) {
        l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
        loci_write(&l, 0x03AF, LOCI_OP_READDIR);
        uint16_t base = l.xstack_ptr;
        char name[65] = {0};
        for (int i = 0; i < 64; i++) name[i] = (char)l.xstack[base + 2 + i];
        if (!name[0]) break;
        if (strcmp(name, "inner.txt") == 0) {
            seen = true;
            uint32_t sz = (uint32_t)l.xstack[base + 68]
                        | ((uint32_t)l.xstack[base + 69] << 8)
                        | ((uint32_t)l.xstack[base + 70] << 16)
                        | ((uint32_t)l.xstack[base + 71] << 24);
            /* Previously this was 0 because of the wrong base path. */
            ASSERT_EQ(sz, 5u);
        }
    }
    ASSERT_TRUE(seen);

    l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    unlink(inner_path); rmdir(subdir); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Sprint 34c R2 — resolve_path must reject the EXACT ".." component
 * (escape) but accept names that merely CONTAIN ".." (legitimate). */
TEST(test_path_allows_double_dot_in_name) {
    char* tmpdir = make_tmpdir();
    char fpath[300];
    snprintf(fpath, sizeof(fpath), "%s/my..backup.bin", tmpdir);
    FILE* fp = fopen(fpath, "wb"); fwrite("x", 1, 1, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "my..backup.bin");
    l.regs[LOCI_REG_API_A] = 0;   /* read-only */
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);   /* used to be EACCES via strstr */

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(fpath); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

/* Sprint 34c R2 — embedded ".." in a sub-component still rejected. */
TEST(test_path_rejects_dotdot_component) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, "/tmp");
    push_path(&l, "sub/../etc/passwd");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EACCES);
    loci_cleanup(&l);
}

/* ─── Sprint 34c R5 — error paths matrix ─────────────────────────
 * The senior review post-34b5 noted the errno surface of LOCI ops is
 * thinly tested. These exercise the 5 main errno classes (EBADF, EINVAL,
 * ENOENT, EACCES) across ops that didn't have a dedicated coverage:
 * READ/WRITE_XSTACK/LSEEK (EBADF), LSEEK (EINVAL), UNLINK/RENAME
 * (ENOENT), READDIR (EBADF), MKDIR (EACCES). */

static uint16_t errno_lo(loci_t* l) {
    return (uint16_t)(l->regs[LOCI_REG_API_ERRNO_LO]
                    | ((uint16_t)l->regs[LOCI_REG_API_ERRNO_HI] << 8));
}

TEST(test_op_read_xstack_bad_fd_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Push uint16 count = 4 (lo first → push hi then lo). */
    push_u16(&l, 4);
    l.regs[LOCI_REG_API_A] = 99;   /* not allocated */
    loci_write(&l, 0x03AF, LOCI_OP_READ_XSTACK);
    ASSERT_EQ(errno_lo(&l), LOCI_EBADF);
    loci_cleanup(&l);
}

TEST(test_op_write_xstack_bad_fd_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Push some payload + count = 1. */
    loci_write(&l, 0x03AC, 'X');
    push_u16(&l, 1);
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_WRITE_XSTACK);
    ASSERT_EQ(errno_lo(&l), LOCI_EBADF);
    loci_cleanup(&l);
}

TEST(test_op_lseek_bad_fd_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Push int32 offset (4 bytes) then uint8 whence (=2 SEEK_SET) on top. */
    loci_write(&l, 0x03AC, 0);   /* offset[3] hi */
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);   /* offset[0] lo */
    loci_write(&l, 0x03AC, 2);   /* whence on top */
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    ASSERT_EQ(errno_lo(&l), LOCI_EBADF);
    loci_cleanup(&l);
}

TEST(test_op_lseek_bad_whence_einval) {
    char* tmpdir = make_tmpdir();
    char path[300]; snprintf(path, sizeof(path), "%s/a.bin", tmpdir);
    FILE* fp = fopen(path, "wb"); fwrite("xyz", 1, 3, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "a.bin");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    int fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= LOCI_FD_OFFSET);

    /* whence = 5 → invalid (only 0/1/2 accepted). Pushed last = on top. */
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 5);   /* whence on top */
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_LSEEK);
    ASSERT_EQ(errno_lo(&l), LOCI_EINVAL);

    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);
    unlink(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_op_unlink_missing_enoent) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "does_not_exist.bin");
    loci_write(&l, 0x03AF, LOCI_OP_UNLINK);
    ASSERT_EQ(errno_lo(&l), LOCI_EFATFS(LOCI_FR_NO_FILE));   /* f_unlink → 32+FR_NO_FILE */
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

/* UNLINK on a non-empty directory must fail with LOCI_EACCES (rmdir ENOTEMPTY)
 * and leave the directory and its contents intact. (xahmol PR #10) */
TEST(test_unlink_nonempty_directory_returns_eacces) {
    char* tmpdir = make_tmpdir();
    char subdir[300], inner_path[400];
    snprintf(subdir, sizeof(subdir), "%s/full_sub", tmpdir);
    mkdir(subdir, 0755);
    snprintf(inner_path, sizeof(inner_path), "%s/inner.txt", subdir);
    FILE* fp = fopen(inner_path, "wb"); fputc('!', fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "full_sub");
    loci_write(&l, 0x03AF, LOCI_OP_UNLINK);
    ASSERT_EQ(errno_lo(&l), LOCI_EFATFS(LOCI_FR_DENIED));    /* f_unlink non-vide → 32+FR_DENIED */
    ASSERT_TRUE(access(subdir, F_OK) == 0);

    unlink(inner_path); rmdir(subdir); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_op_rename_missing_enoent) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    /* xstack : new path on top, old path below — same order as op_open
     * comment in loci.c:1431. */
    push_path(&l, "old_missing.bin");
    push_path(&l, "newname.bin");
    loci_write(&l, 0x03AF, LOCI_OP_RENAME);
    ASSERT_EQ(errno_lo(&l), LOCI_EFATFS(LOCI_FR_NO_FILE));   /* f_rename → 32+FR_NO_FILE */
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

/* Sprint 34c R5 — happy path for op_rename. The bug fix is for both
 * pop calls to read distinct strings (previously the second pop saw a
 * zeroed xstack and returned EINVAL, making rename always fail). */
TEST(test_op_rename_renames_existing) {
    char* tmpdir = make_tmpdir();
    char src[300], dst[300];
    snprintf(src, sizeof(src), "%s/from.bin", tmpdir);
    snprintf(dst, sizeof(dst), "%s/to.bin",   tmpdir);
    FILE* fp = fopen(src, "wb"); fwrite("RENAME", 1, 6, fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "from.bin");   /* old on bottom */
    push_path(&l, "to.bin");     /* new on top */
    loci_write(&l, 0x03AF, LOCI_OP_RENAME);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);

    struct stat st;
    ASSERT_TRUE(stat(src, &st) != 0);    /* source gone */
    ASSERT_EQ(stat(dst, &st), 0);
    ASSERT_EQ(st.st_size, 6);

    unlink(dst); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_op_readdir_bad_fd_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_READDIR);
    ASSERT_EQ(errno_lo(&l), LOCI_EBADF);
    loci_cleanup(&l);
}

TEST(test_op_mkdir_traversal_eacces) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, "/tmp");
    /* Embed ".." as a real path component → rejected by resolve_path. */
    push_path(&l, "..");
    loci_write(&l, 0x03AF, LOCI_OP_MKDIR);
    ASSERT_EQ(errno_lo(&l), LOCI_EACCES);
    loci_cleanup(&l);
}

TEST(test_mkdir_creates_dir) {
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "newsub");
    loci_write(&l, 0x03AF, LOCI_OP_MKDIR);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);

    char path[300];
    snprintf(path, sizeof(path), "%s/newsub", tmpdir);
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    rmdir(path); rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_closedir_bad_fd_ebadf) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 99;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EBADF);
}

TEST(test_uname_pushes_5_fields) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_UNAME);
    /* Total bytes pushed = 17 + 9 + 9 + 9 + 25 = 69 */
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 69);
    /* Top of xstack should hold sysname starting with "Phosphoric". */
    char buf[18] = {0};
    memcpy(buf, &l.xstack[l.xstack_ptr], 17);
    ASSERT_TRUE(strncmp(buf, "Phosphoric LOCI", 15) == 0);
}

/* Sprint 34c hardening — release field tracks EMU_VERSION rather than the
 * historically frozen "1.16.27 " literal. */
TEST(test_uname_release_uses_emu_version) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_UNAME);
    /* Push order from firmware view (sysname pushed last → top of xstack):
     *   xstack[ptr      .. ptr+16] = sysname  (17 bytes)
     *   xstack[ptr+17   .. ptr+25] = nodename (9 bytes)
     *   xstack[ptr+26   .. ptr+34] = release  (9 bytes)
     */
    char release[9] = {0};
    memcpy(release, &l.xstack[l.xstack_ptr + 26], 8);
    /* Should start with EMU_VERSION's leading chars ("1.16.64-alpha" → "1.16.64-"). */
    ASSERT_TRUE(strncmp(release, EMU_VERSION, 8) == 0);
    /* And NOT the frozen literal. */
    ASSERT_TRUE(strncmp(release, "1.16.27", 7) != 0);
}

/* ── 34ag: USB HID stubs ─────────────────────────────────────── */

/* Helper: build a PIX_XREG call to MIA device with channel/addr/word. */
static void pix_xreg_mia(loci_t* l, uint8_t channel, uint8_t addr, uint16_t word) {
    /* Push from bottom to top of stack (high index last). Layout :
     *   bottom of pushed data = lo(word)   (xstack[XSTACK-5])
     *                hi(word) (xstack[XSTACK-4])
     *                addr     (xstack[XSTACK-3])
     *                channel  (xstack[XSTACK-2])
     *                device   (xstack[XSTACK-1])
     * Reverse-push so the final top-of-stack ends up at device. */
    loci_write(l, 0x03AC, 0);              /* push device=0 (MIA) */
    loci_write(l, 0x03AC, channel);
    loci_write(l, 0x03AC, addr);
    loci_write(l, 0x03AC, (uint8_t)(word >> 8));
    loci_write(l, 0x03AC, (uint8_t)(word & 0xFF));
    loci_write(l, 0x03AF, LOCI_OP_PIX_XREG);
}

TEST(test_pix_xreg_kbd_sets_kbd_xram) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);
    ASSERT_EQ(l.kbd_xram, 0x4000);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
}

TEST(test_pix_xreg_mou_sets_mou_xram) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 1, 0x5000);
    ASSERT_EQ(l.mou_xram, 0x5000);
}

TEST(test_pix_xreg_pad_sets_pad_xram) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 2, 0x6000);
    ASSERT_EQ(l.pad_xram, 0x6000);
}

TEST(test_pix_xreg_disables_kbd_with_ffff) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);
    pix_xreg_mia(&l, 0, 0, 0xFFFF);
    ASSERT_EQ(l.kbd_xram, 0xFFFF);
}

TEST(test_pix_xreg_bad_device_einval) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Push with device=1 (VGA — not served). */
    loci_write(&l, 0x03AC, 1);   /* device */
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AC, 0);
    loci_write(&l, 0x03AF, LOCI_OP_PIX_XREG);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EINVAL);
}

TEST(test_kbd_set_report_writes_bitmap) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);

    /* Press 'A' (HID 0x04) + 'B' (0x05). */
    uint8_t kc[6] = {0x04, 0x05, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc);

    /* byte 0 bit 4 = 'A'; byte 0 bit 5 = 'B'. */
    ASSERT_EQ(l.xram[0x4000] & 0x30, 0x30);
    /* No "no-key" sentinel since a key is down. */
    ASSERT_EQ(l.xram[0x4000] & 0x01, 0);
}

TEST(test_kbd_set_report_modifier_byte28) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);

    /* LCtrl = 0x01 modifier, no key. */
    uint8_t kc[6] = {0};
    loci_kbd_set_report(&l, 0x01, kc);

    /* Modifier lives at byte LOCI_HID_KEY_CONTROL_LEFT >> 3 = 0xE0>>3 = 28. */
    ASSERT_EQ(l.xram[0x4000 + 28], 0x01);
    /* No "no-key" sentinel because modifier is non-zero. */
    ASSERT_EQ(l.xram[0x4000] & 0x01, 0);
}

TEST(test_kbd_set_report_no_key_sentinel) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);

    uint8_t kc[6] = {0};
    loci_kbd_set_report(&l, 0, kc);
    /* Bit 0 of byte 0 = "no key pressed" sentinel. */
    ASSERT_EQ(l.xram[0x4000] & 0x01, 0x01);
}

TEST(test_kbd_clear_resets_bitmap) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);

    /* Pre-fill with a key. */
    uint8_t kc1[6] = {0x04, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc1);
    ASSERT_TRUE(l.xram[0x4000] & 0x10);

    loci_kbd_clear(&l);
    /* All keys released → sentinel bit set, key bit cleared. */
    ASSERT_EQ(l.xram[0x4000] & 0x10, 0);
    ASSERT_EQ(l.xram[0x4000] & 0x01, 0x01);
}

TEST(test_kbd_set_report_noop_when_xram_unset) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Pre-fill xram[0..32] with a known pattern. */
    for (int i = 0; i < 32; i++) l.xram[i] = (uint8_t)(0xAA + i);
    /* kbd_xram is 0xFFFF → no-op expected. */
    uint8_t kc[6] = {0x04, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc);
    for (int i = 0; i < 32; i++)
        ASSERT_EQ(l.xram[i], (uint8_t)(0xAA + i));
}

/* ── 34ad: MIA_BOOT + ROM swap callback ──────────────────────── */

/* Capturing callback for tests. */
typedef struct {
    int  call_count;
    char last_path[256];
    uint16_t last_base;
    bool result;
} rom_swap_capture_t;

static bool capture_swap(void* ctx, const char* rom_path, uint16_t base_addr) {
    rom_swap_capture_t* c = (rom_swap_capture_t*)ctx;
    c->call_count++;
    strncpy(c->last_path, rom_path, sizeof(c->last_path) - 1);
    c->last_path[sizeof(c->last_path) - 1] = '\0';
    c->last_base = base_addr;
    return c->result;
}

TEST(test_mia_boot_resume_no_callback_invocation) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_RESUME;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(cap.call_count, 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
}

/* MIA_BOOT with LOCI_BOOT_RESUME calls the session-resume callback
 * (menu "resume" entry, Sprint 85); its failure surfaces as ENOENT. */
static int  test_resume_calls = 0;
static bool test_resume_result = true;
static bool test_resume_cb(void* ctx) { (void)ctx; test_resume_calls++; return test_resume_result; }

TEST(test_mia_boot_resume_invokes_resume_callback) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_resume_callback(&l, test_resume_cb, NULL);
    test_resume_calls = 0;
    test_resume_result = true;
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_RESUME;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(test_resume_calls, 1);
    ASSERT_EQ(errno_lo(&l), 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    /* No snapshot to resume → ENOENT */
    test_resume_result = false;
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_RESUME;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(test_resume_calls, 2);
    ASSERT_EQ(errno_lo(&l), LOCI_ENOENT);
}

TEST(test_mia_boot_basic10_default) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    loci_set_flash_root(&l, "/tmp/foo");
    l.regs[LOCI_REG_API_A] = 0;   /* no B11, no RESUME */
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_base, 0xC000);
    /* Path should be "/tmp/foo/basic10.rom" (sandbox-resolved). */
    ASSERT_TRUE(strstr(cap.last_path, "basic10.rom") != NULL);
}

TEST(test_mia_boot_b11_basic11b) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_B11;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_TRUE(strstr(cap.last_path, "basic11b.rom") != NULL);
}

TEST(test_mia_boot_uses_mounted_rom_slot) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    /* Pre-mount a custom ROM on slot 5. */
    l.mnt_mounted[LOCI_MNT_ROM] = true;
    strcpy(l.mnt_paths[LOCI_MNT_ROM], "custom.rom");
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_B11;   /* should be ignored */
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_TRUE(strstr(cap.last_path, "custom.rom") != NULL);
    ASSERT_TRUE(strstr(cap.last_path, "basic") == NULL);
}

TEST(test_mia_boot_callback_failure_returns_eio) {
    rom_swap_capture_t cap = { .result = false };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EIO);
}

TEST(test_mia_boot_no_callback_acks) {
    /* No callback registered → op returns ax=0 without crashing. */
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
}

TEST(test_mia_boot_settings_latched) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_B11 | LOCI_BOOT_FAST;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(l.boot_settings, LOCI_BOOT_B11 | LOCI_BOOT_FAST);
}

TEST(test_mia_boot_fdc_loads_microdisc_too) {
    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_rom_swap_callback(&l, capture_swap, &cap);
    l.regs[LOCI_REG_API_A] = LOCI_BOOT_FDC;
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    /* Two callbacks: one for $C000 (BASIC), one for $A000 (Microdisc). */
    ASSERT_EQ(cap.call_count, 2);
    ASSERT_EQ(cap.last_base, 0xA000);
    ASSERT_TRUE(strstr(cap.last_path, "microdis") != NULL);
}

/* ── 34af: TAP cassette API + $0315-$0317 ────────────────────── */

/* Build a minimal TAP file with one sync mark + 16-byte header.
 * Layout: lead-in zeros (4) + 16 16 16 24 + header (16). */
static char* make_tap_with_one_header(const char* root, const char* filename) {
    char* path = malloc(300);
    snprintf(path, 300, "%s/%s", root, filename);
    FILE* fp = fopen(path, "wb");
    /* Optional lead-in (a few zero bytes). */
    for (int i = 0; i < 4; i++) fputc(0, fp);
    /* Sync mark. */
    fputc(0x16, fp); fputc(0x16, fp); fputc(0x16, fp); fputc(0x24, fp);
    /* Header (16 bytes): flag/flag/type/autorun/end_hi/end_lo/start_hi/start_lo
     * /reserved/filename[7]. We fill in deterministic values. */
    static const uint8_t hdr[16] = {
        0x00, 0x00, 0x00, 0xC7,        /* flags + autorun */
        0x09, 0xFF,                    /* end addr */
        0x05, 0x00,                    /* start addr */
        0x00,                          /* reserved */
        'M','Y','P','R','G',0,0        /* filename (7 chars used in header) */
    };
    fwrite(hdr, 1, 16, fp);
    fclose(fp);
    return path;
}

TEST(test_tap_mount_opens_file) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "game.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "game.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(l.tap_fp != NULL);
    ASSERT_EQ(l.tap_size, 24);   /* 4 + 4 + 16 */
    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_umount_closes) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_TRUE(l.tap_fp != NULL);
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);
    ASSERT_TRUE(l.tap_fp == NULL);
    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_tell_returns_zero_after_mount) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    loci_write(&l, 0x03AF, LOCI_OP_TAP_TELL);
    uint32_t pos = l.regs[LOCI_REG_API_A] |
                   ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                   ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                   ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_EQ(pos, 0);
    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_seek_updates_counter) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    /* Set AXSREG = 10 then call SEEK. */
    l.regs[LOCI_REG_API_A] = 10;
    l.regs[LOCI_REG_API_X] = 0;
    l.regs[LOCI_REG_API_SREG] = 0;
    l.regs[LOCI_REG_API_SREG_HI] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_TAP_SEEK);
    ASSERT_EQ(l.tap_counter, 10);
    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_seek_clamps_to_size_minus_1) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    l.regs[LOCI_REG_API_A] = 0xFF;   /* way past size 24 */
    l.regs[LOCI_REG_API_X] = 0xFF;
    l.regs[LOCI_REG_API_SREG] = 0;
    l.regs[LOCI_REG_API_SREG_HI] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_TAP_SEEK);
    /* Should clamp to size-1 = 23. */
    ASSERT_EQ(l.tap_counter, 23);
    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_read_header_finds_sync_and_pushes_header) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    loci_write(&l, 0x03AF, LOCI_OP_TAP_READ_HEADER);

    /* AXSREG should hold sync start = position of first 0x16 = 4. */
    uint32_t sync_start = l.regs[LOCI_REG_API_A] |
                         ((uint32_t)l.regs[LOCI_REG_API_X]    << 8) |
                         ((uint32_t)l.regs[LOCI_REG_API_SREG] << 16) |
                         ((uint32_t)l.regs[LOCI_REG_API_SREG_HI] << 24);
    ASSERT_EQ(sync_start, 4);

    /* xstack should hold the 16-byte header. */
    ASSERT_EQ(LOCI_XSTACK_SIZE - l.xstack_ptr, 16);
    /* Filename starts at offset 9 (offset 9 == 'M'). */
    ASSERT_EQ(l.xstack[l.xstack_ptr + 9], 'M');

    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

TEST(test_tap_read_header_no_sync_returns_enoent) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/empty.tap", tmpdir);
    /* File large enough to pass the size check but no sync mark. */
    FILE* fp = fopen(path, "wb");
    for (int i = 0; i < 32; i++) fputc(0, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "empty.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    loci_write(&l, 0x03AF, LOCI_OP_TAP_READ_HEADER);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOENT);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

TEST(test_tap_seek_no_tape_returns_enodev) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_TAP_SEEK);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENODEV);
}

TEST(test_tap_io_stat_reports_not_ready_when_empty) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    uint8_t v = loci_tap_read(&l, 0x0316);
    ASSERT_TRUE((v & LOCI_TAP_STAT_NOT_READY) != 0);
}

TEST(test_tap_io_cmd_rewind_resets_counter) {
    char* tmpdir = make_tmpdir();
    char* tap_path = make_tap_with_one_header(tmpdir, "x.tap");
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);

    /* Move forward via SEEK then rewind via $0315 CMD. */
    l.regs[LOCI_REG_API_A] = 10;
    l.regs[LOCI_REG_API_X] = 0;
    l.regs[LOCI_REG_API_SREG] = 0;
    l.regs[LOCI_REG_API_SREG_HI] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_TAP_SEEK);
    ASSERT_EQ(l.tap_counter, 10);
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_REW);
    ASSERT_EQ(l.tap_counter, 0);

    unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

/* ── 34ae: DSK multi-drive WD179x stub ───────────────────────── */

static char* make_blob(const char* root, const char* name, int size) {
    char* p = malloc(300);
    snprintf(p, 300, "%s/%s", root, name);
    FILE* fp = fopen(p, "wb");
    for (int i = 0; i < size; i++) fputc(i & 0xFF, fp);
    fclose(fp);
    return p;
}

TEST(test_dsk_mount_opens_drive_image) {
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "side0.dsk", 256);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "side0.dsk");
    l.regs[LOCI_REG_API_A] = 0;   /* drive 0 */
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_TRUE(l.dsk_fp[0] != NULL);
    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

TEST(test_dsk_umount_closes) {
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "x.dsk", 16);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "x.dsk");
    l.regs[LOCI_REG_API_A] = 2;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_TRUE(l.dsk_fp[2] != NULL);
    l.regs[LOCI_REG_API_A] = 2;
    loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);
    ASSERT_TRUE(l.dsk_fp[2] == NULL);
    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

/* opendir with an EMPTY path returns the device-list iterator (fd 0 =
 * firmware FD_OFFS_DEV): readdir yields "0: Internal storage [15MB]",
 * then one usb_set_status line per declared USB device (MSC key,
 * picowifi CDC modem), then an empty name. AM_SYS attribute. */
TEST(test_opendir_empty_lists_devices) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(loci_add_usb_device(&l, "CDC modem mounted"), 0);
    push_path(&l, "");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);        /* fd = FD_OFFS_DEV */
    /* Entry 0: internal storage */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_READDIR);
    ASSERT_EQ(memcmp(&l.xstack[LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE + 2],
                     "0: Internal storage [15MB]", 26), 0);
    ASSERT_EQ(l.xstack[LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE + 66], LOCI_AM_SYS);
    /* Entry 1: the picowifi modem */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_READDIR);
    ASSERT_EQ(memcmp(&l.xstack[LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE + 2],
                     "1: CDC modem mounted", 20), 0);
    /* End of list: empty name */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_READDIR);
    ASSERT_EQ(l.xstack[LOCI_XSTACK_SIZE - LOCI_DIRENT_SIZE + 2], 0);
    /* closedir releases the single iterator; a second opendir works */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    push_path(&l, "");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    loci_cleanup(&l);
}

/* A USB storage device registered with a host root serves its "N:"
 * volume: "1:FILE" opens inside the key's directory, while "0:"/bare
 * paths keep resolving in the flash root (internal storage). */
TEST(test_usb_storage_paths_resolve_to_host_root) {
    char* flash = make_tmpdir();
    char* key = make_tmpdir();
    char fpath[400];
    snprintf(fpath, sizeof(fpath), "%s/USBFILE.TXT", key);
    FILE* fp = fopen(fpath, "wb"); fputs("KEY!", fp); fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, flash);
    ASSERT_EQ(loci_add_usb_storage(&l, "MSC 1.0 GB TESTKEY", key), 1);

    /* "1:USBFILE.TXT" reads from the key */
    push_path(&l, "1:USBFILE.TXT");
    l.regs[LOCI_REG_API_A] = 0;   /* RDONLY */
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t fd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(fd >= 3 && fd <= 18);
    l.regs[LOCI_REG_API_A] = (uint8_t)fd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSE);

    /* "0:USBFILE.TXT" = internal storage: the file is NOT there */
    push_path(&l, "0:USBFILE.TXT");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_EFATFS(LOCI_FR_NO_FILE));

    unlink(fpath); rmdir(key); rmdir(flash);
    loci_cleanup(&l); free(key); free(flash);
}

/* Conformance pin: firmware dir.c FD_OFFS_FAT=64 (dir descriptors on the
 * SD/FAT backend) and api.h API_EFATFS = 32+FRESULT for filesystem errors.
 * cc65 apps compiled against the real firmware see these exact values. */
TEST(test_firmware_abi_dir_fd_and_efatfs) {
    ASSERT_EQ(LOCI_DIR_OFFSET, 64);
    ASSERT_EQ(LOCI_EFATFS(LOCI_FR_NO_FILE), 36);
    char* tmpdir = make_tmpdir();
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, ".");                      /* open the root dir */
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    uint16_t dfd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(dfd >= 64 && dfd < 64 + LOCI_DIR_MAX);   /* FAT range */
    l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);
    rmdir(tmpdir); loci_cleanup(&l); free(tmpdir);
}

TEST(test_dsk_bad_sector_seeded_at_mount) {
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "dmg.dsk", 256);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    /* CLI injection happens before any mount: seeds the drive-0 CLI map */
    ASSERT_EQ(loci_add_bad_sector(&l, 0, 0, 4, 2), 0);
    ASSERT_EQ(l.dsk_bad_cli[0].count, 1);
    /* Mounting media in drive 0 carries the injected damage */
    push_path(&l, "dmg.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.dsk_bad_map[0].count, 1);
    ASSERT_EQ(l.dsk_bad_map[0].entry[0].track, 4);
    ASSERT_EQ(l.dsk_fdc.bad.count, 1);       /* drive 0 selected by default */
    /* Umount: no media, live map cleared, CLI map kept for the next mount */
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);
    ASSERT_EQ(l.dsk_bad_map[0].count, 0);
    ASSERT_EQ(l.dsk_fdc.bad.count, 0);
    ASSERT_EQ(l.dsk_bad_cli[0].count, 1);
    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

TEST(test_dsk_cmd_reports_not_ready_for_empty_drive) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* No drive mounted → selected drive 0 not ready. */
    uint8_t s = loci_dsk_read(&l, 0x0310);
    ASSERT_TRUE((s & LOCI_DSK_STAT_NOT_READY) != 0);
}

TEST(test_dsk_cmd_ready_for_mounted_drive) {
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "y.dsk", 32);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "y.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    uint8_t s = loci_dsk_read(&l, 0x0310);
    ASSERT_EQ(s & LOCI_DSK_STAT_NOT_READY, 0);
    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

TEST(test_dsk_ctrl_select_drive_via_bits_5_6) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Bits 5-6 = drive 2 → value 0b01000000 = 0x40. */
    loci_dsk_write(&l, 0x0314, 0x40);
    ASSERT_EQ(l.dsk_selected, 2);
    ASSERT_EQ(l.dsk_ctrl, 0x40);
}

TEST(test_dsk_track_sect_data_passthrough) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_dsk_write(&l, 0x0311, 0x12);   /* track */
    loci_dsk_write(&l, 0x0312, 0x34);   /* sector */
    loci_dsk_write(&l, 0x0313, 0x56);   /* data */
    ASSERT_EQ(loci_dsk_read(&l, 0x0311), 0x12);
    ASSERT_EQ(loci_dsk_read(&l, 0x0312), 0x34);
    ASSERT_EQ(loci_dsk_read(&l, 0x0313), 0x56);
}

TEST(test_dsk_drq_register) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Sprint 36f : firmware serves exact 0x80 (idle) / 0x00 (asserted)
     * for the active-low DRQ register — no |0x7F padding. */
    ASSERT_EQ(loci_dsk_read(&l, 0x0318), 0x80);
}

TEST(test_dsk_id_register_returns_loci_marker) {
    /* Real loci-firmware sets IOREGS(0x0319)='L' as a permanent identity
     * marker (mia.c:1399) and disables writes to it (mia.c:819-822).
     * Client software (locifilemanager loci_present()) detects LOCI by
     * checking peek(0x0319) == 'L'. */
    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_EQ(loci_dsk_read(&l, 0x0319), 'L');
    loci_dsk_write(&l, 0x0319, 0x00);
    ASSERT_EQ(loci_dsk_read(&l, 0x0319), 'L');
}

TEST(test_dsk_id_register_ff_when_disabled) {
    loci_t l; loci_init(&l);
    l.enabled = false;
    ASSERT_EQ(loci_dsk_read(&l, 0x0319), 0xFF);
}

/* ── Sprint 36f : conformité firmware (console, TAP bas niveau, …) ── */

TEST(test_dsk_spare_registers_31a_31b) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Firmware keeps $031A-$031B in the read-enable map, init 0, plain
     * storage on write. */
    ASSERT_EQ(loci_dsk_read(&l, 0x031A), 0x00);
    ASSERT_EQ(loci_dsk_read(&l, 0x031B), 0x00);
    loci_dsk_write(&l, 0x031A, 0x42);
    loci_dsk_write(&l, 0x031B, 0x99);
    ASSERT_EQ(loci_dsk_read(&l, 0x031A), 0x42);
    ASSERT_EQ(loci_dsk_read(&l, 0x031B), 0x99);
    ASSERT_TRUE(loci_addr_in_dsk(0x031A));
    ASSERT_TRUE(loci_addr_in_dsk(0x031B));
}

TEST(test_dsk_irq_register_exact_bytes) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* $0314 read = IRQ register, active low: exact 0x80 idle / 0x00
     * pending (firmware bytes, no |0x7F padding). */
    ASSERT_EQ(loci_dsk_read(&l, 0x0314), 0x80);
    l.dsk_fdc.set_intrq(&l);
    ASSERT_EQ(loci_dsk_read(&l, 0x0314), 0x00);
    l.dsk_fdc.clr_intrq(&l);
    ASSERT_EQ(loci_dsk_read(&l, 0x0314), 0x80);
}

TEST(test_cons_flags_tx_ready_rx_pending) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* Bit 7 (TX writable) always set; bit 6 (RX ready) follows the ring. */
    ASSERT_EQ(loci_read(&l, 0x03A0), 0x80);
    loci_cons_inject(&l, 'A');
    ASSERT_EQ(loci_read(&l, 0x03A0), 0xC0);
    ASSERT_EQ(loci_read(&l, 0x03A2), 'A');   /* read consumes */
    ASSERT_EQ(loci_read(&l, 0x03A0), 0x80);
    ASSERT_EQ(loci_read(&l, 0x03A2), 0x00);  /* empty queue reads 0 */
}

TEST(test_cons_rx_fifo_order) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_cons_inject(&l, 'O');
    loci_cons_inject(&l, 'K');
    ASSERT_EQ(loci_read(&l, 0x03A2), 'O');
    ASSERT_EQ(loci_read(&l, 0x03A2), 'K');
}

TEST(test_cons_tx_line_buffer_flushes_on_newline) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03A1, 'H');
    loci_write(&l, 0x03A1, 'I');
    ASSERT_EQ(l.cons_tx_len, 2);
    loci_write(&l, 0x03A1, '\n');   /* flush to log */
    ASSERT_EQ(l.cons_tx_len, 0);
}

TEST(test_ula_snoop_write_clears_register) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.regs[LOCI_REG_ULA_SNOOP] = 0x1A;
    loci_write(&l, 0x03A3, 0x30);   /* reload matcher → register cleared */
    ASSERT_EQ(loci_read(&l, 0x03A3), 0x00);
}

TEST(test_op_adj_scan_reports_completed) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.xram[0xFFF0] = 0xFF;
    loci_write(&l, 0x03AF, LOCI_OP_ADJ_SCAN);
    ASSERT_EQ(errno_lo(&l), 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_X], 0);
    /* Progress byte: bit 7 clear = scan no longer in progress. */
    ASSERT_EQ(l.xram[0xFFF0] & 0x80, 0);
    ASSERT_EQ(l.op_count[LOCI_OP_ADJ_SCAN], 1);
}

/* Helper: create a TAP-protocol test file with given bytes. */
static char* make_tap_file(const char* tmpdir, const uint8_t* bytes, size_t n) {
    char* path = malloc(300);
    snprintf(path, 300, "%s/proto.tap", tmpdir);
    FILE* fp = fopen(path, "wb");
    fwrite(bytes, 1, n, fp);
    fclose(fp);
    return path;
}

TEST(test_tap_play_lead_in_then_data) {
    char* tmpdir = make_tmpdir();
    uint8_t content[2] = { 0xAA, 0xBB };
    char* path = make_tap_file(tmpdir, content, 2);

    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_TRUE(loci_tap_open(&l, path));
    loci_tap_motor(&l, true);

    /* First 8 PLAY reads serve the synthetic 0x16 lead-in; the byte
     * counter must not move (firmware tap_read_byte). */
    for (int i = 0; i < 8; i++) {
        loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_PLAY);
        ASSERT_EQ(loci_tap_read(&l, 0x0315), 0x00);   /* cmd done */
        ASSERT_EQ(loci_tap_read(&l, 0x0317), 0x16);
        ASSERT_EQ(l.tap_counter, 0);
    }
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_PLAY);
    ASSERT_EQ(loci_tap_read(&l, 0x0317), 0xAA);
    ASSERT_EQ(l.tap_counter, 1);
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_PLAY);
    ASSERT_EQ(loci_tap_read(&l, 0x0317), 0xBB);
    /* Past EOF: firmware returns 0x00. */
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_PLAY);
    ASSERT_EQ(loci_tap_read(&l, 0x0317), 0x00);

    loci_tap_close(&l);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(path); free(tmpdir);
}

TEST(test_tap_play_is_motor_protected) {
    char* tmpdir = make_tmpdir();
    uint8_t content[1] = { 0x5C };
    char* path = make_tap_file(tmpdir, content, 1);

    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_TRUE(loci_tap_open(&l, path));

    /* Motor off: PLAY stays latched in $0315 (firmware state machine
     * waits in TAP_IDLE), DATA untouched. */
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_PLAY);
    ASSERT_EQ(loci_tap_read(&l, 0x0315), LOCI_TAP_CMD_PLAY);
    ASSERT_EQ(loci_tap_read(&l, 0x0317), 0x00);

    /* Motor on (VIA ORB bit 6 snoop): latched command executes. */
    loci_tap_motor(&l, true);
    ASSERT_EQ(loci_tap_read(&l, 0x0315), 0x00);
    ASSERT_EQ(loci_tap_read(&l, 0x0317), 0x16);   /* lead-in byte 1 */

    loci_tap_close(&l);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(path); free(tmpdir);
}

TEST(test_tap_read_bit_14bit_frame) {
    char* tmpdir = make_tmpdir();
    uint8_t content[1] = { 0x00 };
    char* path = make_tap_file(tmpdir, content, 1);

    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_TRUE(loci_tap_open(&l, path));
    loci_tap_motor(&l, true);

    /* First byte streamed is the 0x16 lead-in. Expected frame
     * (firmware tap_encode_byte): 0b11110000000000 | parity<<9 | b<<1.
     * 0x16 has 3 set bits → odd-parity bit = (1+3)&1 = 0.
     * frame = 0x3C00 | 0x16<<1 = 0x3C2C, served LSB first over 14 reads. */
    uint16_t frame = 0;
    for (int i = 0; i < 14; i++) {
        loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_READ_BIT);
        ASSERT_EQ(loci_tap_read(&l, 0x0315), 0x00);
        frame |= (uint16_t)((loci_tap_read(&l, 0x0317) & 0x01) << i);
    }
    ASSERT_EQ(frame, 0x3C2C);

    loci_tap_close(&l);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(path); free(tmpdir);
}

TEST(test_tap_rec_writes_byte_and_ffw) {
    char* tmpdir = make_tmpdir();
    uint8_t content[2] = { 0x00, 0x00 };
    char* path = make_tap_file(tmpdir, content, 2);

    loci_t l; loci_init(&l);
    l.enabled = true;
    ASSERT_TRUE(loci_tap_open(&l, path));
    ASSERT_TRUE(!l.tap_wprot);   /* opened r+b */
    loci_tap_motor(&l, true);

    /* REC writes the $0317 DATA byte at the current counter. */
    loci_tap_write(&l, 0x0317, 0x5A);
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_REC);
    ASSERT_EQ(loci_tap_read(&l, 0x0315), 0x00);
    ASSERT_EQ(l.tap_counter, 1);

    FILE* check = fopen(path, "rb");
    ASSERT_EQ(fgetc(check), 0x5A);
    fclose(check);

    /* FFW seeks to end of tape (firmware tap_ffw). */
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_FFW);
    ASSERT_EQ(l.tap_counter, l.tap_size);

    /* REW resets counter AND replays the lead-in (firmware tap_rewind). */
    loci_tap_write(&l, 0x0315, LOCI_TAP_CMD_REW);
    ASSERT_EQ(l.tap_counter, 0);
    ASSERT_EQ(l.tap_lead_in, 0);

    loci_tap_close(&l);
    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(path); free(tmpdir);
}

TEST(test_dsk_four_independent_drives) {
    char* tmpdir = make_tmpdir();
    char* a = make_blob(tmpdir, "a.dsk", 16);
    char* b = make_blob(tmpdir, "b.dsk", 16);
    char* c = make_blob(tmpdir, "c.dsk", 16);
    char* d = make_blob(tmpdir, "d.dsk", 16);
    const char* names[4] = {"a.dsk", "b.dsk", "c.dsk", "d.dsk"};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    for (int i = 0; i < 4; i++) {
        push_path(&l, names[i]);
        l.regs[LOCI_REG_API_A] = (uint8_t)i;
        loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
        ASSERT_TRUE(l.dsk_fp[i] != NULL);
    }
    unlink(a); unlink(b); unlink(c); unlink(d); rmdir(tmpdir);
    loci_cleanup(&l);
    free(a); free(b); free(c); free(d); free(tmpdir);
}

/* ── Sprint 34aw : WD1793 cycle-accurate behind LOCI DSK bus ───── */

TEST(test_dsk_wd1793_restore_command_clears_busy) {
    /* Monte un .DSK minimal, envoie Restore (cmd $08), tickte le FDC, et
     * vérifie que le bit BUSY redescend. Valide que le pipe LOCI → fdc_t
     * route bien les commandes WD1793. */
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "r.dsk", 256 * 17 * 41);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "r.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    /* Drive 0 selected by default. Send Restore. */
    loci_dsk_write(&l, 0x0310, 0x08);
    /* Tick the FDC for ~1 cycle (the implementation completes restore
     * synchronously, BUSY is set then cleared via callbacks). */
    fdc_ticktock(&l.dsk_fdc, 64);
    uint8_t s = loci_dsk_read(&l, 0x0310);
    ASSERT_EQ(s & FDC_ST_BUSY, 0);
    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

TEST(test_dsk_wd1793_ctrl_change_drive_repoints_fdc) {
    /* Mount drives 0 et 1, switch selected, vérifie que le FDC pointe sur
     * le bon buffer (taille distincte pour différencier). */
    char* tmpdir = make_tmpdir();
    char* d0 = make_blob(tmpdir, "0.dsk", 100);
    char* d1 = make_blob(tmpdir, "1.dsk", 200);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    /* mount drive 0 */
    push_path(&l, "0.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    /* mount drive 1 */
    push_path(&l, "1.dsk");
    l.regs[LOCI_REG_API_A] = 1;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    /* Initially drive 0 selected, FDC points at the 100-byte image. */
    ASSERT_EQ(l.dsk_fdc.disk_data, l.dsk_image[0]);
    /* Switch to drive 1 via CTRL ($0314 with bits 5-6 = 1 → 0x20). */
    loci_dsk_write(&l, 0x0314, 0x20);
    ASSERT_EQ(l.dsk_selected, 1);
    ASSERT_EQ(l.dsk_fdc.disk_data, l.dsk_image[1]);
    unlink(d0); unlink(d1); rmdir(tmpdir);
    loci_cleanup(&l); free(d0); free(d1); free(tmpdir);
}

TEST(test_dsk_drq_callback_updates_loci_byte) {
    /* Le callback de fdc_t qu'on a wiré dans loci_init met à jour
     * loci->dsk_drq. Vérifie le path en appelant directement. */
    loci_t l; loci_init(&l);
    l.enabled = true;
    l.dsk_drq = 0xFF;   /* dummy initial */
    /* Simule callback "DRQ set" comme le ferait fdc_t. */
    l.dsk_fdc.set_drq(&l);
    ASSERT_EQ(l.dsk_drq, 0x00);
    l.dsk_fdc.clr_drq(&l);
    ASSERT_EQ(l.dsk_drq, 0x80);
}

/* ── 34ah: scénarios d'intégration composés ──────────────────── */

/* Scénario : monter ROM + 2 disques + 1 tape ensemble, vérifier l'état
 * cohérent, puis tout démonter et vérifier le cleanup. */
TEST(test_integration_full_mount_session) {
    char* tmpdir = make_tmpdir();
    char path[300];

    /* Pré-créer 4 fichiers cibles. */
    snprintf(path, sizeof(path), "%s/rom.bin", tmpdir);
    FILE* fp = fopen(path, "wb"); fputc('R', fp); fclose(fp);
    snprintf(path, sizeof(path), "%s/d0.dsk", tmpdir);
    fp = fopen(path, "wb"); fputc('D', fp); fclose(fp);
    snprintf(path, sizeof(path), "%s/d1.dsk", tmpdir);
    fp = fopen(path, "wb"); fputc('E', fp); fclose(fp);
    char* tap_path = make_tap_with_one_header(tmpdir, "game.tap");

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    struct { uint8_t drive; const char* name; } steps[] = {
        { LOCI_MNT_ROM, "rom.bin"  },
        { 0,            "d0.dsk"   },
        { 1,            "d1.dsk"   },
        { LOCI_MNT_TAP, "game.tap" },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        push_path(&l, steps[i].name);
        l.regs[LOCI_REG_API_A] = steps[i].drive;
        loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
        ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
        ASSERT_TRUE(l.mnt_mounted[steps[i].drive]);
    }
    /* DSK slots 0/1 should hold open handles, TAP backend opened. */
    ASSERT_TRUE(l.dsk_fp[0] != NULL);
    ASSERT_TRUE(l.dsk_fp[1] != NULL);
    ASSERT_TRUE(l.dsk_fp[2] == NULL);
    ASSERT_TRUE(l.dsk_fp[3] == NULL);
    ASSERT_TRUE(l.tap_fp   != NULL);

    /* Unmount everything. */
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        l.regs[LOCI_REG_API_A] = steps[i].drive;
        loci_write(&l, 0x03AF, LOCI_OP_UMOUNT);
        ASSERT_TRUE(!l.mnt_mounted[steps[i].drive]);
    }
    ASSERT_TRUE(l.dsk_fp[0] == NULL);
    ASSERT_TRUE(l.dsk_fp[1] == NULL);
    ASSERT_TRUE(l.tap_fp   == NULL);

    /* Cleanup files. */
    snprintf(path, sizeof(path), "%s/rom.bin", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/d0.dsk", tmpdir);  unlink(path);
    snprintf(path, sizeof(path), "%s/d1.dsk", tmpdir);  unlink(path);
    unlink(tap_path);
    rmdir(tmpdir);
    loci_cleanup(&l); free(tap_path); free(tmpdir);
}

/* Scénario : énumération complète d'un dossier avec 3 fichiers.
 * Vérifie qu'opendir + 3 readdir + 1 readdir vide + closedir convergent. */
TEST(test_integration_dir_enumeration) {
    char* tmpdir = make_tmpdir();
    const char* names[] = { "alpha.txt", "beta.bin", "gamma.dat" };
    char path[300];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", tmpdir, names[i]);
        FILE* fp = fopen(path, "wb"); fputc('!', fp); fclose(fp);
    }

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, ".");
    loci_write(&l, 0x03AF, LOCI_OP_OPENDIR);
    int dfd = l.regs[LOCI_REG_API_A];
    ASSERT_TRUE(dfd >= LOCI_DIR_OFFSET);

    bool seen[3] = {false, false, false};
    int total_seen = 0;
    /* Up to 6 readdir calls — should yield 3 named + then empty terminator. */
    for (int round = 0; round < 6; round++) {
        l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
        loci_write(&l, 0x03AF, LOCI_OP_READDIR);
        uint16_t base = l.xstack_ptr;
        char name[65] = {0};
        for (int i = 0; i < 64; i++) name[i] = (char)l.xstack[base + 2 + i];
        if (!name[0]) break;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strcmp(name, names[i]) == 0 && !seen[i]) {
                seen[i] = true;
                total_seen++;
                break;
            }
        }
    }
    ASSERT_EQ(total_seen, 3);
    ASSERT_TRUE(seen[0] && seen[1] && seen[2]);

    l.regs[LOCI_REG_API_A] = (uint8_t)dfd;
    loci_write(&l, 0x03AF, LOCI_OP_CLOSEDIR);

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", tmpdir, names[i]);
        unlink(path);
    }
    rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Scénario : TAP avec plusieurs headers consécutifs. Vérifie que des
 * READ_HEADER successifs trouvent chaque header sans se chevaucher. */
TEST(test_integration_tap_multi_header_scan) {
    char* tmpdir = make_tmpdir();
    char path[300];
    snprintf(path, sizeof(path), "%s/multi.tap", tmpdir);

    /* Construire un TAP avec 3 headers, séparés par des données filler. */
    FILE* fp = fopen(path, "wb");
    static const uint8_t hdr1[16] = {
        0,0,0,0xC7, 0x09,0xFF, 0x05,0x00, 0,
        'A','A','A',0,0,0,0
    };
    static const uint8_t hdr2[16] = {
        0,0,0,0xC7, 0xFF,0x00, 0x00,0x10, 0,
        'B','B','B',0,0,0,0
    };
    static const uint8_t hdr3[16] = {
        0,0,0,0xC7, 0x10,0xCC, 0x20,0x10, 0,
        'C','C','C',0,0,0,0
    };
    /* Lead-in, sync, header, padding, sync, header, padding, sync, header. */
    for (int i = 0; i < 4; i++) fputc(0, fp);
    fputc(0x16, fp); fputc(0x16, fp); fputc(0x16, fp); fputc(0x24, fp);
    fwrite(hdr1, 1, 16, fp);
    for (int i = 0; i < 6; i++) fputc(0x55, fp);
    fputc(0x16, fp); fputc(0x16, fp); fputc(0x16, fp); fputc(0x24, fp);
    fwrite(hdr2, 1, 16, fp);
    for (int i = 0; i < 6; i++) fputc(0xAA, fp);
    fputc(0x16, fp); fputc(0x16, fp); fputc(0x16, fp); fputc(0x24, fp);
    fwrite(hdr3, 1, 16, fp);
    fclose(fp);

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "multi.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);

    char first_byte[3] = {0};
    for (int i = 0; i < 3; i++) {
        loci_write(&l, 0x03AF, LOCI_OP_TAP_READ_HEADER);
        /* Filename starts at offset 9. */
        first_byte[i] = (char)l.xstack[l.xstack_ptr + 9];
    }
    /* Should have read 'A', 'B', 'C' in order. */
    ASSERT_EQ(first_byte[0], 'A');
    ASSERT_EQ(first_byte[1], 'B');
    ASSERT_EQ(first_byte[2], 'C');

    unlink(path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Scénario : MIA_BOOT flow complet — préparer un ROM mounté, déclencher
 * boot, vérifier que le callback reçoit le bon path puis ax=0. */
TEST(test_integration_mia_boot_with_mounted_rom) {
    char* tmpdir = make_tmpdir();
    char rom_path[300];
    snprintf(rom_path, sizeof(rom_path), "%s/myrom.bin", tmpdir);
    FILE* fp = fopen(rom_path, "wb");
    for (int i = 0; i < 256; i++) fputc(0xEA, fp);   /* NOPs */
    fclose(fp);

    rom_swap_capture_t cap = { .result = true };
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    loci_set_rom_swap_callback(&l, capture_swap, &cap);

    /* Mount custom ROM in slot 5. */
    push_path(&l, "myrom.bin");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_ROM;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);

    /* Boot — should use the custom ROM, not basic10/basic11b. */
    l.regs[LOCI_REG_API_A] = 0;   /* default settings */
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
    ASSERT_EQ(cap.call_count, 1);
    ASSERT_TRUE(strstr(cap.last_path, "myrom.bin") != NULL);
    ASSERT_TRUE(strstr(cap.last_path, "basic")     == NULL);
    ASSERT_EQ(cap.last_base, 0xC000);

    unlink(rom_path); rmdir(tmpdir);
    loci_cleanup(&l); free(tmpdir);
}

/* Scénario : configuration HID kbd_xram puis injection d'une combo
 * Shift+A. Le bitmap doit refléter les deux états simultanément. */
TEST(test_integration_hid_combo_shift_a) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x4000);   /* configurer kbd_xram */

    /* HID_KEY_A = 0x04, modifier LEFT_SHIFT = 0x02. */
    uint8_t kc[6] = {0x04, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0x02, kc);

    /* Byte 0 bit 4 (= 1 << (0x04 & 7)) should be set. */
    ASSERT_TRUE(l.xram[0x4000] & 0x10);
    /* Byte 28 should hold modifier 0x02. */
    ASSERT_EQ(l.xram[0x4000 + 28], 0x02);
    /* No-key sentinel must be cleared (a key is down). */
    ASSERT_EQ(l.xram[0x4000] & 0x01, 0);
}

/* Scénario : la ROM LOCI binaire est cohérente — taille 16 KB, reset
 * vector pointe dans la plage ROM ($C000-$FFFF). */
TEST(test_integration_locirom_binary_sanity) {
    FILE* fp = fopen("roms/loci/locirom", "rb");
    if (!fp) {
        printf("(roms/loci/locirom absent — skip) ");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    ASSERT_EQ(sz, 16384);

    /* Reset vector lit à offset $3FFC-$3FFD (correspond à $FFFC). */
    fseek(fp, 0x3FFC, SEEK_SET);
    uint8_t lo = (uint8_t)fgetc(fp);
    uint8_t hi = (uint8_t)fgetc(fp);
    fclose(fp);
    uint16_t reset = (uint16_t)lo | ((uint16_t)hi << 8);
    ASSERT_TRUE(reset >= 0xC000);
}

/* Scénario : mount d'un drive 0 + accès bus $0310 — la ROM peut probe
 * via le bus sans avoir à utiliser l'API. Vérifie la cohérence. */
TEST(test_integration_dsk_mount_then_bus_probe) {
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "boot.dsk", 64);
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);
    push_path(&l, "boot.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);

    /* Selected drive = 0 by default. NOT_READY should be cleared. */
    uint8_t st = loci_dsk_read(&l, 0x0310);
    ASSERT_EQ(st & LOCI_DSK_STAT_NOT_READY, 0);

    /* Switch via CTRL to drive 1 (non monté) — should report NOT_READY. */
    loci_dsk_write(&l, 0x0314, 1u << LOCI_DSK_CTRL_DRV_SEL_SHIFT);
    ASSERT_EQ(l.dsk_selected, 1);
    st = loci_dsk_read(&l, 0x0310);
    ASSERT_TRUE((st & LOCI_DSK_STAT_NOT_READY) != 0);

    unlink(dsk_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tmpdir);
}

/* ── 34ai: Action button (warm IRQ trap) ─────────────────────── */

typedef struct {
    int install_count;
    int release_count;
} action_capture_t;

static void capture_install(void* ctx) {
    ((action_capture_t*)ctx)->install_count++;
}
static void capture_release(void* ctx) {
    ((action_capture_t*)ctx)->release_count++;
}

TEST(test_action_short_writes_trap_bytes) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_action_button_short(&l);
    /* Trap mirrored at MIA offsets 0x1A-0x1F. */
    ASSERT_EQ(l.regs[0x1A], 0xB8);
    ASSERT_EQ(l.regs[0x1B], 0x50);
    ASSERT_EQ(l.regs[0x1C], 0xFE);
    ASSERT_EQ(l.regs[0x1D], 0x6C);
    ASSERT_EQ(l.regs[0x1E], 0xFA);
    ASSERT_EQ(l.regs[0x1F], 0xFF);
    ASSERT_TRUE(l.action_active);
}

TEST(test_action_short_invokes_install_cb) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_short(&l);
    ASSERT_EQ(cap.install_count, 1);
    ASSERT_EQ(cap.release_count, 0);
}

TEST(test_action_short_idempotent_when_already_active) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_short(&l);
    loci_action_button_short(&l);   /* second press while active */
    loci_action_button_short(&l);
    ASSERT_EQ(cap.install_count, 1);
}

TEST(test_action_release_invokes_release_cb) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_short(&l);
    loci_action_button_release(&l);
    ASSERT_EQ(cap.release_count, 1);
    ASSERT_TRUE(!l.action_active);
}

TEST(test_action_release_noop_when_inactive) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_release(&l);   /* never short-pressed */
    ASSERT_EQ(cap.release_count, 0);
}

TEST(test_action_disabled_loci_is_noop) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = false;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_short(&l);
    ASSERT_EQ(cap.install_count, 0);
    ASSERT_TRUE(!l.action_active);
}

TEST(test_action_works_without_callbacks) {
    /* No host hooks installed — the LOCI side still flips state and
     * writes the trap bytes, just no IRQ pulse / V-flag-set happens. */
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_action_button_short(&l);
    ASSERT_TRUE(l.action_active);
    ASSERT_EQ(l.regs[0x1A], 0xB8);
    loci_action_button_release(&l);
    ASSERT_TRUE(!l.action_active);
}

TEST(test_action_re_press_after_release) {
    action_capture_t cap = {0};
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_action_callbacks(&l, capture_install, capture_release, &cap);
    loci_action_button_short(&l);
    loci_action_button_release(&l);
    loci_action_button_short(&l);   /* second press cycle */
    loci_action_button_release(&l);
    ASSERT_EQ(cap.install_count, 2);
    ASSERT_EQ(cap.release_count, 2);
}

/* ── 34aj: F5/F8 SDL bindings — Reset preserves mounts ───────── */

TEST(test_loci_reset_preserves_mounts) {
    /* Sprint 34aj: F5 also calls loci_reset() when --loci is active.
     * Verify that loci_reset zeroes MIA register state but keeps the
     * mount table + open file handles. */
    char* tmpdir = make_tmpdir();
    char* dsk_path = make_blob(tmpdir, "side.dsk", 32);
    char* tap_path = make_tap_with_one_header(tmpdir, "tape.tap");

    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_set_flash_root(&l, tmpdir);

    /* Mount drive 0 and a tape. */
    push_path(&l, "side.dsk");
    l.regs[LOCI_REG_API_A] = 0;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    push_path(&l, "tape.tap");
    l.regs[LOCI_REG_API_A] = LOCI_MNT_TAP;
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);
    ASSERT_TRUE(l.mnt_mounted[0]);
    ASSERT_TRUE(l.mnt_mounted[LOCI_MNT_TAP]);
    ASSERT_TRUE(l.dsk_fp[0] != NULL);
    ASSERT_TRUE(l.tap_fp   != NULL);

    /* Dirty some MIA state. */
    loci_write(&l, 0x03AC, 0xAA);   /* xstack push */
    ASSERT_TRUE(l.xstack_ptr < LOCI_XSTACK_SIZE);

    /* Hard reset (= F5 path) */
    loci_reset(&l);

    /* MIA state cleared. */
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
    ASSERT_EQ(l.regs[LOCI_REG_API_OP], 0);

    /* Mounts preserved — this is what makes the LOCI reset "warm". */
    ASSERT_TRUE(l.mnt_mounted[0]);
    ASSERT_TRUE(l.mnt_mounted[LOCI_MNT_TAP]);
    ASSERT_TRUE(l.dsk_fp[0] != NULL);
    ASSERT_TRUE(l.tap_fp   != NULL);

    unlink(dsk_path); unlink(tap_path); rmdir(tmpdir);
    loci_cleanup(&l); free(dsk_path); free(tap_path); free(tmpdir);
}

/* ── 34ak: SDL → HID bridge (mapping et bitmap conventions) ──── */

/* Pas de dépendance SDL ici — on teste juste que le format HID utilisé
 * par le bridge SDL est cohérent avec les keycodes documentés du firmware.
 * Le bridge lui-même est dans main.c et utilise SDL_GetKeyboardState. */

TEST(test_hid_a_is_0x04) {
    /* Boot keyboard HID usage page : A = 0x04 (same as SDL_SCANCODE_A). */
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x2000);   /* set kbd_xram */
    uint8_t kc[6] = {0x04, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc);
    /* Bit (4 & 7) = bit 4 of byte (4 >> 3) = byte 0. */
    ASSERT_EQ(l.xram[0x2000] & 0x10, 0x10);
}

TEST(test_hid_enter_is_0x28) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x2000);
    uint8_t kc[6] = {0x28, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc);
    /* 0x28 >> 3 = 5, 0x28 & 7 = 0 → byte 5 bit 0. */
    ASSERT_EQ(l.xram[0x2000 + 5] & 0x01, 0x01);
}

TEST(test_hid_up_arrow_is_0x52) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x2000);
    uint8_t kc[6] = {0x52, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, kc);
    /* 0x52 >> 3 = 10, 0x52 & 7 = 2 → byte 10 bit 2. */
    ASSERT_EQ(l.xram[0x2000 + 10] & 0x04, 0x04);
}

TEST(test_hid_six_simultaneous_keys) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x2000);
    /* Press A,B,C,D,E,F simultaneously (HID 0x04..0x09). */
    uint8_t kc[6] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    loci_kbd_set_report(&l, 0, kc);
    /* All 6 bits in byte 0 + byte 1 should be set (bits 4..7 of byte 0
     * + bits 0..1 of byte 1). */
    ASSERT_EQ(l.xram[0x2000] & 0xF0, 0xF0);   /* A,B,C,D in bits 4-7 */
    ASSERT_EQ(l.xram[0x2001] & 0x03, 0x03);   /* E,F in bits 0-1 */
}

TEST(test_hid_release_clears_corresponding_bits) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 0, 0x2000);

    /* Press A. */
    uint8_t k1[6] = {0x04, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, k1);
    ASSERT_EQ(l.xram[0x2000] & 0x10, 0x10);

    /* Release: empty report = all bits clear + sentinel. */
    uint8_t k2[6] = {0, 0, 0, 0, 0, 0};
    loci_kbd_set_report(&l, 0, k2);
    ASSERT_EQ(l.xram[0x2000] & 0x10, 0);
    ASSERT_EQ(l.xram[0x2000] & 0x01, 0x01);   /* no-key sentinel */
}

/* ── 34al: bridge souris (loci_mou_report) ───────────────────── */

TEST(test_mou_report_writes_buttons_and_deltas) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 1, 0x3000);   /* set mou_xram */

    loci_mou_report(&l, 0x05, 3, -2, 1, 0);
    /* Layout: buttons / dx / dy / wheel / pan */
    ASSERT_EQ(l.xram[0x3000 + 0], 0x05);
    ASSERT_EQ(l.xram[0x3000 + 1], 3);
    ASSERT_EQ(l.xram[0x3000 + 2], (uint8_t)(-2));
    ASSERT_EQ(l.xram[0x3000 + 3], 1);
    ASSERT_EQ(l.xram[0x3000 + 4], 0);
}

TEST(test_mou_report_accumulates_deltas) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 1, 0x3000);
    loci_mou_report(&l, 0, 5, 5, 0, 0);
    loci_mou_report(&l, 0, 3, 4, 0, 0);
    ASSERT_EQ(l.xram[0x3001], 8);    /* 5 + 3 */
    ASSERT_EQ(l.xram[0x3002], 9);    /* 5 + 4 */
}

TEST(test_mou_report_noop_when_xram_unset) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    /* mou_xram is 0xFFFF (default). */
    for (int i = 0; i < 8; i++) l.xram[i] = 0xAB;
    loci_mou_report(&l, 0x01, 10, 10, 0, 0);
    for (int i = 0; i < 8; i++) ASSERT_EQ(l.xram[i], 0xAB);
}

TEST(test_mou_report_buttons_independent_of_deltas) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    pix_xreg_mia(&l, 0, 1, 0x3000);
    loci_mou_report(&l, 0, 7, 7, 0, 0);   /* deltas only */
    ASSERT_EQ(l.xram[0x3000], 0);
    loci_mou_report(&l, 0x02, 0, 0, 0, 0); /* right button, no delta */
    ASSERT_EQ(l.xram[0x3000], 0x02);
    ASSERT_EQ(l.xram[0x3001], 7);          /* delta unchanged */
}

/* ── Sprint 34au : 7 tuning / config stubs ───────────────────── */

TEST(test_op_cpu_phi2_returns_1mhz) {
    loci_t l; loci_init(&l); l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_CPU_PHI2);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    /* Firmware convention (cpu.c cpu_api_phi2): AX = PHI2 in kHz.
     * 1 MHz = 1000 = $03E8 → A=$E8, X=$03 (Sprint 36e fix). */
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0xE8);
    ASSERT_EQ(l.regs[LOCI_REG_API_X], 0x03);
    ASSERT_EQ(l.op_count[LOCI_OP_CPU_PHI2], 1);
}

TEST(test_op_oem_codepage_noop_success) {
    loci_t l; loci_init(&l); l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0xB5;  /* arbitrary codepage byte */
    loci_write(&l, 0x03AF, LOCI_OP_OEM_CODEPAGE);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(l.op_count[LOCI_OP_OEM_CODEPAGE], 1);
}

TEST(test_op_stdin_opt_noop_success) {
    loci_t l; loci_init(&l); l.enabled = true;
    l.regs[LOCI_REG_API_A] = 0xFF;
    loci_write(&l, 0x03AF, LOCI_OP_STDIN_OPT);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(l.op_count[LOCI_OP_STDIN_OPT], 1);
}

/* Helper for the 5 MAP_TUNE_* — push some bytes into xstack first to
 * verify they are consumed (xstack_ptr returns to top). */
static void run_map_tune_test(uint8_t op) {
    loci_t l; loci_init(&l); l.enabled = true;
    /* Pre-populate xstack with 4 bytes that the op should consume. */
    l.xstack_ptr = LOCI_XSTACK_SIZE - 4;
    l.xstack[LOCI_XSTACK_SIZE - 1] = 0xAA;
    l.xstack[LOCI_XSTACK_SIZE - 2] = 0xBB;
    l.xstack[LOCI_XSTACK_SIZE - 3] = 0xCC;
    l.xstack[LOCI_XSTACK_SIZE - 4] = 0xDD;
    loci_write(&l, 0x03AF, op);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
    ASSERT_EQ(l.op_count[op], 1);
}

TEST(test_op_map_tune_tmap_clears_xstack) { run_map_tune_test(LOCI_OP_MAP_TUNE_TMAP); }
TEST(test_op_map_tune_tior_clears_xstack) { run_map_tune_test(LOCI_OP_MAP_TUNE_TIOR); }
TEST(test_op_map_tune_tiow_clears_xstack) { run_map_tune_test(LOCI_OP_MAP_TUNE_TIOW); }
TEST(test_op_map_tune_tiod_clears_xstack) { run_map_tune_test(LOCI_OP_MAP_TUNE_TIOD); }
TEST(test_op_map_tune_tadr_clears_xstack) { run_map_tune_test(LOCI_OP_MAP_TUNE_TADR); }

/* ── MIA bus-timing model (picowifi reliability) ─────────────── */

TEST(test_mia_defaults) {
    loci_t l; loci_init(&l);
    ASSERT_EQ(l.mia_tmap, 10);
    ASSERT_EQ(l.mia_tula, 10);
    ASSERT_EQ(l.mia_tior, 0);
    ASSERT_EQ(l.mia_tior_lo, 0);
    ASSERT_EQ(l.mia_tior_hi, 31);
    ASSERT_TRUE(loci_mia_io_reliable(&l));   /* open window → always reliable */
}

/* Firmware map.c/cfg.c semantics: A <= 31 sets and the op returns the
 * CURRENT value in AX; A > 31 is a pure query (value untouched). The
 * stored value is unmasked — the firmware masks only at the PIO level. */
TEST(test_mia_tune_tior_stores_value) {
    loci_t l; loci_init(&l); l.enabled = true;
    l.regs[LOCI_REG_API_A] = 15;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOR);
    ASSERT_EQ(l.mia_tior, 15);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 15);   /* AX = current value */
    ASSERT_EQ(l.regs[LOCI_REG_API_X], 0);
    /* Query form: A > 31 must NOT modify the setting */
    l.regs[LOCI_REG_API_A] = 0xFF;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOR);
    ASSERT_EQ(l.mia_tior, 15);               /* unchanged */
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 15);   /* AX = current value */
    /* tiod: stored unmasked (cfg keeps <= 31; PIO alone applies & 0x07) */
    l.regs[LOCI_REG_API_A] = 12;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOD);
    ASSERT_EQ(l.mia_tiod, 12);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 12);
}

/* ADJ_SCAN leaves xram[$FFF0] holding the configured tior, bit 7 clear
 * (firmware adj.c ADJ_CLEANUP), not a hardwired zero. */
TEST(test_adj_scan_reports_current_tior) {
    loci_t l; loci_init(&l); l.enabled = true;
    l.regs[LOCI_REG_API_A] = 21;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOR);
    l.xram[0xFFF0] = 0xFF;
    loci_write(&l, 0x03AF, LOCI_OP_ADJ_SCAN);
    ASSERT_EQ(l.xram[0xFFF0], 21);
}

TEST(test_mia_window_blocks_and_tunes) {
    loci_t l; loci_init(&l); l.enabled = true;
    loci_set_mia_window(&l, 12, 18);         /* only tior 12..18 works */
    ASSERT_TRUE(!loci_mia_io_reliable(&l));  /* boot tior=0 → unreachable */
    l.regs[LOCI_REG_API_A] = 15;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOR);
    ASSERT_TRUE(loci_mia_io_reliable(&l));   /* tuned into window */
    l.regs[LOCI_REG_API_A] = 25;
    loci_write(&l, 0x03AF, LOCI_OP_MAP_TUNE_TIOR);
    ASSERT_TRUE(!loci_mia_io_reliable(&l));  /* over-shoot → unreachable */
}

TEST(test_mia_set_window_clamps_and_orders) {
    loci_t l; loci_init(&l);
    loci_set_mia_window(&l, 40, 5);          /* 40→31, then swap */
    ASSERT_EQ(l.mia_tior_lo, 5);
    ASSERT_EQ(l.mia_tior_hi, 31);
}

/* ── reset ──────────────────────────────────────────────────── */

TEST(test_reset_clears_state) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_OPEN);
    loci_write(&l, 0x03A0, 0x55);
    loci_reset(&l);
    ASSERT_EQ(l.regs[LOCI_REG_API_ERRNO_LO], 0);
    ASSERT_EQ(l.regs[0], 0);
    ASSERT_EQ(l.active_op, 0);
    ASSERT_EQ(l.xstack_ptr, LOCI_XSTACK_SIZE);
}

/* ── 6502 differential ABI test : real CPU executes JSR $03B0 ────
 *
 * The whole point of this test : exercise the MIA spin-window ABI from
 * the CPU side, not by calling handlers directly. Builds a tiny program
 * that triggers an op, JSR $03B0, captures the returned A/X/SREG into
 * RAM, then BRKs. If the spin window isn't materialised correctly the
 * CPU never returns and we time out. */

static loci_t g_loci_for_diff;

static uint8_t diff_io_read(uint16_t addr, void* ud) {
    (void)ud;
    if (loci_addr_in_mia(addr)) return loci_read(&g_loci_for_diff, addr);
    return 0xFF;
}
static void diff_io_write(uint16_t addr, uint8_t val, void* ud) {
    (void)ud;
    if (loci_addr_in_mia(addr)) loci_write(&g_loci_for_diff, addr, val);
}

TEST(test_6502_jsr_spin_returns_via_released_stub) {
    /* Set up real CPU + memory + LOCI on the bus. */
    memory_t mem;
    memory_init(&mem);
    memory_set_io_callbacks(&mem, diff_io_read, diff_io_write, NULL);
    loci_init(&g_loci_for_diff);
    g_loci_for_diff.enabled = true;

    cpu6502_t cpu;
    cpu_init(&cpu, &mem);

    /* Test program at $C000 :
     *
     *   $C000  LDA #$04           ; op = RNG_LRAND (handler always returns OK)
     *   $C002  STA $03AF          ; trigger op (installs blocked then resolves)
     *   $C005  20 B0 03           JSR $03B0   ; spin → CLV/BVC+0/LDA/LDX/RTS
     *   $C008  STA $0200          ; record A
     *   $C00B  STX $0201          ; record X
     *   $C00E  LDA $03B8          ; SREG lo
     *   $C011  STA $0202
     *   $C014  LDA $03B9          ; SREG hi
     *   $C017  STA $0203
     *   $C01A  00                 ; BRK (sentinel for test end)
     *
     * Reset vector $FFFC/D → $C000. */
    static const uint8_t program[] = {
        /* C000 */ 0xA9, 0x04,
        /* C002 */ 0x8D, 0xAF, 0x03,
        /* C005 */ 0x20, 0xB0, 0x03,
        /* C008 */ 0x8D, 0x00, 0x02,
        /* C00B */ 0x8E, 0x01, 0x02,
        /* C00E */ 0xAD, 0xB8, 0x03,
        /* C011 */ 0x8D, 0x02, 0x02,
        /* C014 */ 0xAD, 0xB9, 0x03,
        /* C017 */ 0x8D, 0x03, 0x02,
        /* C01A */ 0x00
    };
    /* Memory model: writes to ROM area via memory_write go to ram or rom
     * depending on banking — for the test we poke ram directly. The CPU
     * fetch path will route through memory_read which respects the I/O
     * range for $03A0-BF but otherwise reads from RAM/ROM. We use the
     * RAM region (memory_read reads ram for $0000-$BFFF) — but $C000+
     * is in ROM. So we put the program at $0300... wait, $0300 is in I/O.
     *
     * Safest: put the program at $0400 (well above zero page+stack,
     * outside I/O, in RAM). Reset vector points there. */
    for (size_t i = 0; i < sizeof(program); i++) {
        memory_write(&mem, (uint16_t)(0x0400 + i), program[i]);
    }
    /* Patch the JSR target absolute address embedded above to point to
     * the correct $03B0 spin window — already $03B0 so just fix the
     * program offsets for STA $03AF in the relocated copy : */
    /* The program above is position-independent for the data accesses
     * ($03AF, $03B0, $0200, etc. are absolute) so it works at $0400.
     * Reset vector → $0400. */
    /* Reset vector goes to ROM (memory_write to $C000-$FFFF is ignored
     * when ROM is enabled by default). Poke mem.rom directly. */
    mem.rom[0x3FFC] = 0x00;
    mem.rom[0x3FFD] = 0x04;
    cpu_reset(&cpu);

    /* Run until BRK or 1000 cycles (the spin needs ~12 cycles, total ~50). */
    int total = 0;
    for (int i = 0; i < 200; i++) {
        if (memory_read(&mem, cpu.PC) == 0x00) break;  /* BRK = test end */
        total += cpu_step(&cpu);
        if (total > 1000) break;  /* runaway guard */
    }

    /* Verify we exited the spin via released stub. */
    ASSERT_TRUE(total < 1000);
    /* RNG_LRAND returns a 31-bit positive uint32 via AXSREG.
     * High bit (bit 7 of SREG hi at $03B9) must be cleared. */
    uint8_t a   = memory_read(&mem, 0x0200);
    uint8_t x   = memory_read(&mem, 0x0201);
    uint8_t srl = memory_read(&mem, 0x0202);
    uint8_t srh = memory_read(&mem, 0x0203);
    uint32_t axsreg = (uint32_t)a | ((uint32_t)x << 8) |
                     ((uint32_t)srl << 16) | ((uint32_t)srh << 24);
    ASSERT_TRUE((axsreg & 0x80000000u) == 0);   /* 31-bit positive */
    /* Released stub must leave BUSY ($03B2 bit 7) cleared. */
    ASSERT_TRUE((g_loci_for_diff.regs[LOCI_REG_BUSY] & 0x80) == 0);
    /* The CPU executed past the JSR — PC should be at the BRK ($041A). */
    ASSERT_EQ(cpu.PC, 0x041A);

    loci_cleanup(&g_loci_for_diff);
}

TEST(test_6502_jsr_spin_zxstack_op_00) {
    /* Op $00 (zxstack) : clears xstack, returns ax=0 via released stub. */
    memory_t mem;
    memory_init(&mem);
    memory_set_io_callbacks(&mem, diff_io_read, diff_io_write, NULL);
    loci_init(&g_loci_for_diff);
    g_loci_for_diff.enabled = true;

    cpu6502_t cpu;
    cpu_init(&cpu, &mem);

    /* Pre-fill xstack so we can verify it was cleared. */
    loci_write(&g_loci_for_diff, 0x03AC, 0x42);
    ASSERT_TRUE(g_loci_for_diff.xstack_ptr < LOCI_XSTACK_SIZE);

    /* Program : LDA #0 ; STA $03AF ; JSR $03B0 ; STA $0200 ; STX $0201 ; BRK */
    static const uint8_t program[] = {
        0xA9, 0x00,
        0x8D, 0xAF, 0x03,
        0x20, 0xB0, 0x03,
        0x8D, 0x00, 0x02,
        0x8E, 0x01, 0x02,
        0x00
    };
    for (size_t i = 0; i < sizeof(program); i++)
        memory_write(&mem, (uint16_t)(0x0400 + i), program[i]);
    /* Reset vector goes to ROM (memory_write to $C000-$FFFF is ignored
     * when ROM is enabled by default). Poke mem.rom directly. */
    mem.rom[0x3FFC] = 0x00;
    mem.rom[0x3FFD] = 0x04;
    cpu_reset(&cpu);

    int total = 0;
    for (int i = 0; i < 200; i++) {
        if (memory_read(&mem, cpu.PC) == 0x00) break;
        total += cpu_step(&cpu);
        if (total > 1000) break;
    }
    ASSERT_TRUE(total < 1000);
    /* zxstack must reset xstack_ptr. */
    ASSERT_EQ(g_loci_for_diff.xstack_ptr, LOCI_XSTACK_SIZE);
    /* ax = 0 → A = 0, X = 0 */
    ASSERT_EQ(memory_read(&mem, 0x0200), 0);
    ASSERT_EQ(memory_read(&mem, 0x0201), 0);

    loci_cleanup(&g_loci_for_diff);
}

TEST(test_6502_initial_jsr_returns_zero) {
    /* Before any op : initial seeded stub returns A=X=SREG=0 cleanly.
     * Catches regressions where loci_init forgets seed_initial_stub. */
    memory_t mem;
    memory_init(&mem);
    memory_set_io_callbacks(&mem, diff_io_read, diff_io_write, NULL);
    loci_init(&g_loci_for_diff);
    g_loci_for_diff.enabled = true;

    cpu6502_t cpu;
    cpu_init(&cpu, &mem);

    /* Program : JSR $03B0 ; STA $0200 ; STX $0201 ; BRK */
    static const uint8_t program[] = {
        0x20, 0xB0, 0x03,
        0x8D, 0x00, 0x02,
        0x8E, 0x01, 0x02,
        0x00
    };
    for (size_t i = 0; i < sizeof(program); i++)
        memory_write(&mem, (uint16_t)(0x0400 + i), program[i]);
    /* Reset vector goes to ROM (memory_write to $C000-$FFFF is ignored
     * when ROM is enabled by default). Poke mem.rom directly. */
    mem.rom[0x3FFC] = 0x00;
    mem.rom[0x3FFD] = 0x04;
    cpu_reset(&cpu);

    int total = 0;
    for (int i = 0; i < 200; i++) {
        if (memory_read(&mem, cpu.PC) == 0x00) break;
        total += cpu_step(&cpu);
        if (total > 1000) break;
    }
    ASSERT_TRUE(total < 1000);
    ASSERT_EQ(memory_read(&mem, 0x0200), 0);
    ASSERT_EQ(memory_read(&mem, 0x0201), 0);

    loci_cleanup(&g_loci_for_diff);
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);
    printf("\n");
    printf("===========================================================\n");
    printf("  Phosphoric LOCI Emulation Tests (Sprint 34y skeleton)\n");
    printf("===========================================================\n\n");

    RUN(test_init_zeroes_state);
    RUN(test_addr_in_mia_filter);
    RUN(test_disabled_read_returns_ff);
    RUN(test_disabled_write_is_noop);
    RUN(test_write_read_passthrough);
    RUN(test_out_of_range_read_ff);
    RUN(test_api_op_dispatches_and_sets_enosys);
    RUN(test_api_op_none_does_not_dispatch);
    RUN(test_op_count_increments);
    RUN(test_xstack_push_pop_roundtrip);
    RUN(test_xstack_read_pops_byte);
    RUN(test_op_pix_xreg_empty_xstack_einval);
    RUN(test_op_rng_lrand_returns_axsreg);
    RUN(test_op_clock_returns_uptime);
    RUN(test_op_clk_getres_pushes_one_sec);
    RUN(test_op_clk_gettime_pushes_time);
    RUN(test_op_clk_getres_bad_id_returns_einval);
    RUN(test_unimplemented_op_still_enosys);
    RUN(test_op_open_close_creates_file);
    RUN(test_op_open_missing_returns_enoent);
    RUN(test_open_path_traversal_rejected);
    RUN(test_write_read_roundtrip);
    RUN(test_lseek_set_then_read);
    RUN(test_lseek_whence_cur_and_end_firmware_mapping);
    RUN(test_lseek_short_stack_one_byte_offset);
    RUN(test_xstack_512_bytes_capacity);
    RUN(test_read_xstack_count_above_256);
    RUN(test_unlink_removes_file);
    RUN(test_unlink_removes_empty_directory);
    RUN(test_close_bad_fd_returns_ebadf);
    RUN(test_fd_exhaustion_emfile);
    RUN(test_xram_window0_read_advances);
    RUN(test_xram_window0_write_advances);
    RUN(test_xram_window_step_negative);
    RUN(test_xram_window1_independent);
    RUN(test_mount_records_path);
    RUN(test_mount_missing_file_enoent);
    RUN(test_mount_bad_drive_einval);
    RUN(test_umount_clears_slot);
    RUN(test_dsk_raw_writes_persisted_on_umount);
    RUN(test_read_xram_from_file);
    RUN(test_getcwd_returns_path);
    RUN(test_getcwd_255_derives_rom);
    RUN(test_opendir_closedir);
    RUN(test_opendir_missing_enoent);
    RUN(test_readdir_lists_entries_skipping_dotdot);
    RUN(test_readdir_subdir_uses_correct_base);
    RUN(test_path_allows_double_dot_in_name);
    RUN(test_path_rejects_dotdot_component);
    RUN(test_op_read_xstack_bad_fd_ebadf);
    RUN(test_op_write_xstack_bad_fd_ebadf);
    RUN(test_op_lseek_bad_fd_ebadf);
    RUN(test_op_lseek_bad_whence_einval);
    RUN(test_op_unlink_missing_enoent);
    RUN(test_unlink_nonempty_directory_returns_eacces);
    RUN(test_op_rename_missing_enoent);
    RUN(test_op_rename_renames_existing);
    RUN(test_op_readdir_bad_fd_ebadf);
    RUN(test_op_mkdir_traversal_eacces);
    RUN(test_mkdir_creates_dir);
    RUN(test_closedir_bad_fd_ebadf);
    RUN(test_uname_pushes_5_fields);
    RUN(test_uname_release_uses_emu_version);
    RUN(test_pix_xreg_kbd_sets_kbd_xram);
    RUN(test_pix_xreg_mou_sets_mou_xram);
    RUN(test_pix_xreg_pad_sets_pad_xram);
    RUN(test_pix_xreg_disables_kbd_with_ffff);
    RUN(test_pix_xreg_bad_device_einval);
    RUN(test_kbd_set_report_writes_bitmap);
    RUN(test_kbd_set_report_modifier_byte28);
    RUN(test_kbd_set_report_no_key_sentinel);
    RUN(test_kbd_clear_resets_bitmap);
    RUN(test_kbd_set_report_noop_when_xram_unset);
    RUN(test_mia_boot_resume_no_callback_invocation);
    RUN(test_mia_boot_resume_invokes_resume_callback);
    RUN(test_mia_boot_basic10_default);
    RUN(test_mia_boot_b11_basic11b);
    RUN(test_mia_boot_uses_mounted_rom_slot);
    RUN(test_mia_boot_callback_failure_returns_eio);
    RUN(test_mia_boot_no_callback_acks);
    RUN(test_mia_boot_settings_latched);
    RUN(test_mia_boot_fdc_loads_microdisc_too);
    RUN(test_tap_mount_opens_file);
    RUN(test_tap_umount_closes);
    RUN(test_tap_tell_returns_zero_after_mount);
    RUN(test_tap_seek_updates_counter);
    RUN(test_tap_seek_clamps_to_size_minus_1);
    RUN(test_tap_read_header_finds_sync_and_pushes_header);
    RUN(test_tap_read_header_no_sync_returns_enoent);
    RUN(test_tap_seek_no_tape_returns_enodev);
    RUN(test_tap_io_stat_reports_not_ready_when_empty);
    RUN(test_tap_io_cmd_rewind_resets_counter);
    RUN(test_dsk_mount_opens_drive_image);
    RUN(test_dsk_umount_closes);
    RUN(test_dsk_bad_sector_seeded_at_mount);
    RUN(test_opendir_empty_lists_devices);
    RUN(test_usb_storage_paths_resolve_to_host_root);
    RUN(test_firmware_abi_dir_fd_and_efatfs);
    RUN(test_dsk_cmd_reports_not_ready_for_empty_drive);
    RUN(test_dsk_cmd_ready_for_mounted_drive);
    RUN(test_dsk_ctrl_select_drive_via_bits_5_6);
    RUN(test_dsk_track_sect_data_passthrough);
    RUN(test_dsk_drq_register);
    RUN(test_dsk_id_register_returns_loci_marker);
    RUN(test_dsk_id_register_ff_when_disabled);
    RUN(test_dsk_spare_registers_31a_31b);
    RUN(test_dsk_irq_register_exact_bytes);
    RUN(test_cons_flags_tx_ready_rx_pending);
    RUN(test_cons_rx_fifo_order);
    RUN(test_cons_tx_line_buffer_flushes_on_newline);
    RUN(test_ula_snoop_write_clears_register);
    RUN(test_op_adj_scan_reports_completed);
    RUN(test_tap_play_lead_in_then_data);
    RUN(test_tap_play_is_motor_protected);
    RUN(test_tap_read_bit_14bit_frame);
    RUN(test_tap_rec_writes_byte_and_ffw);
    RUN(test_dsk_four_independent_drives);
    RUN(test_dsk_wd1793_restore_command_clears_busy);
    RUN(test_dsk_wd1793_ctrl_change_drive_repoints_fdc);
    RUN(test_dsk_drq_callback_updates_loci_byte);
    RUN(test_integration_full_mount_session);
    RUN(test_integration_dir_enumeration);
    RUN(test_integration_tap_multi_header_scan);
    RUN(test_integration_mia_boot_with_mounted_rom);
    RUN(test_integration_hid_combo_shift_a);
    RUN(test_integration_locirom_binary_sanity);
    RUN(test_integration_dsk_mount_then_bus_probe);
    RUN(test_action_short_writes_trap_bytes);
    RUN(test_action_short_invokes_install_cb);
    RUN(test_action_short_idempotent_when_already_active);
    RUN(test_action_release_invokes_release_cb);
    RUN(test_action_release_noop_when_inactive);
    RUN(test_action_disabled_loci_is_noop);
    RUN(test_action_works_without_callbacks);
    RUN(test_action_re_press_after_release);
    RUN(test_loci_reset_preserves_mounts);
    RUN(test_hid_a_is_0x04);
    RUN(test_hid_enter_is_0x28);
    RUN(test_hid_up_arrow_is_0x52);
    RUN(test_hid_six_simultaneous_keys);
    RUN(test_hid_release_clears_corresponding_bits);
    RUN(test_mou_report_writes_buttons_and_deltas);
    RUN(test_mou_report_accumulates_deltas);
    RUN(test_mou_report_noop_when_xram_unset);
    RUN(test_mou_report_buttons_independent_of_deltas);
    RUN(test_6502_initial_jsr_returns_zero);
    RUN(test_6502_jsr_spin_zxstack_op_00);
    RUN(test_6502_jsr_spin_returns_via_released_stub);
    RUN(test_op_cpu_phi2_returns_1mhz);
    RUN(test_op_oem_codepage_noop_success);
    RUN(test_op_stdin_opt_noop_success);
    RUN(test_op_map_tune_tmap_clears_xstack);
    RUN(test_op_map_tune_tior_clears_xstack);
    RUN(test_op_map_tune_tiow_clears_xstack);
    RUN(test_op_map_tune_tadr_clears_xstack);
    RUN(test_op_map_tune_tiod_clears_xstack);
    RUN(test_mia_defaults);
    RUN(test_mia_tune_tior_stores_value);
    RUN(test_adj_scan_reports_current_tior);
    RUN(test_mia_window_blocks_and_tunes);
    RUN(test_mia_set_window_clamps_and_orders);
    RUN(test_reset_clears_state);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_run - tests_passed, tests_run);
    printf("===========================================================\n\n");
    return tests_passed == tests_run ? 0 : 1;
}
