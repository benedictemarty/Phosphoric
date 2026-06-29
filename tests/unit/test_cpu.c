/**
 * @file test_cpu.c
 * @brief Comprehensive CPU 6502 unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 1.0.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cpu/cpu6502.h"
#include "cpu/cpu_internal.h"
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
        printf("FAIL\n    %s:%d: expected %d, got %d\n", __FILE__, __LINE__, (int)(b), (int)(a)); \
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

/* Setup helper: initialize CPU+memory and place code at a given address */
static void setup(cpu6502_t* cpu, memory_t* mem) {
    memory_init(mem);
    cpu_init(cpu, mem);
    /* Set reset vector to $0200 */
    mem->rom[0x3FFC] = 0x00; /* $FFFC low */
    mem->rom[0x3FFD] = 0x02; /* $FFFC high -> $0200 */
    cpu_reset(cpu);
    /* Verify PC is at $0200 */
}

/* Write a small program starting at addr */
static void write_program(memory_t* mem, uint16_t addr, const uint8_t* code, size_t len) {
    for (size_t i = 0; i < len; i++) {
        memory_write(mem, (uint16_t)(addr + i), code[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INITIALIZATION TESTS                                              */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_cpu_init) {
    cpu6502_t cpu;
    memory_t mem;
    memory_init(&mem);
    cpu_init(&cpu, &mem);
    ASSERT_EQ(cpu.A, 0);
    ASSERT_EQ(cpu.X, 0);
    ASSERT_EQ(cpu.Y, 0);
    ASSERT_TRUE(cpu.P & FLAG_UNUSED);
    ASSERT_TRUE(cpu.P & FLAG_INTERRUPT);
}

TEST(test_cpu_reset) {
    cpu6502_t cpu;
    memory_t mem;
    setup(&cpu, &mem);
    ASSERT_EQ(cpu.SP, 0xFD);
    ASSERT_EQ(cpu.PC, 0x0200);
    ASSERT_EQ(cpu.cycles, 0);
    ASSERT_FALSE(cpu.halted);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  LOAD/STORE TESTS                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_lda_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0x42}; /* LDA #$42 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_NEGATIVE));
}

TEST(test_lda_zero) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0x00}; /* LDA #$00 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x00);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
}

TEST(test_lda_negative) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0x80}; /* LDA #$80 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_NEGATIVE));
}

TEST(test_lda_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0010, 0x55);
    uint8_t code[] = {0xA5, 0x10}; /* LDA $10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x55);
}

TEST(test_lda_absolute) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x1234, 0xAB);
    uint8_t code[] = {0xAD, 0x34, 0x12}; /* LDA $1234 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAB);
}

TEST(test_lda_absolute_x) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x05;
    memory_write(&mem, 0x1239, 0xCD);
    uint8_t code[] = {0xBD, 0x34, 0x12}; /* LDA $1234,X */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xCD);
}

TEST(test_lda_absolute_y) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x03;
    memory_write(&mem, 0x1237, 0xEF);
    uint8_t code[] = {0xB9, 0x34, 0x12}; /* LDA $1234,Y */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xEF);
}

TEST(test_ldx_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA2, 0x33}; /* LDX #$33 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x33);
}

TEST(test_ldy_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA0, 0x77}; /* LDY #$77 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.Y, 0x77);
}

TEST(test_sta_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xBE;
    uint8_t code[] = {0x85, 0x50}; /* STA $50 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0050), 0xBE);
}

TEST(test_sta_absolute) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xCA;
    uint8_t code[] = {0x8D, 0x00, 0x10}; /* STA $1000 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x1000), 0xCA);
}

TEST(test_stx_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0xDD;
    uint8_t code[] = {0x86, 0x30}; /* STX $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0xDD);
}

TEST(test_sty_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0xEE;
    uint8_t code[] = {0x84, 0x40}; /* STY $40 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0040), 0xEE);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ARITHMETIC TESTS                                                  */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_adc_no_carry) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x10;
    uint8_t code[] = {0x69, 0x20}; /* ADC #$20 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x30);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY));
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_OVERFLOW));
}

TEST(test_adc_with_carry_in) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x10;
    cpu_set_flag(&cpu, FLAG_CARRY, true);
    uint8_t code[] = {0x69, 0x20}; /* ADC #$20 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x31);
}

TEST(test_adc_carry_out) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xFF;
    uint8_t code[] = {0x69, 0x01}; /* ADC #$01 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x00);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
}

TEST(test_adc_overflow) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x7F;
    uint8_t code[] = {0x69, 0x01}; /* ADC #$01 -> 0x80 (overflow) */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_OVERFLOW));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_NEGATIVE));
}

TEST(test_sbc_simple) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x50;
    cpu_set_flag(&cpu, FLAG_CARRY, true); /* No borrow */
    uint8_t code[] = {0xE9, 0x10}; /* SBC #$10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x40);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_sbc_borrow) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x00;
    cpu_set_flag(&cpu, FLAG_CARRY, true);
    uint8_t code[] = {0xE9, 0x01}; /* SBC #$01 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xFF);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY)); /* Borrow occurred */
}

TEST(test_adc_bcd) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_DECIMAL, true);
    cpu.A = 0x09;
    uint8_t code[] = {0x69, 0x01}; /* ADC #$01 in BCD */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x10); /* 9 + 1 = 10 in BCD */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  LOGIC TESTS                                                       */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_and_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xFF;
    uint8_t code[] = {0x29, 0x0F}; /* AND #$0F */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x0F);
}

TEST(test_ora_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xF0;
    uint8_t code[] = {0x09, 0x0F}; /* ORA #$0F */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xFF);
}

TEST(test_eor_immediate) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0xFF;
    uint8_t code[] = {0x49, 0xAA}; /* EOR #$AA */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x55);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  SHIFT/ROTATE TESTS                                                */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_asl_accumulator) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x81;
    uint8_t code[] = {0x0A}; /* ASL A */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x02);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_lsr_accumulator) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x03;
    uint8_t code[] = {0x4A}; /* LSR A */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x01);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_rol_accumulator) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x80;
    cpu_set_flag(&cpu, FLAG_CARRY, true);
    uint8_t code[] = {0x2A}; /* ROL A */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x01);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY)); /* old bit 7 */
}

TEST(test_ror_accumulator) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x01;
    cpu_set_flag(&cpu, FLAG_CARRY, true);
    uint8_t code[] = {0x6A}; /* ROR A */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY)); /* old bit 0 */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  COMPARE TESTS                                                     */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_cmp_equal) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x42;
    uint8_t code[] = {0xC9, 0x42}; /* CMP #$42 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_cmp_greater) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x50;
    uint8_t code[] = {0xC9, 0x42}; /* CMP #$42 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_cmp_less) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x10;
    uint8_t code[] = {0xC9, 0x42}; /* CMP #$42 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_cpx) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x10;
    uint8_t code[] = {0xE0, 0x10}; /* CPX #$10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_cpy) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x20;
    uint8_t code[] = {0xC0, 0x10}; /* CPY #$10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_ZERO));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_bit_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x0F;
    memory_write(&mem, 0x0010, 0xC0);
    uint8_t code[] = {0x24, 0x10}; /* BIT $10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));     /* $0F & $C0 == 0 */
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_NEGATIVE));  /* bit 7 of mem */
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_OVERFLOW));   /* bit 6 of mem */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INCREMENT/DECREMENT TESTS                                         */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_inx) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x0A;
    uint8_t code[] = {0xE8}; /* INX */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x0B);
}

TEST(test_inx_wrap) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0xFF;
    uint8_t code[] = {0xE8}; /* INX */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x00);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
}

TEST(test_iny) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x05;
    uint8_t code[] = {0xC8}; /* INY */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.Y, 0x06);
}

TEST(test_dex) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x05;
    uint8_t code[] = {0xCA}; /* DEX */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x04);
}

TEST(test_dey) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x01;
    uint8_t code[] = {0x88}; /* DEY */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.Y, 0x00);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));
}

TEST(test_inc_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0020, 0x41);
    uint8_t code[] = {0xE6, 0x20}; /* INC $20 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0020), 0x42);
}

TEST(test_dec_zero_page) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0020, 0x42);
    uint8_t code[] = {0xC6, 0x20}; /* DEC $20 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0020), 0x41);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  TRANSFER TESTS                                                    */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_tax) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x42;
    uint8_t code[] = {0xAA}; /* TAX */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x42);
}

TEST(test_tay) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x33;
    uint8_t code[] = {0xA8}; /* TAY */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.Y, 0x33);
}

TEST(test_txa) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x55;
    uint8_t code[] = {0x8A}; /* TXA */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x55);
}

TEST(test_tya) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x77;
    uint8_t code[] = {0x98}; /* TYA */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x77);
}

TEST(test_tsx) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xBA}; /* TSX */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0xFD); /* SP after reset */
}

TEST(test_txs) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0xFF;
    uint8_t code[] = {0x9A}; /* TXS */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.SP, 0xFF);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  STACK TESTS                                                       */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_pha_pla) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x42;
    uint8_t code[] = {
        0x48,       /* PHA */
        0xA9, 0x00, /* LDA #$00 */
        0x68        /* PLA */
    };
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); /* PHA */
    cpu_step(&cpu); /* LDA #$00 */
    ASSERT_EQ(cpu.A, 0x00);
    cpu_step(&cpu); /* PLA */
    ASSERT_EQ(cpu.A, 0x42);
}

TEST(test_php_plp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_CARRY, true);
    cpu_set_flag(&cpu, FLAG_ZERO, true);
    uint8_t saved_p = cpu.P;
    uint8_t code[] = {
        0x08,       /* PHP */
        0x18,       /* CLC */
        0x28        /* PLP */
    };
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); /* PHP */
    cpu_step(&cpu); /* CLC */
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY));
    cpu_step(&cpu); /* PLP */
    /* PLP clears B flag and sets unused */
    ASSERT_EQ(cpu.P, (saved_p & ~FLAG_BREAK) | FLAG_UNUSED);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  BRANCH TESTS                                                      */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_bne_taken) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.A = 0x01; /* Non-zero */
    cpu_set_flag(&cpu, FLAG_ZERO, false);
    uint8_t code[] = {0xD0, 0x02}; /* BNE +2 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0204); /* 0x0202 + 2 */
}

TEST(test_bne_not_taken) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_ZERO, true);
    uint8_t code[] = {0xD0, 0x02}; /* BNE +2 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0202); /* Not taken */
}

TEST(test_beq_taken) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_ZERO, true);
    uint8_t code[] = {0xF0, 0x05}; /* BEQ +5 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0207);
}

TEST(test_bcc_taken) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_CARRY, false);
    uint8_t code[] = {0x90, 0x03}; /* BCC +3 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0205);
}

TEST(test_branch_backward) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_ZERO, false);
    uint8_t code[] = {0xD0, 0xFE}; /* BNE -2 (loop to self) */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0200); /* Loops back */
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  JUMP/SUBROUTINE TESTS                                             */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_jmp_absolute) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0x4C, 0x00, 0x10}; /* JMP $1000 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000);
}

TEST(test_jmp_indirect) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x1000, 0x34);
    memory_write(&mem, 0x1001, 0x12);
    uint8_t code[] = {0x6C, 0x00, 0x10}; /* JMP ($1000) */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234);
}

TEST(test_jmp_indirect_page_bug) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* 6502 page boundary bug: JMP ($10FF) reads low from $10FF and high from $1000 */
    memory_write(&mem, 0x10FF, 0x34);
    memory_write(&mem, 0x1000, 0x12); /* NOT $1100 */
    uint8_t code[] = {0x6C, 0xFF, 0x10}; /* JMP ($10FF) */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234);
}

TEST(test_jsr_rts) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {
        0x20, 0x10, 0x02, /* JSR $0210 */
        0xEA,              /* NOP (return here) */
    };
    write_program(&mem, 0x0200, code, sizeof(code));
    /* Subroutine at $0210 */
    uint8_t sub[] = {0x60}; /* RTS */
    write_program(&mem, 0x0210, sub, sizeof(sub));

    cpu_step(&cpu); /* JSR */
    ASSERT_EQ(cpu.PC, 0x0210);
    cpu_step(&cpu); /* RTS */
    ASSERT_EQ(cpu.PC, 0x0203);
}

TEST(test_brk) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Set IRQ vector */
    mem.rom[0x3FFE] = 0x00; /* $FFFE low */
    mem.rom[0x3FFF] = 0x10; /* $FFFE high -> $1000 */
    uint8_t code[] = {0x00}; /* BRK */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_INTERRUPT));
}

TEST(test_rti) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Push return state as if an interrupt occurred from PC=$0300 with P=$20 */
    cpu_push_word(&cpu, 0x0300);
    cpu_push(&cpu, FLAG_UNUSED);
    uint8_t code[] = {0x40}; /* RTI */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0300);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  FLAG INSTRUCTION TESTS                                            */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_clc_sec) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0x38, 0x18}; /* SEC, CLC */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_cli_sei) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0x58, 0x78}; /* CLI, SEI */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_INTERRUPT));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_INTERRUPT));
}

TEST(test_cld_sed) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xF8, 0xD8}; /* SED, CLD */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_DECIMAL));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_DECIMAL));
}

TEST(test_clv) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu_set_flag(&cpu, FLAG_OVERFLOW, true);
    uint8_t code[] = {0xB8}; /* CLV */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_OVERFLOW));
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  INTERRUPT TESTS                                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_irq) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Set IRQ vector to $1000 */
    mem.rom[0x3FFE] = 0x00;
    mem.rom[0x3FFF] = 0x10;
    /* CLI to enable interrupts */
    cpu_set_flag(&cpu, FLAG_INTERRUPT, false);
    /* NOP at $0200 */
    uint8_t code[] = {0xEA};
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_irq(&cpu);
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_INTERRUPT));
}

TEST(test_irq_masked) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* I flag set by default after reset - IRQ should be masked */
    uint8_t code[] = {0xEA}; /* NOP */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_irq(&cpu);
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0201); /* Executed NOP, not jumped to IRQ vector */
}

TEST(test_nmi) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Set NMI vector to $2000 */
    mem.rom[0x3FFA] = 0x00;
    mem.rom[0x3FFB] = 0x20;
    uint8_t code[] = {0xEA}; /* NOP */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_nmi(&cpu);
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x2000);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  DISASSEMBLER TEST                                                 */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_disassemble) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0x42}; /* LDA #$42 */
    write_program(&mem, 0x0200, code, sizeof(code));
    char buf[64];
    int size = cpu_disassemble(&cpu, 0x0200, buf, sizeof(buf));
    ASSERT_EQ(size, 2);
    ASSERT_TRUE(strstr(buf, "LDA") != NULL);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MULTI-INSTRUCTION PROGRAM TESTS                                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_count_loop) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Count from 0 to 5 in X register */
    uint8_t code[] = {
        0xA2, 0x00, /* LDX #$00 */
        0xE8,       /* INX */
        0xE0, 0x05, /* CPX #$05 */
        0xD0, 0xFB, /* BNE -5 (back to INX) */
        0xEA,       /* NOP (end) */
    };
    write_program(&mem, 0x0200, code, sizeof(code));
    /* Run enough steps */
    for (int i = 0; i < 30; i++) {
        cpu_step(&cpu);
        if (cpu.PC == 0x0207) break;
    }
    ASSERT_EQ(cpu.X, 0x05);
}

TEST(test_memory_copy) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* Copy 4 bytes from $0300 to $0400 */
    /* Note: $0300 is I/O space, use $0500 instead */
    memory_write(&mem, 0x0500, 0xAA);
    memory_write(&mem, 0x0501, 0xBB);
    memory_write(&mem, 0x0502, 0xCC);
    memory_write(&mem, 0x0503, 0xDD);
    uint8_t code[] = {
        0xA2, 0x00,       /* LDX #$00 */
        0xBD, 0x00, 0x05, /* LDA $0500,X */
        0x9D, 0x00, 0x06, /* STA $0600,X */
        0xE8,             /* INX */
        0xE0, 0x04,       /* CPX #$04 */
        0xD0, 0xF5,       /* BNE -11 */
    };
    write_program(&mem, 0x0200, code, sizeof(code));
    for (int i = 0; i < 50; i++) {
        cpu_step(&cpu);
        if (cpu.X == 0x04 && cpu_get_flag(&cpu, FLAG_ZERO)) break;
    }
    ASSERT_EQ(memory_read(&mem, 0x0600), 0xAA);
    ASSERT_EQ(memory_read(&mem, 0x0601), 0xBB);
    ASSERT_EQ(memory_read(&mem, 0x0602), 0xCC);
    ASSERT_EQ(memory_read(&mem, 0x0603), 0xDD);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ADDRESSING MODE TESTS                                             */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_indexed_indirect) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0x04;
    /* ZP at $14 contains pointer $1000 */
    memory_write(&mem, 0x0014, 0x00);
    memory_write(&mem, 0x0015, 0x10);
    memory_write(&mem, 0x1000, 0xAB);
    uint8_t code[] = {0xA1, 0x10}; /* LDA ($10,X) */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAB);
}

TEST(test_indirect_indexed) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.Y = 0x05;
    /* ZP at $20 contains pointer $1000 */
    memory_write(&mem, 0x0020, 0x00);
    memory_write(&mem, 0x0021, 0x10);
    memory_write(&mem, 0x1005, 0xCD);
    uint8_t code[] = {0xB1, 0x20}; /* LDA ($20),Y */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0xCD);
}

TEST(test_zero_page_x_wrap) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    cpu.X = 0xFF;
    memory_write(&mem, 0x000F, 0x77); /* ($10 + $FF) & $FF = $0F */
    uint8_t code[] = {0xB5, 0x10}; /* LDA $10,X */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x77);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ILLEGAL / UNDOCUMENTED OPCODE TESTS (NMOS 6502)                   */
/* ═══════════════════════════════════════════════════════════════════ */

TEST(test_illegal_lax_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0010, 0x80);
    uint8_t code[] = {0xA7, 0x10}; /* LAX $10 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80);
    ASSERT_EQ(cpu.X, 0x80);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_NEGATIVE));
}

TEST(test_illegal_sax_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0xCC, 0xA2, 0xF0, 0x87, 0x20}; /* LDA #$CC; LDX #$F0; SAX $20 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0020), 0xC0); /* $CC & $F0 */
}

TEST(test_illegal_slo_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x40);          /* mem<<1 = $80, carry=0 */
    uint8_t code[] = {0xA9, 0x01, 0x07, 0x30};  /* LDA #$01; SLO $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x80); /* shifted */
    ASSERT_EQ(cpu.A, 0x81);                      /* $01 | $80 */
    ASSERT_FALSE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_illegal_rla_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x40);          /* ROL with C=0 -> $80 */
    uint8_t code[] = {0x18, 0xA9, 0xFF, 0x27, 0x30}; /* CLC; LDA #$FF; RLA $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x80);
    ASSERT_EQ(cpu.A, 0x80);                      /* $FF & $80 */
}

TEST(test_illegal_sre_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x02);          /* >>1 = $01, carry=0 */
    uint8_t code[] = {0xA9, 0xFF, 0x47, 0x30};  /* LDA #$FF; SRE $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x01);
    ASSERT_EQ(cpu.A, 0xFE);                      /* $FF ^ $01 */
}

TEST(test_illegal_rra_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x02);          /* ROR C=0 -> $01, carry out=0 */
    uint8_t code[] = {0x18, 0xA9, 0x10, 0x67, 0x30}; /* CLC; LDA #$10; RRA $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x01);
    ASSERT_EQ(cpu.A, 0x11);                      /* $10 + $01 + carry(0) */
}

TEST(test_illegal_dcp_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x43);          /* dec -> $42 */
    uint8_t code[] = {0xA9, 0x42, 0xC7, 0x30};  /* LDA #$42; DCP $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x42);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_ZERO));  /* A == mem */
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY));
}

TEST(test_illegal_isc_zp) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    memory_write(&mem, 0x0030, 0x0F);          /* inc -> $10 */
    uint8_t code[] = {0x38, 0xA9, 0x20, 0xE7, 0x30}; /* SEC; LDA #$20; ISC $30 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(memory_read(&mem, 0x0030), 0x10);
    ASSERT_EQ(cpu.A, 0x10);                      /* $20 - $10 */
}

TEST(test_illegal_anc) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0xFF, 0x0B, 0x80}; /* LDA #$FF; ANC #$80 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_NEGATIVE));
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY)); /* C = bit7 */
}

TEST(test_illegal_alr) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0xA9, 0xFF, 0x4B, 0x03}; /* LDA #$FF; ALR #$03 -> ($FF&$03)>>1 */
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x01);                      /* $03 >> 1 */
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY)); /* bit0 of $03 */
}

TEST(test_illegal_sbx) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* LDA #$FF; LDX #$0F; SBX #$01 -> X = ($FF&$0F)-$01 = $0E */
    uint8_t code[] = {0xA9, 0xFF, 0xA2, 0x0F, 0xCB, 0x01};
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(cpu.X, 0x0E);
    ASSERT_TRUE(cpu_get_flag(&cpu, FLAG_CARRY)); /* $0F >= $01 */
}

TEST(test_illegal_jam_halts) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    uint8_t code[] = {0x02}; /* JAM/KIL */
    write_program(&mem, 0x0200, code, sizeof(code));
    ASSERT_FALSE(cpu.halted);
    cpu_step(&cpu);
    ASSERT_TRUE(cpu.halted);
}

TEST(test_illegal_nop_imm_consumes_operand) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* NOP #$AA (0x80) then LDA #$42; PC must skip the immediate operand */
    uint8_t code[] = {0x80, 0xAA, 0xA9, 0x42};
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0202);
    cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42);
}

TEST(test_illegal_sbc_eb) {
    cpu6502_t cpu; memory_t mem;
    setup(&cpu, &mem);
    /* SEC; LDA #$50; SBC #$10 (unofficial 0xEB) -> $40 */
    uint8_t code[] = {0x38, 0xA9, 0x50, 0xEB, 0x10};
    write_program(&mem, 0x0200, code, sizeof(code));
    cpu_step(&cpu); cpu_step(&cpu); cpu_step(&cpu);
    ASSERT_EQ(cpu.A, 0x40);
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  MAIN                                                              */
/* ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("Running CPU 6502 tests...\n");
    printf("═══════════════════════════════════════════════════════════\n");

    printf("\n  Initialization:\n");
    RUN(test_cpu_init);
    RUN(test_cpu_reset);

    printf("\n  Load/Store:\n");
    RUN(test_lda_immediate);
    RUN(test_lda_zero);
    RUN(test_lda_negative);
    RUN(test_lda_zero_page);
    RUN(test_lda_absolute);
    RUN(test_lda_absolute_x);
    RUN(test_lda_absolute_y);
    RUN(test_ldx_immediate);
    RUN(test_ldy_immediate);
    RUN(test_sta_zero_page);
    RUN(test_sta_absolute);
    RUN(test_stx_zero_page);
    RUN(test_sty_zero_page);

    printf("\n  Arithmetic:\n");
    RUN(test_adc_no_carry);
    RUN(test_adc_with_carry_in);
    RUN(test_adc_carry_out);
    RUN(test_adc_overflow);
    RUN(test_sbc_simple);
    RUN(test_sbc_borrow);
    RUN(test_adc_bcd);

    printf("\n  Logic:\n");
    RUN(test_and_immediate);
    RUN(test_ora_immediate);
    RUN(test_eor_immediate);

    printf("\n  Shift/Rotate:\n");
    RUN(test_asl_accumulator);
    RUN(test_lsr_accumulator);
    RUN(test_rol_accumulator);
    RUN(test_ror_accumulator);

    printf("\n  Compare:\n");
    RUN(test_cmp_equal);
    RUN(test_cmp_greater);
    RUN(test_cmp_less);
    RUN(test_cpx);
    RUN(test_cpy);
    RUN(test_bit_zero_page);

    printf("\n  Increment/Decrement:\n");
    RUN(test_inx);
    RUN(test_inx_wrap);
    RUN(test_iny);
    RUN(test_dex);
    RUN(test_dey);
    RUN(test_inc_zero_page);
    RUN(test_dec_zero_page);

    printf("\n  Transfers:\n");
    RUN(test_tax);
    RUN(test_tay);
    RUN(test_txa);
    RUN(test_tya);
    RUN(test_tsx);
    RUN(test_txs);

    printf("\n  Stack:\n");
    RUN(test_pha_pla);
    RUN(test_php_plp);

    printf("\n  Branches:\n");
    RUN(test_bne_taken);
    RUN(test_bne_not_taken);
    RUN(test_beq_taken);
    RUN(test_bcc_taken);
    RUN(test_branch_backward);

    printf("\n  Jump/Subroutine:\n");
    RUN(test_jmp_absolute);
    RUN(test_jmp_indirect);
    RUN(test_jmp_indirect_page_bug);
    RUN(test_jsr_rts);
    RUN(test_brk);
    RUN(test_rti);

    printf("\n  Flags:\n");
    RUN(test_clc_sec);
    RUN(test_cli_sei);
    RUN(test_cld_sed);
    RUN(test_clv);

    printf("\n  Interrupts:\n");
    RUN(test_irq);
    RUN(test_irq_masked);
    RUN(test_nmi);

    printf("\n  Disassembler:\n");
    RUN(test_disassemble);

    printf("\n  Programs:\n");
    RUN(test_count_loop);
    RUN(test_memory_copy);

    printf("\n  Addressing Modes:\n");
    RUN(test_indexed_indirect);
    RUN(test_indirect_indexed);
    RUN(test_zero_page_x_wrap);

    printf("\n  Illegal/Undocumented Opcodes:\n");
    RUN(test_illegal_lax_zp);
    RUN(test_illegal_sax_zp);
    RUN(test_illegal_slo_zp);
    RUN(test_illegal_rla_zp);
    RUN(test_illegal_sre_zp);
    RUN(test_illegal_rra_zp);
    RUN(test_illegal_dcp_zp);
    RUN(test_illegal_isc_zp);
    RUN(test_illegal_anc);
    RUN(test_illegal_alr);
    RUN(test_illegal_sbx);
    RUN(test_illegal_jam_halts);
    RUN(test_illegal_nop_imm_consumes_operand);
    RUN(test_illegal_sbc_eb);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
