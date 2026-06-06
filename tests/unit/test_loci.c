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

#include "../../include/io/loci.h"
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

/* ── lifecycle ───────────────────────────────────────────────── */

TEST(test_init_zeroes_state) {
    loci_t l; memset(&l, 0xFF, sizeof(l));
    ASSERT_TRUE(loci_init(&l));
    for (size_t i = 0; i < sizeof(l.regs); i++) ASSERT_EQ(l.regs[i], 0);
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
    loci_write(&l, 0x03A0, 0x42);
    ASSERT_EQ(loci_read(&l, 0x03A0), 0x42);
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
    /* Use an op with no implementation yet (MIA_BOOT in Sprint 34aa). */
    loci_write(&l, 0x03AF, LOCI_OP_MIA_BOOT);
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, LOCI_ENOSYS);
    ASSERT_TRUE((l.regs[LOCI_REG_BUSY] & 0x80) == 0);
    ASSERT_EQ(l.op_count[LOCI_OP_MIA_BOOT], 1);
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

TEST(test_op_pix_xreg_no_errno) {
    loci_t l; loci_init(&l);
    l.enabled = true;
    loci_write(&l, 0x03AF, LOCI_OP_PIX_XREG);
    /* PIX_XREG currently accepts everything silently. */
    uint16_t e = l.regs[LOCI_REG_API_ERRNO_LO] |
                 ((uint16_t)l.regs[LOCI_REG_API_ERRNO_HI] << 8);
    ASSERT_EQ(e, 0);
    ASSERT_EQ(l.regs[LOCI_REG_API_A], 0);
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
    loci_write(&l, 0x03AF, LOCI_OP_MOUNT);   /* not implemented until 34ab */
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
    ASSERT_EQ(e, LOCI_ENOENT);
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
     * To write "HELLO" the 6502 pushes O,L,L,E,H. */
    loci_write(&l, 0x03AC, 'O');
    loci_write(&l, 0x03AC, 'L');
    loci_write(&l, 0x03AC, 'L');
    loci_write(&l, 0x03AC, 'E');
    loci_write(&l, 0x03AC, 'H');
    /* Then push count uint16 (hi first, lo last → lo on top, LE in xstack). */
    push_u16(&l, 5);

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

    /* Push int32 offset=5, then uint8 whence=0 (SEEK_SET). */
    int32_t offset = 5;
    loci_write(&l, 0x03AC, 0);   /* whence */
    loci_write(&l, 0x03AC, (uint8_t)((offset >> 24) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)((offset >> 16) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)((offset >>  8) & 0xFF));
    loci_write(&l, 0x03AC, (uint8_t)(offset & 0xFF));

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
    RUN(test_op_pix_xreg_no_errno);
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
    RUN(test_unlink_removes_file);
    RUN(test_close_bad_fd_returns_ebadf);
    RUN(test_fd_exhaustion_emfile);
    RUN(test_reset_clears_state);

    printf("\n===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_run - tests_passed, tests_run);
    printf("===========================================================\n\n");
    return tests_passed == tests_run ? 0 : 1;
}
