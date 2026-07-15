/**
 * @file test_video.c
 * @brief Video export & rendering tests - PPM, BMP, ASCII export
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-beta.2
 *
 * Tests video rendering + export module.
 * Validates PPM/BMP file format, ASCII output, and ROM boot screenshot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "video/video.h"
#include "video/export.h"
#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include "io/via6522.h"

#define ROM_PATH "/home/bmarty/oricutron/roms/basic10.rom"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-55s", #name); \
    int _before = tests_failed; \
    name(); \
    if (tests_failed == _before) { \
        tests_passed++; \
        printf("PASS\n"); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, (long)(b), (long)(a)); \
        tests_failed++; return; \
    } \
} while(0)

static long file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* Create a fake charset for testing: simple diagonal pattern */
static void make_test_charset(uint8_t* charset) {
    memset(charset, 0, 2048);
    /* Set character 'A' (0x41) to a visible pattern */
    for (int row = 0; row < 8; row++) {
        charset[0x41 * 8 + row] = (uint8_t)(0x3E >> row);
    }
    /* Set space (0x20) to all zeros - already done by memset */
}

/* ═══════════════════════════════════════════════════════════════ */
/*  TEXT MODE RENDERING + PPM EXPORT                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_text_render_ppm_export) {
    video_t vid;
    video_init(&vid);

    uint8_t charset[2048];
    make_test_charset(charset);
    vid.charset = charset;

    /* Create a simple memory image with text at $BB80 */
    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);

    /* Fill screen with 'A' characters */
    for (int i = 0; i < 40 * 28; i++) {
        mem[0xBB80 + i] = 'A';
    }

    video_render_frame(&vid, mem);

    /* Export PPM */
    const char* path = "/tmp/test_video_text.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));

    /* PPM P6: header + 240*224*3 bytes = header + 161280 */
    long sz = file_size(path);
    ASSERT_TRUE(sz > 161280); /* Header adds some bytes */

    /* Verify PPM header */
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char header[32];
    char* r = fgets(header, sizeof(header), fp);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(strncmp(header, "P6", 2) == 0);
    fclose(fp);

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* Sprint 77: the bordered export composites the OCULA overscan border, so the
 * image is larger than the active area (and than the plain export). */
TEST(test_export_bordered_dimensions) {
    video_t vid;
    video_init(&vid);

    uint8_t charset[2048];
    make_test_charset(charset);
    vid.charset = charset;

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    for (int i = 0; i < 40 * 28; i++) mem[0xBB80 + i] = 'A';
    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_video_bordered.ppm";
    ASSERT_TRUE(video_export_auto_bordered(&vid, path));

    /* Header carries the bordered geometry: native + 2*border per axis. */
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char magic[8];
    int w = 0, h = 0;
    int got = fscanf(fp, "%2s %d %d", magic, &w, &h);
    fclose(fp);
    ASSERT_EQ(got, 3);
    ASSERT_TRUE(strncmp(magic, "P6", 2) == 0);
    ASSERT_EQ(w, vid.native_w + 2 * OCULA_BORDER_W);
    ASSERT_EQ(h, vid.native_h + 2 * OCULA_BORDER_H);

    /* Bordered payload is strictly larger than the active-area export. */
    long sz = file_size(path);
    ASSERT_TRUE(sz > (long)(vid.native_w * vid.native_h * 3));

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

TEST(test_text_render_framebuffer_not_black) {
    video_t vid;
    video_init(&vid);

    uint8_t charset[2048];
    make_test_charset(charset);
    vid.charset = charset;

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);

    /* Put some visible characters */
    for (int i = 0; i < 40; i++) {
        mem[0xBB80 + i] = 'A';
    }

    video_render_frame(&vid, mem);

    /* Check framebuffer is not all black */
    int nonzero = 0;
    for (int i = 0; i < ORIC_SCREEN_W * ORIC_SCREEN_H * 3; i++) {
        if (vid.framebuffer[i] != 0) nonzero++;
    }
    ASSERT_TRUE(nonzero > 0);

    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  HIRES MODE RENDERING + PPM EXPORT                             */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_hires_render_ppm_export) {
    video_t vid;
    video_init(&vid);
    video_set_mode(&vid, true);

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);

    /* Fill HIRES area with a pattern */
    for (int y = 0; y < 200; y++) {
        for (int col = 0; col < 40; col++) {
            mem[0xA000 + y * 40 + col] = 0x55 | 0x40; /* alternating pixels, bit 6 set */
        }
    }

    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_video_hires.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));

    long sz = file_size(path);
    ASSERT_TRUE(sz > 161280);

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* HIRES inverse bit (bit 7) complements ink/paper (XOR 7); it does NOT
 * swap them. Regression test for the AIC-image bug (forum Defence Force
 * p35017): blue ink on black paper with inverse must render yellow-on-white,
 * matching the real ULA / Oricutron, not black-on-blue. */
TEST(test_hires_inverse_complement) {
    video_t vid;
    video_init(&vid);
    video_set_mode(&vid, true);
    vid.vid_mode = 0x06;   /* HIRES (bit 2), so lines 0-199 read from $A000 */

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);

    /* Column 0 = INK attribute 4 (blue). Column 1 = inverse HIRES byte with
     * all 6 pixels set ($BF = 0x80 inverse | 0x3F pixels). Paper stays black
     * (0) from the scanline reset. */
    mem[0xA000 + 0] = 0x04;          /* serial attr: INK = blue (4) */
    mem[0xA000 + 1] = 0x80 | 0x3F;   /* inverse, all pixels ON */

    video_render_frame(&vid, mem);

    /* Pixel at x=6 (column 1, first dot): pattern bit set -> foreground.
     * Inverse foreground = ink^7 = 4^7 = 3 = yellow (255,255,0). */
    int w = vid.native_w;
    const uint8_t* p = &vid.framebuffer[(0 * w + 6) * 3];
    ASSERT_EQ(p[0], 255); ASSERT_EQ(p[1], 255); ASSERT_EQ(p[2], 0);

    /* Now clear the pixels: inverse background = paper^7 = 0^7 = 7 = white. */
    mem[0xA000 + 1] = 0x80 | 0x00;   /* inverse, all pixels OFF */
    video_render_frame(&vid, mem);
    p = &vid.framebuffer[(0 * w + 6) * 3];
    ASSERT_EQ(p[0], 255); ASSERT_EQ(p[1], 255); ASSERT_EQ(p[2], 255);

    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  BMP EXPORT                                                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_bmp_export_valid_header) {
    video_t vid;
    video_init(&vid);

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_video.bmp";
    ASSERT_TRUE(video_export_bmp(&vid, path));

    /* Verify BMP header */
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);

    uint8_t bfh[14];
    size_t rd = fread(bfh, 1, 14, fp);
    ASSERT_EQ((long)rd, 14L);
    ASSERT_EQ(bfh[0], 'B');
    ASSERT_EQ(bfh[1], 'M');

    /* Verify BITMAPINFOHEADER */
    uint8_t bih[40];
    rd = fread(bih, 1, 40, fp);
    ASSERT_EQ((long)rd, 40L);
    ASSERT_EQ(bih[0], 40); /* header size */
    int w = bih[4] | (bih[5] << 8);
    int h = bih[8] | (bih[9] << 8);
    ASSERT_EQ(w, ORIC_SCREEN_W);
    ASSERT_EQ(h, ORIC_SCREEN_H);
    ASSERT_EQ(bih[14], 24); /* bits per pixel */

    fclose(fp);

    /* Verify file size is reasonable */
    long sz = file_size(path);
    ASSERT_TRUE(sz > 54); /* At least header */

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  ASCII EXPORT                                                   */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_ascii_export_nonempty) {
    video_t vid;
    video_init(&vid);

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);

    /* Put white pixels so we get non-black output */
    for (int i = 0; i < 40; i++) {
        mem[0xBB80 + i] = 'A';
    }

    uint8_t charset[2048];
    make_test_charset(charset);
    vid.charset = charset;

    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_video_ascii.txt";
    FILE* fp = fopen(path, "w");
    ASSERT_TRUE(fp != NULL);

    ASSERT_TRUE(video_export_ascii(&vid, fp, 2, 2));
    fclose(fp);

    /* Verify file is not empty */
    long sz = file_size(path);
    ASSERT_TRUE(sz > 100); /* Should have many ANSI escape codes */

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  AUTO EXPORT (extension detection)                              */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_auto_export_ppm) {
    video_t vid;
    video_init(&vid);
    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_auto.ppm";
    ASSERT_TRUE(video_export_auto(&vid, path));
    long sz = file_size(path);
    ASSERT_TRUE(sz > 161280);
    unlink(path);

    free(mem);
    video_cleanup(&vid);
}

TEST(test_auto_export_bmp) {
    video_t vid;
    video_init(&vid);
    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_auto.bmp";
    ASSERT_TRUE(video_export_auto(&vid, path));
    long sz = file_size(path);
    ASSERT_TRUE(sz > 54);

    /* Verify it's actually BMP */
    FILE* fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char sig[2];
    size_t rd = fread(sig, 1, 2, fp);
    fclose(fp);
    ASSERT_EQ((long)rd, 2L);
    ASSERT_EQ(sig[0], 'B');
    ASSERT_EQ(sig[1], 'M');

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  PPM FILE SIZE VERIFICATION                                     */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_ppm_exact_pixel_data) {
    video_t vid;
    video_init(&vid);
    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    video_render_frame(&vid, mem);

    const char* path = "/tmp/test_ppm_size.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));

    /* Read and verify: header "P6\n240 224\n255\n" + 240*224*3 = 161280 bytes */
    long sz = file_size(path);
    /* Header: "P6\n" (3) + "240 224\n" (8) + "255\n" (4) = 15 bytes */
    long expected = 15 + 240 * 224 * 3;
    ASSERT_EQ(sz, expected);

    unlink(path);
    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  ROM BOOT SCREENSHOT                                            */
/* ═══════════════════════════════════════════════════════════════ */

/* I/O callbacks for ROM boot test */
typedef struct {
    cpu6502_t cpu;
    memory_t memory;
    via6522_t via;
} video_test_system_t;

static uint8_t vt_io_read(uint16_t addr, void* ud) {
    video_test_system_t* sys = (video_test_system_t*)ud;
    return via_read(&sys->via, (uint8_t)(addr & 0x0F));
}

static void vt_io_write(uint16_t addr, uint8_t val, void* ud) {
    video_test_system_t* sys = (video_test_system_t*)ud;
    via_write(&sys->via, (uint8_t)(addr & 0x0F), val);
}

static void vt_irq_cb(bool state, void* ud) {
    video_test_system_t* sys = (video_test_system_t*)ud;
    if (state) cpu_irq_set(&sys->cpu, IRQF_VIA);
    else cpu_irq_clear(&sys->cpu, IRQF_VIA);
}

TEST(test_rom_boot_screenshot) {
    /* This test requires the real ROM */
    FILE* check = fopen(ROM_PATH, "rb");
    if (!check) {
        printf("SKIP (ROM not found)\n");
        tests_passed++;
        return;
    }
    fclose(check);

    video_test_system_t sys;
    memset(&sys, 0, sizeof(sys));

    memory_init(&sys.memory);
    cpu_init(&sys.cpu, &sys.memory);
    via_init(&sys.via);
    via_reset(&sys.via);

    memory_set_io_callbacks(&sys.memory, vt_io_read, vt_io_write, &sys);
    via_set_irq_callback(&sys.via, vt_irq_cb, &sys);

    ASSERT_TRUE(memory_load_rom(&sys.memory, ROM_PATH, 0));

    /* Set up video with charset from ROM */
    video_t vid;
    video_init(&vid);
    vid.charset = sys.memory.charset;

    /* Boot and run 5M cycles */
    cpu_reset(&sys.cpu);
    int total = 0;
    while (total < 5000000 && !sys.cpu.halted) {
        int step = cpu_step(&sys.cpu);
        via_update(&sys.via, step);
        total += step;
    }

    /* Render and export */
    video_render_frame(&vid, sys.memory.ram);

    const char* path = "/tmp/test_rom_boot.ppm";
    ASSERT_TRUE(video_export_ppm(&vid, path));

    /* Verify file exists and is not empty */
    long sz = file_size(path);
    ASSERT_TRUE(sz > 161280);

    /* Check that framebuffer is not all black (ROM should have written to screen) */
    int nonzero = 0;
    for (int i = 0; i < ORIC_SCREEN_W * ORIC_SCREEN_H * 3; i++) {
        if (vid.framebuffer[i] != 0) nonzero++;
    }
    ASSERT_TRUE(nonzero > 100); /* At least some non-black pixels */

    unlink(path);
    video_cleanup(&vid);
    memory_cleanup(&sys.memory);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  NULL/EDGE CASE TESTS                                           */
/* ═══════════════════════════════════════════════════════════════ */

TEST(test_export_null_params) {
    video_t vid;
    video_init(&vid);

    /* NULL filename should fail gracefully */
    ASSERT_TRUE(!video_export_ppm(&vid, NULL));
    ASSERT_TRUE(!video_export_bmp(&vid, NULL));
    ASSERT_TRUE(!video_export_ppm(NULL, "/tmp/test.ppm"));
    ASSERT_TRUE(!video_export_bmp(NULL, "/tmp/test.bmp"));
    ASSERT_TRUE(!video_export_ascii(NULL, stdout, 2, 2));
    ASSERT_TRUE(!video_export_ascii(&vid, NULL, 2, 2));

    video_cleanup(&vid);
}

TEST(test_video_init_cleanup) {
    video_t vid;
    ASSERT_TRUE(video_init(&vid));
    ASSERT_TRUE(!vid.hires_mode);
    ASSERT_TRUE(vid.need_refresh);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  DOUBLE-HEIGHT ROW PARITY (Oricutron/real ULA model)            */
/* ═══════════════════════════════════════════════════════════════ */

/* Is the pixel at (x,y) lit (white-ish foreground) ? */
static int dh_pixel_on(const video_t* v, int x, int y) {
    return v->framebuffer[(y * ORIC_SCREEN_W + x) * 3] > 100;
}

/* Lay a double-height glyph 'A' on text rows r0..r1 (attr 0x0A at col 0,
 * glyph at cols 1+). */
static void dh_put(uint8_t* mem, int r0, int r1) {
    for (int r = r0; r <= r1; r++) {
        mem[0xBB80 + r * 40 + 0] = 0x0A;                 /* double-height attr */
        for (int c = 1; c < 40; c++) mem[0xBB80 + r * 40 + c] = 'A';
    }
}

/* Real Oric ULA renders double height by absolute char-row parity (y>>1)&7:
 * even rows show the top half of the glyph, odd rows the bottom half. A glyph
 * placed on an odd row therefore shows its BOTTOM half first (the well-known
 * "must align double height to an even row" quirk). */
TEST(test_double_height_row_parity) {
    video_t vid;
    video_init(&vid);

    /* Glyph 'A': top half (rows 0-3) solid, bottom half (rows 4-7) blank. */
    uint8_t charset[2048];
    memset(charset, 0, sizeof(charset));
    for (int r = 0; r < 4; r++) charset['A' * 8 + r] = 0x3F;
    vid.charset = charset;

    uint8_t* mem = (uint8_t*)calloc(49152, 1);
    ASSERT_TRUE(mem != NULL);
    const int x = 8;                /* inside column 1 */
    #define MIDY(row) ((row) * 8 + 3)

    /* Sanity: a NON-double 'A' on row 0 uses real glyph rows (chline=y&7):
     * y=0 -> glyph row 0 (solid), y=4 -> glyph row 4 (blank). */
    memset(mem + 0xBB80, ' ', 40 * 28);
    for (int c = 0; c < 40; c++) mem[0xBB80 + c] = 'A';
    video_render_frame(&vid, mem);
    ASSERT_TRUE(dh_pixel_on(&vid, x, 0));      /* glyph row 0 solid */
    ASSERT_TRUE(!dh_pixel_on(&vid, x, 4));     /* glyph row 4 blank */

    /* EVEN-aligned (rows 0,1): top half then bottom half. */
    memset(mem + 0xBB80, ' ', 40 * 28);
    dh_put(mem, 0, 1);
    video_render_frame(&vid, mem);
    ASSERT_TRUE(dh_pixel_on(&vid, x, MIDY(0)));    /* row 0 = top (solid) */
    ASSERT_TRUE(!dh_pixel_on(&vid, x, MIDY(1)));   /* row 1 = bottom (blank) */

    /* ODD-aligned (rows 1,2): parity quirk — row 1 shows BOTTOM half. */
    memset(mem + 0xBB80, ' ', 40 * 28);
    dh_put(mem, 1, 2);
    video_render_frame(&vid, mem);
    ASSERT_TRUE(!dh_pixel_on(&vid, x, MIDY(1)));   /* row 1 = bottom (blank) */
    ASSERT_TRUE(dh_pixel_on(&vid, x, MIDY(2)));    /* row 2 = top (solid) */

    #undef MIDY
    free(mem);
    video_cleanup(&vid);
}

/* ═══════════════════════════════════════════════════════════════ */
/*  MAIN                                                           */
/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("===========================================================\n");
    printf("  ORIC-1 Video Export Tests\n");
    printf("===========================================================\n\n");

    RUN(test_video_init_cleanup);
    RUN(test_text_render_ppm_export);
    RUN(test_export_bordered_dimensions);
    RUN(test_text_render_framebuffer_not_black);
    RUN(test_hires_render_ppm_export);
    RUN(test_hires_inverse_complement);
    RUN(test_bmp_export_valid_header);
    RUN(test_ascii_export_nonempty);
    RUN(test_auto_export_ppm);
    RUN(test_auto_export_bmp);
    RUN(test_ppm_exact_pixel_data);
    RUN(test_export_null_params);
    RUN(test_rom_boot_screenshot);
    RUN(test_double_height_row_parity);

    printf("\n");
    printf("===========================================================\n");
    printf("  Results: %d passed, %d failed (total: %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("===========================================================\n\n");

    return tests_failed > 0 ? 1 : 0;
}
