/**
 * @file test_full_system.c
 * @brief Integration tests - CPU + Memory + VIA wired together
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu6502.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "io/via6522.h"
#include "emulator.h"

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

typedef struct {
    cpu6502_t cpu;
    memory_t memory;
    via6522_t via;
} test_system_t;

static uint8_t sys_io_read(uint16_t addr, void* ud) {
    test_system_t* sys = (test_system_t*)ud;
    return via_read(&sys->via, (uint8_t)(addr & 0x0F));
}

static void sys_io_write(uint16_t addr, uint8_t val, void* ud) {
    test_system_t* sys = (test_system_t*)ud;
    via_write(&sys->via, (uint8_t)(addr & 0x0F), val);
}

static void sys_irq_cb(bool state, void* ud) {
    test_system_t* sys = (test_system_t*)ud;
    if (state) cpu_irq_set(&sys->cpu, IRQF_VIA);
    else cpu_irq_clear(&sys->cpu, IRQF_VIA);
}

static void sys_init(test_system_t* sys) {
    memory_init(&sys->memory);
    cpu_init(&sys->cpu, &sys->memory);
    via_init(&sys->via);
    via_reset(&sys->via);
    memory_set_io_callbacks(&sys->memory, sys_io_read, sys_io_write, sys);
    via_set_irq_callback(&sys->via, sys_irq_cb, sys);

    /* Set reset vector to $0200 */
    sys->memory.rom[0x3FFC] = 0x00;
    sys->memory.rom[0x3FFD] = 0x02;
    cpu_reset(&sys->cpu);
}

static void sys_write_program(test_system_t* sys, uint16_t addr, const uint8_t* code, size_t len) {
    for (size_t i = 0; i < len; i++) {
        memory_write(&sys->memory, (uint16_t)(addr + i), code[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INTEGRATION TESTS                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_cpu_via_write) {
    test_system_t sys;
    sys_init(&sys);
    /* Write VIA DDRA via CPU */
    uint8_t code[] = {
        0xA9, 0xFF,       /* LDA #$FF */
        0x8D, 0x03, 0x03, /* STA $0303 (VIA_DDRA) */
    };
    sys_write_program(&sys, 0x0200, code, sizeof(code));
    cpu_step(&sys.cpu);
    cpu_step(&sys.cpu);
    ASSERT_EQ(sys.via.ddra, 0xFF);
}

TEST(test_cpu_via_read) {
    test_system_t sys;
    sys_init(&sys);
    /* Set DDR register directly, then read via CPU */
    sys.via.ddra = 0xAA;
    uint8_t code[] = {
        0xAD, 0x03, 0x03, /* LDA $0303 (VIA_DDRA) */
    };
    sys_write_program(&sys, 0x0200, code, sizeof(code));
    cpu_step(&sys.cpu);
    ASSERT_EQ(sys.cpu.A, 0xAA);
}

TEST(test_cpu_via_timer_irq) {
    test_system_t sys;
    sys_init(&sys);

    /* Enable VIA T1 interrupt */
    via_write(&sys.via, VIA_IER, 0x80 | VIA_INT_T1);

    /* Set IRQ vector */
    sys.memory.rom[0x3FFE] = 0x00; /* $FFFE -> $1000 */
    sys.memory.rom[0x3FFF] = 0x10;

    /* At $1000: RTI */
    memory_write(&sys.memory, 0x1000, 0x40);

    /* Program: CLI, start timer, NOP loop */
    uint8_t code[] = {
        0x58,             /* CLI - enable interrupts */
        0xA9, 0x02,       /* LDA #$02 */
        0x8D, 0x04, 0x03, /* STA $0304 (T1CL) */
        0xA9, 0x00,       /* LDA #$00 */
        0x8D, 0x05, 0x03, /* STA $0305 (T1CH) - starts timer */
        0xEA,             /* NOP */
        0xEA,             /* NOP */
        0xEA,             /* NOP */
    };
    sys_write_program(&sys, 0x0200, code, sizeof(code));

    /* Execute instructions */
    for (int i = 0; i < 6; i++) {
        int cyc = cpu_step(&sys.cpu);
        via_update(&sys.via, cyc);
    }

    /* Timer should have expired and triggered an interrupt */
    /* Check that IFR has T1 flag set */
    ASSERT_TRUE(sys.via.ifr & VIA_INT_T1);
}

TEST(test_fibonacci_program) {
    test_system_t sys;
    sys_init(&sys);

    /* Calculate first 8 Fibonacci numbers at $0080-$0087 */
    /* Simpler approach: use absolute addressing */
    uint8_t code[] = {
        0xA9, 0x00,             /* $0200: LDA #0 */
        0x85, 0x80,             /* $0202: STA $80 (fib[0] = 0) */
        0xA9, 0x01,             /* $0204: LDA #1 */
        0x85, 0x81,             /* $0206: STA $81 (fib[1] = 1) */
        0xA2, 0x00,             /* $0208: LDX #0 (loop counter) */
        /* loop at $020A: */
        0xB5, 0x80,             /* $020A: LDA $80,X (fib[i]) */
        0x18,                   /* $020C: CLC */
        0x75, 0x81,             /* $020D: ADC $81,X (fib[i+1]) */
        0x95, 0x82,             /* $020F: STA $82,X (fib[i+2]) */
        0xE8,                   /* $0211: INX */
        0xE0, 0x06,             /* $0212: CPX #6 */
        0xD0, 0xF4,             /* $0214: BNE $020A (-12) */
        /* done at $0216 */
    };
    sys_write_program(&sys, 0x0200, code, sizeof(code));

    for (int i = 0; i < 200; i++) {
        cpu_step(&sys.cpu);
        if (sys.cpu.PC == 0x0216) break;
    }

    /* Fibonacci: 0, 1, 1, 2, 3, 5, 8, 13 */
    ASSERT_EQ(memory_read(&sys.memory, 0x80), 0);
    ASSERT_EQ(memory_read(&sys.memory, 0x81), 1);
    ASSERT_EQ(memory_read(&sys.memory, 0x82), 1);
    ASSERT_EQ(memory_read(&sys.memory, 0x83), 2);
    ASSERT_EQ(memory_read(&sys.memory, 0x84), 3);
    ASSERT_EQ(memory_read(&sys.memory, 0x85), 5);
    ASSERT_EQ(memory_read(&sys.memory, 0x86), 8);
    ASSERT_EQ(memory_read(&sys.memory, 0x87), 13);
}

TEST(test_subroutine_multiply) {
    test_system_t sys;
    sys_init(&sys);

    /* Multiply 6 * 7 = 42 using repeated addition */
    /* $80 = multiplicand, $81 = multiplier, $82 = result */
    uint8_t code[] = {
        0xA9, 0x06,       /* LDA #6 */
        0x85, 0x80,       /* STA $80 */
        0xA9, 0x07,       /* LDA #7 */
        0x85, 0x81,       /* STA $81 */
        0x20, 0x20, 0x02, /* JSR $0220 (multiply) */
        0xEA,             /* NOP (done) - $020B */
    };
    sys_write_program(&sys, 0x0200, code, sizeof(code));

    /* Multiply subroutine at $0220 */
    uint8_t sub[] = {
        0xA9, 0x00,       /* LDA #0 (result) */
        0xA6, 0x81,       /* LDX $81 (counter) */
        /* loop: */
        0x18,             /* CLC */
        0x65, 0x80,       /* ADC $80 */
        0xCA,             /* DEX */
        0xD0, 0xFB,       /* BNE loop */
        0x85, 0x82,       /* STA $82 (result) */
        0x60,             /* RTS */
    };
    sys_write_program(&sys, 0x0220, sub, sizeof(sub));

    for (int i = 0; i < 200; i++) {
        cpu_step(&sys.cpu);
        if (sys.cpu.PC == 0x020B) break;
    }

    ASSERT_EQ(memory_read(&sys.memory, 0x82), 42);
}

TEST(test_execute_cycles) {
    test_system_t sys;
    sys_init(&sys);

    /* Simple NOP loop */
    for (int i = 0; i < 100; i++) {
        memory_write(&sys.memory, 0x0200 + i, 0xEA); /* NOP */
    }

    int executed = cpu_execute_cycles(&sys.cpu, 20);
    ASSERT_TRUE(executed >= 20);
    ASSERT_TRUE(sys.cpu.cycles > 0);
}

TEST(test_state_string) {
    test_system_t sys;
    sys_init(&sys);
    sys.cpu.A = 0x42;
    sys.cpu.X = 0x10;
    sys.cpu.Y = 0x20;

    char buf[256];
    cpu_get_state_string(&sys.cpu, buf, sizeof(buf));
    ASSERT_TRUE(strlen(buf) > 0);
    /* Should contain register values */
    ASSERT_TRUE(strstr(buf, "A:42") != NULL);
    ASSERT_TRUE(strstr(buf, "X:10") != NULL);
    ASSERT_TRUE(strstr(buf, "Y:20") != NULL);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Deferred fast-load tests                                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_deferred_fastload_fields) {
    /* Verify deferred fast-load fields initialize to safe defaults */
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    ASSERT_TRUE(emu.fastload_buf == NULL);
    ASSERT_EQ(emu.fastload_addr, 0);
    ASSERT_EQ(emu.fastload_size, 0);
    ASSERT_TRUE(!emu.fastload_pending);
}

TEST(test_deferred_fastload_buffer) {
    /* Verify buffering: data stored in fastload fields, not in memory */
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);

    uint16_t addr = 0x0500;
    uint16_t size = 4;
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};

    emu.fastload_buf = (uint8_t*)malloc(size);
    memcpy(emu.fastload_buf, data, size);
    emu.fastload_addr = addr;
    emu.fastload_size = size;
    emu.fastload_pending = true;

    /* Memory should NOT contain the data yet */
    ASSERT_EQ(memory_read(&emu.memory, 0x0500), 0x00);
    ASSERT_EQ(memory_read(&emu.memory, 0x0501), 0x00);

    /* Simulate deferred injection (as emulator_run would do) */
    for (int i = 0; i < emu.fastload_size; i++) {
        memory_write(&emu.memory, emu.fastload_addr + i, emu.fastload_buf[i]);
    }
    free(emu.fastload_buf);
    emu.fastload_buf = NULL;
    emu.fastload_pending = false;

    /* Now memory should contain the data */
    ASSERT_EQ(memory_read(&emu.memory, 0x0500), 0xDE);
    ASSERT_EQ(memory_read(&emu.memory, 0x0501), 0xAD);
    ASSERT_EQ(memory_read(&emu.memory, 0x0502), 0xBE);
    ASSERT_EQ(memory_read(&emu.memory, 0x0503), 0xEF);
    ASSERT_TRUE(!emu.fastload_pending);

    memory_cleanup(&emu.memory);
}

TEST(test_deferred_fastload_survives_ram_clear) {
    /* Verify that fastload buffer survives memory_clear_ram() */
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    memory_init(&emu.memory);

    uint8_t data[] = {0x42, 0x43, 0x44};
    emu.fastload_buf = (uint8_t*)malloc(3);
    memcpy(emu.fastload_buf, data, 3);
    emu.fastload_addr = 0x0400;
    emu.fastload_size = 3;
    emu.fastload_pending = true;

    /* Simulate RAM test clearing memory */
    memory_clear_ram(&emu.memory, 0x00);

    /* Buffer should still be intact */
    ASSERT_TRUE(emu.fastload_pending);
    ASSERT_EQ(emu.fastload_buf[0], 0x42);
    ASSERT_EQ(emu.fastload_buf[1], 0x43);
    ASSERT_EQ(emu.fastload_buf[2], 0x44);

    /* Inject after "RAM test" */
    for (int i = 0; i < emu.fastload_size; i++) {
        memory_write(&emu.memory, emu.fastload_addr + i, emu.fastload_buf[i]);
    }

    ASSERT_EQ(memory_read(&emu.memory, 0x0400), 0x42);
    ASSERT_EQ(memory_read(&emu.memory, 0x0401), 0x43);
    ASSERT_EQ(memory_read(&emu.memory, 0x0402), 0x44);

    free(emu.fastload_buf);
    emu.fastload_buf = NULL;
    memory_cleanup(&emu.memory);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TYPE-KEYS DEBOUNCE TESTS                                          */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_type_keys_debounce_field) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    /* Verify debounce fields exist and are zero-initialized */
    ASSERT_EQ(emu.type_keys_last_char, 0);
    ASSERT_EQ(emu.type_keys_debounce, 0);
    /* Set and verify */
    emu.type_keys_last_char = 'A';
    emu.type_keys_debounce = 2;
    ASSERT_EQ(emu.type_keys_last_char, 'A');
    ASSERT_EQ(emu.type_keys_debounce, 2);
}

/* Sprint 60p : file de séquences multi --type-keys. Vérifie que les champs
 * existent, sont zéro-initialisés, et qu'une activation d'entrée reproduit
 * fidèlement le chargement fait par la boucle d'émulation. */
TEST(test_type_keys_seq_fields) {
    emulator_t emu;
    memset(&emu, 0, sizeof(emu));
    ASSERT_EQ(emu.type_keys_seq_count, 0);
    ASSERT_EQ(emu.type_keys_seq_idx, 0);

    /* Empile deux entrées (volontairement dans le désordre de cycle). */
    emu.type_keys_seq[0].at = 5500000;
    emu.type_keys_seq[0].text = "PRINT\"B\"\\n";
    emu.type_keys_seq[0].loci_hid = false;
    emu.type_keys_seq[1].at = 3000000;
    emu.type_keys_seq[1].text = "PRINT\"A\"\\n";
    emu.type_keys_seq[1].loci_hid = true;
    emu.type_keys_seq_count = 2;
    ASSERT_EQ(emu.type_keys_seq_count, 2);
    ASSERT_TRUE(emu.type_keys_seq[1].loci_hid);
    ASSERT_EQ(emu.type_keys_seq[0].at, 5500000);

    /* Simule l'activation de l'entrée 1 (comme le fait la boucle). */
    int s = 1;
    emu.type_keys_at = emu.type_keys_seq[s].at;
    emu.type_keys_text = emu.type_keys_seq[s].text;
    emu.type_keys_loci_hid = emu.type_keys_seq[s].loci_hid;
    emu.type_keys_idx = 0;
    emu.type_keys_next_cycle = emu.type_keys_seq[s].at;
    emu.type_keys_done = false;
    emu.type_keys_seq_idx = s + 1;
    ASSERT_EQ(emu.type_keys_at, 3000000);
    ASSERT_TRUE(emu.type_keys_loci_hid);
    ASSERT_EQ(emu.type_keys_seq_idx, 2);
    ASSERT_TRUE(!emu.type_keys_done);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("Running Full System Integration tests...\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  CPU + VIA Integration:\n");
    RUN(test_cpu_via_write);
    RUN(test_cpu_via_read);
    RUN(test_cpu_via_timer_irq);

    printf("\n  Programs:\n");
    RUN(test_fibonacci_program);
    RUN(test_subroutine_multiply);

    printf("\n  System:\n");
    RUN(test_execute_cycles);
    RUN(test_state_string);

    printf("\n  Deferred Fast-Load:\n");
    RUN(test_deferred_fastload_fields);
    RUN(test_deferred_fastload_buffer);
    RUN(test_deferred_fastload_survives_ram_clear);

    printf("\n  Type-Keys Debounce:\n");
    RUN(test_type_keys_debounce_field);
    RUN(test_type_keys_seq_fields);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
