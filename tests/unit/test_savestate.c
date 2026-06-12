/**
 * @file test_savestate.c
 * @brief Save state serialization/deserialization unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-02
 * @version 1.4.0-alpha
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "emulator.h"
#include "savestate.h"

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
        printf("FAIL\n    %s:%d: expected 0x%llX, got 0x%llX\n", __FILE__, __LINE__, \
               (unsigned long long)(b), (unsigned long long)(a)); \
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

static const char* TEST_FILE = "/tmp/oric1_test_savestate.ost";

/* Helper: initialize a minimal emulator for testing */
static void init_test_emu(emulator_t* emu) {
    memset(emu, 0, sizeof(emulator_t));
    memory_init(&emu->memory);
    cpu_init(&emu->cpu, &emu->memory);
    via_init(&emu->via);
    ay_init(&emu->psg, 1000000);
    video_init(&emu->video);
    oric_keyboard_init(&emu->keyboard);
    emu->tape_syncstack = -1;
    emu->has_microdisc = false;
}

static void cleanup_test(void) {
    unlink(TEST_FILE);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 1: CPU registers roundtrip                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_cpu) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    /* Set specific CPU state */
    emu1.cpu.A = 0x42;
    emu1.cpu.X = 0xAA;
    emu1.cpu.Y = 0xBB;
    emu1.cpu.SP = 0xF0;
    emu1.cpu.PC = 0xC000;
    emu1.cpu.P = 0x35;
    emu1.cpu.cycles = 123456789ULL;
    emu1.cpu.irq = IRQF_VIA | IRQF_DISK;

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    ASSERT_EQ(emu2.cpu.A, 0x42);
    ASSERT_EQ(emu2.cpu.X, 0xAA);
    ASSERT_EQ(emu2.cpu.Y, 0xBB);
    ASSERT_EQ(emu2.cpu.SP, 0xF0);
    ASSERT_EQ(emu2.cpu.PC, 0xC000);
    ASSERT_EQ(emu2.cpu.P, 0x35);
    ASSERT_EQ(emu2.cpu.cycles, 123456789ULL);
    ASSERT_EQ(emu2.cpu.irq, IRQF_VIA | IRQF_DISK);
    /* Verify memory pointer was restored */
    ASSERT_TRUE(emu2.cpu.memory == &emu2.memory);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 2: Memory (48KB RAM) roundtrip                               */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_memory) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    /* Write a pattern to RAM */
    for (int i = 0; i < RAM_SIZE; i++) {
        emu1.memory.ram[i] = (uint8_t)(i * 7 + 13);
    }
    /* Write pattern to upper_ram */
    for (int i = 0; i < ROM_SIZE; i++) {
        emu1.memory.upper_ram[i] = (uint8_t)(i * 3 + 5);
    }
    emu1.memory.rom_enabled = false;
    emu1.memory.overlay_active = true;
    emu1.memory.basic_rom_disabled = true;

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));

    /* Clear emu2 memory to verify it gets restored */
    memset(emu2.memory.ram, 0, RAM_SIZE);
    memset(emu2.memory.upper_ram, 0, ROM_SIZE);

    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    /* Verify RAM content */
    for (int i = 0; i < RAM_SIZE; i++) {
        ASSERT_EQ(emu2.memory.ram[i], (uint8_t)(i * 7 + 13));
    }
    for (int i = 0; i < ROM_SIZE; i++) {
        ASSERT_EQ(emu2.memory.upper_ram[i], (uint8_t)(i * 3 + 5));
    }
    ASSERT_FALSE(emu2.memory.rom_enabled);
    ASSERT_TRUE(emu2.memory.overlay_active);
    ASSERT_TRUE(emu2.memory.basic_rom_disabled);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 3: VIA registers + timers roundtrip                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_via) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    emu1.via.ora = 0x12;
    emu1.via.orb = 0x34;
    emu1.via.ddra = 0xFF;
    emu1.via.ddrb = 0x07;
    emu1.via.t1_counter = 0x1234;
    emu1.via.t1_latch = 0x5678;
    emu1.via.t2_counter = 0xABCD;
    emu1.via.t2_latch = 0x9A;
    emu1.via.t1_running = true;
    emu1.via.t2_running = false;
    emu1.via.acr = 0x60;
    emu1.via.pcr = 0xEE;
    emu1.via.ifr = 0x40;
    emu1.via.ier = 0xC0;
    emu1.via.cb1_pin = true;
    emu1.via.irq_line = true;

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    ASSERT_EQ(emu2.via.ora, 0x12);
    ASSERT_EQ(emu2.via.orb, 0x34);
    ASSERT_EQ(emu2.via.ddra, 0xFF);
    ASSERT_EQ(emu2.via.ddrb, 0x07);
    ASSERT_EQ(emu2.via.t1_counter, 0x1234);
    ASSERT_EQ(emu2.via.t1_latch, 0x5678);
    ASSERT_EQ(emu2.via.t2_counter, 0xABCD);
    ASSERT_EQ(emu2.via.t2_latch, 0x9A);
    ASSERT_TRUE(emu2.via.t1_running);
    ASSERT_FALSE(emu2.via.t2_running);
    ASSERT_EQ(emu2.via.acr, 0x60);
    ASSERT_EQ(emu2.via.pcr, 0xEE);
    ASSERT_EQ(emu2.via.ifr, 0x40);
    ASSERT_EQ(emu2.via.ier, 0xC0);
    ASSERT_TRUE(emu2.via.cb1_pin);
    ASSERT_TRUE(emu2.via.irq_line);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 4: PSG registers + tone/envelope roundtrip                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_psg) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    /* Set PSG registers */
    for (int i = 0; i < AY_NUM_REGISTERS; i++) {
        emu1.psg.registers[i] = (uint8_t)(i * 17);
    }
    emu1.psg.selected_reg = 7;
    emu1.psg.tone_period[0] = 0x100;
    emu1.psg.tone_period[1] = 0x200;
    emu1.psg.tone_period[2] = 0x300;
    emu1.psg.tone_counter[0] = 50;
    emu1.psg.tone_counter[1] = 100;
    emu1.psg.tone_counter[2] = 150;
    emu1.psg.tone_output[0] = 1;
    emu1.psg.tone_output[1] = 0;
    emu1.psg.tone_output[2] = 1;
    emu1.psg.noise_period = 0x1F;
    emu1.psg.noise_counter = 42;
    emu1.psg.noise_shift = 0x12345;
    emu1.psg.noise_output = 1;
    emu1.psg.env_period = 0xFFFF;
    emu1.psg.env_counter = 999;
    emu1.psg.env_shape = 14;
    emu1.psg.env_step = 20;
    emu1.psg.env_volume = 15;
    emu1.psg.env_holding = true;
    emu1.psg.clock_rate = 1000000;

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    for (int i = 0; i < AY_NUM_REGISTERS; i++) {
        ASSERT_EQ(emu2.psg.registers[i], (uint8_t)(i * 17));
    }
    ASSERT_EQ(emu2.psg.selected_reg, 7);
    ASSERT_EQ(emu2.psg.tone_period[0], 0x100);
    ASSERT_EQ(emu2.psg.tone_period[1], 0x200);
    ASSERT_EQ(emu2.psg.tone_period[2], 0x300);
    ASSERT_EQ(emu2.psg.tone_counter[0], 50u);
    ASSERT_EQ(emu2.psg.tone_counter[1], 100u);
    ASSERT_EQ(emu2.psg.tone_counter[2], 150u);
    ASSERT_EQ(emu2.psg.noise_shift, 0x12345u);
    ASSERT_EQ(emu2.psg.env_shape, 14);
    ASSERT_EQ(emu2.psg.env_step, 20);
    ASSERT_EQ(emu2.psg.env_volume, 15);
    ASSERT_TRUE(emu2.psg.env_holding);
    ASSERT_EQ(emu2.psg.clock_rate, 1000000u);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 5: Full emulator roundtrip                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_roundtrip) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    /* Set various state across all subsystems */
    emu1.cpu.A = 0x55;
    emu1.cpu.PC = 0xE696;
    emu1.cpu.cycles = 1000000ULL;
    emu1.memory.ram[0x0400] = 0xDE;
    emu1.memory.ram[0x0401] = 0xAD;
    emu1.via.ora = 0x77;
    emu1.via.t1_counter = 0x4321;
    emu1.psg.registers[0] = 0xAA;
    emu1.psg.tone_period[0] = 0x1FF;
    emu1.video.hires_mode = true;
    emu1.video.vid_mode = 0x06;
    emu1.keyboard.matrix[0] = 0xFE;
    emu1.keyboard.matrix[3] = 0x7F;
    emu1.tape_loaded = true;
    emu1.tapelen = 1024;
    emu1.tapeoffs = 512;
    emu1.tape_syncstack = 0xFD;

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    /* Verify all subsystems */
    ASSERT_EQ(emu2.cpu.A, 0x55);
    ASSERT_EQ(emu2.cpu.PC, 0xE696);
    ASSERT_EQ(emu2.cpu.cycles, 1000000ULL);
    ASSERT_EQ(emu2.memory.ram[0x0400], 0xDE);
    ASSERT_EQ(emu2.memory.ram[0x0401], 0xAD);
    ASSERT_EQ(emu2.via.ora, 0x77);
    ASSERT_EQ(emu2.via.t1_counter, 0x4321);
    ASSERT_EQ(emu2.psg.registers[0], 0xAA);
    ASSERT_EQ(emu2.psg.tone_period[0], 0x1FF);
    ASSERT_TRUE(emu2.video.hires_mode);
    ASSERT_EQ(emu2.video.vid_mode, 0x06);
    ASSERT_EQ(emu2.keyboard.matrix[0], 0xFE);
    ASSERT_EQ(emu2.keyboard.matrix[3], 0x7F);
    ASSERT_TRUE(emu2.tape_loaded);
    ASSERT_EQ(emu2.tapelen, 1024);
    ASSERT_EQ(emu2.tapeoffs, 512);
    ASSERT_EQ(emu2.tape_syncstack, 0xFD);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 6: File header validation                                    */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_file_header) {
    emulator_t emu1;
    init_test_emu(&emu1);

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));

    /* Read raw header */
    FILE* fp = fopen(TEST_FILE, "rb");
    ASSERT_TRUE(fp != NULL);

    char magic[4];
    fread(magic, 1, 4, fp);
    ASSERT_TRUE(memcmp(magic, "OST1", 4) == 0);

    uint8_t ver_buf[4];
    fread(ver_buf, 1, 4, fp);
    uint32_t version = (uint32_t)ver_buf[0] | ((uint32_t)ver_buf[1] << 8) |
                       ((uint32_t)ver_buf[2] << 16) | ((uint32_t)ver_buf[3] << 24);
    ASSERT_EQ(version, 1u);

    /* File size at offset 8 */
    uint8_t size_buf[4];
    fread(size_buf, 1, 4, fp);
    uint32_t file_size = (uint32_t)size_buf[0] | ((uint32_t)size_buf[1] << 8) |
                         ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);
    fseek(fp, 0, SEEK_END);
    long actual_size = ftell(fp);
    ASSERT_EQ(file_size, (uint32_t)actual_size);

    /* CRC32 at offset 12 should be non-zero */
    fseek(fp, 12, SEEK_SET);
    uint8_t crc_buf[4];
    fread(crc_buf, 1, 4, fp);
    uint32_t crc = (uint32_t)crc_buf[0] | ((uint32_t)crc_buf[1] << 8) |
                   ((uint32_t)crc_buf[2] << 16) | ((uint32_t)crc_buf[3] << 24);
    ASSERT_TRUE(crc != 0);

    /* Emulator version at offset 16 */
    fseek(fp, 16, SEEK_SET);
    char emu_ver[32];
    fread(emu_ver, 1, 32, fp);
    ASSERT_TRUE(strstr(emu_ver, "alpha") != NULL);

    fclose(fp);
    memory_cleanup(&emu1.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 7: Reject invalid file                                       */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_load_invalid_file) {
    emulator_t emu;
    init_test_emu(&emu);

    /* Non-existent file */
    ASSERT_FALSE(savestate_load(&emu, "/tmp/nonexistent_oric1.ost"));

    /* Create file with bad magic */
    FILE* fp = fopen(TEST_FILE, "wb");
    ASSERT_TRUE(fp != NULL);
    fwrite("BAAD", 1, 4, fp);
    uint8_t zeros[44] = {0};
    fwrite(zeros, 1, 44, fp);
    fclose(fp);

    ASSERT_FALSE(savestate_load(&emu, TEST_FILE));

    /* Create file too small */
    fp = fopen(TEST_FILE, "wb");
    fwrite("OST1", 1, 4, fp);
    fclose(fp);
    ASSERT_FALSE(savestate_load(&emu, TEST_FILE));

    memory_cleanup(&emu.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 8: Save/load with Microdisc state                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_with_microdisc) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    /* Enable Microdisc */
    emu1.has_microdisc = true;
    microdisc_init(&emu1.microdisc);
    emu1.microdisc.status = 0x55;
    emu1.microdisc.intrq = 0x80;
    emu1.microdisc.drq = 0x00;
    emu1.microdisc.diskrom = true;
    emu1.microdisc.romdis = true;
    emu1.microdisc.intena = true;
    emu1.microdisc.drive = 2;
    emu1.microdisc.side = 1;
    emu1.microdisc.fdc.track = 5;
    emu1.microdisc.fdc.sector = 3;
    emu1.microdisc.fdc.status = 0x01;
    emu1.microdisc.fdc.c_track = 5;
    emu1.microdisc.fdc.delayed_drq = 100;
    emu1.microdisc.fdc.delayed_int = 200;

    /* emu2 also needs microdisc flag for loading */
    emu2.has_microdisc = true;
    microdisc_init(&emu2.microdisc);

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));

    ASSERT_EQ(emu2.microdisc.status, 0x55);
    ASSERT_EQ(emu2.microdisc.intrq, 0x80);
    ASSERT_EQ(emu2.microdisc.drq, 0x00);
    ASSERT_TRUE(emu2.microdisc.diskrom);
    ASSERT_TRUE(emu2.microdisc.romdis);
    ASSERT_TRUE(emu2.microdisc.intena);
    ASSERT_EQ(emu2.microdisc.drive, 2);
    ASSERT_EQ(emu2.microdisc.side, 1);
    ASSERT_EQ(emu2.microdisc.fdc.track, 5);
    ASSERT_EQ(emu2.microdisc.fdc.sector, 3);
    ASSERT_EQ(emu2.microdisc.fdc.status, 0x01);
    ASSERT_EQ(emu2.microdisc.fdc.c_track, 5);
    ASSERT_EQ(emu2.microdisc.fdc.delayed_drq, 100);
    ASSERT_EQ(emu2.microdisc.fdc.delayed_int, 200);
    ASSERT_EQ(emu2.microdisc.fdc.side, 1);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TEST 9: ULA profile (OCULA) roundtrip in VID section               */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_save_load_ula_profile) {
    emulator_t emu1, emu2;
    init_test_emu(&emu1);
    init_test_emu(&emu2);

    video_set_profile(&emu1.video, ULA_PROFILE_OCULA);

    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));
    ASSERT_EQ(video_get_profile(&emu2.video), ULA_PROFILE_OCULA);

    /* And the stock profile roundtrips too */
    video_set_profile(&emu1.video, ULA_PROFILE_HCS10017);
    ASSERT_TRUE(savestate_save(&emu1, TEST_FILE));
    ASSERT_TRUE(savestate_load(&emu2, TEST_FILE));
    ASSERT_EQ(video_get_profile(&emu2.video), ULA_PROFILE_HCS10017);

    memory_cleanup(&emu1.memory);
    memory_cleanup(&emu2.memory);
    cleanup_test();
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Save State Tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    RUN(test_save_load_cpu);
    RUN(test_save_load_memory);
    RUN(test_save_load_via);
    RUN(test_save_load_psg);
    RUN(test_save_load_roundtrip);
    RUN(test_save_file_header);
    RUN(test_load_invalid_file);
    RUN(test_save_load_with_microdisc);
    RUN(test_save_load_ula_profile);

    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
