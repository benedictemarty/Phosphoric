/**
 * @file test_loci_coproc.c
 * @brief EXPERIMENTAL — Coprocesseur arithmétique LOCI $A9 (branche experiment/…).
 * @author bmarty <bmarty@mailo.com>
 *
 * Vecteurs déterministes pour op_math() : entiers, IEEE754, transcendantes, pont
 * MBF, gestion d'erreurs, et garde opt-in (OFF → ENOSYS). Pilote directement le
 * handler via l'ABI fastcall (xstack + API_A + retour AX:SREG), sans Oric.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif
#include "io/loci.h"
#include "io/loci_internal.h"

static int tests_passed = 0, tests_failed = 0;
#define TEST(n) static void n(void)
#define RUN(n) do { printf("  %-42s", #n); n(); printf("\n"); } while (0)
#define ASSERT_TRUE(c) do { if (!(c)) { printf("[FAIL] %s:%d %s", __FILE__, __LINE__, #c); tests_failed++; return; } } while (0)
#define ASSERT_EQ(a,b) do { long _a=(long)(a),_b=(long)(b); if(_a!=_b){printf("[FAIL] %s:%d %s==%s (%ld!=%ld)",__FILE__,__LINE__,#a,#b,_a,_b);tests_failed++;return;} } while(0)
#define PASS() do { tests_passed++; printf("[OK]"); } while (0)

static loci_t L;

static void begin(uint8_t subcode) {
    memset(&L, 0, sizeof(L));
    L.coproc_enabled = true;
    xstack_zero(&L);
    L.regs[LOCI_REG_API_A] = subcode;  /* posé APRÈS, écrasé par le retour */
}
/* Le 6502 pousse les opérandes dans l'ordre (a d'abord, b au sommet). */
static void push_u16(uint16_t v){ xstack_push_n(&L,&v,2); }
static void push_u32(uint32_t v){ xstack_push_n(&L,&v,4); }
static void push_i16(int16_t v){ xstack_push_n(&L,&v,2); }
static void push_i32(int32_t v){ xstack_push_n(&L,&v,4); }
static void push_f32(float f){ uint32_t b; memcpy(&b,&f,4); xstack_push_n(&L,&b,4); }
static void push_bytes(const uint8_t* p, int n){ xstack_push_n(&L,p,n); }

/* Attention : begin() pose subcode dans API_A ; comme les push suivent, il faut
 * (re)poser le subcode juste avant l'appel car push n'y touche pas — OK. */
static uint32_t call_axsreg(void) {
    op_math(&L);
    return (uint32_t)L.regs[LOCI_REG_API_A] | ((uint32_t)L.regs[LOCI_REG_API_X] << 8) |
           ((uint32_t)L.regs[LOCI_REG_API_SREG] << 16) | ((uint32_t)L.regs[LOCI_REG_API_SREG_HI] << 24);
}
static float call_f32(void) { uint32_t b = call_axsreg(); float f; memcpy(&f,&b,4); return f; }
static uint16_t pop_result_u16(void) {   /* dépile le sommet du xstack (résultat) */
    uint16_t v; memcpy(&v,&L.xstack[L.xstack_ptr],2); L.xstack_ptr+=2; return v;
}
static uint32_t pop_result_u32(void) {
    uint32_t v; memcpy(&v,&L.xstack[L.xstack_ptr],4); L.xstack_ptr+=4; return v;
}
static int approx(float a, float b) { return fabsf(a-b) <= 1e-5f * (1.0f + fabsf(b)); }

/* ── Entiers ────────────────────────────────────────────────────────────── */
TEST(t_mul_u16) {
    begin(0x00); push_u16(1234); push_u16(5678); L.regs[LOCI_REG_API_A]=0x00;
    ASSERT_EQ(call_axsreg(), 1234u*5678u);   /* 7006652 */
    PASS();
}
TEST(t_mul_i16_negative) {
    begin(0x01); push_i16(-1234); push_i16(5678); L.regs[LOCI_REG_API_A]=0x01;
    ASSERT_EQ((int32_t)call_axsreg(), -1234*5678);
    PASS();
}
TEST(t_divmod_u16) {
    begin(0x02); push_u16(1000); push_u16(7); L.regs[LOCI_REG_API_A]=0x02;
    op_math(&L);
    ASSERT_EQ(L.regs[LOCI_REG_API_A], 0);      /* succès */
    ASSERT_EQ(pop_result_u16(), 142);          /* quotient (sommet) */
    ASSERT_EQ(pop_result_u16(), 6);            /* reste */
    PASS();
}
TEST(t_divmod_u16_by_zero_errno) {
    begin(0x02); push_u16(1000); push_u16(0); L.regs[LOCI_REG_API_A]=0x02;
    ASSERT_EQ(call_axsreg(), 0xFFFFFFFFu);     /* -1 */
    ASSERT_EQ(L.regs[LOCI_REG_API_ERRNO_LO], LOCI_EINVAL);
    PASS();
}
TEST(t_mul_u32) {
    begin(0x04); push_u32(100000); push_u32(40000); L.regs[LOCI_REG_API_A]=0x04;
    ASSERT_EQ(call_axsreg(), (uint32_t)(100000u*40000u));  /* tronqué 32b */
    PASS();
}
TEST(t_divmod_i32) {
    begin(0x06); push_i32(-1000003); push_i32(7); L.regs[LOCI_REG_API_A]=0x06;
    op_math(&L);
    ASSERT_EQ((int32_t)pop_result_u32(), -1000003/7);
    ASSERT_EQ((int32_t)pop_result_u32(), -1000003%7);
    PASS();
}

/* ── Flottant IEEE754 ───────────────────────────────────────────────────── */
TEST(t_fadd) { begin(0x10); push_f32(1.5f); push_f32(2.25f); L.regs[LOCI_REG_API_A]=0x10; ASSERT_TRUE(call_f32()==3.75f); PASS(); }
TEST(t_fsub) { begin(0x11); push_f32(5.0f); push_f32(1.25f); L.regs[LOCI_REG_API_A]=0x11; ASSERT_TRUE(call_f32()==3.75f); PASS(); }
TEST(t_fmul) { begin(0x12); push_f32(2.5f); push_f32(4.0f); L.regs[LOCI_REG_API_A]=0x12; ASSERT_TRUE(call_f32()==10.0f); PASS(); }
TEST(t_fdiv) { begin(0x13); push_f32(7.0f); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x13; ASSERT_TRUE(call_f32()==3.5f); PASS(); }
TEST(t_fcmp) {
    begin(0x14); push_f32(1.0f); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x14; ASSERT_EQ((int32_t)call_axsreg(), -1);
    begin(0x14); push_f32(2.0f); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x14; ASSERT_EQ((int32_t)call_axsreg(), 0);
    begin(0x14); push_f32(3.0f); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x14; ASSERT_EQ((int32_t)call_axsreg(), 1);
    PASS();
}
TEST(t_itof_ftoi) {
    begin(0x15); push_i32(-42); L.regs[LOCI_REG_API_A]=0x15; ASSERT_TRUE(call_f32()==-42.0f);
    begin(0x16); push_f32(3.9f); L.regs[LOCI_REG_API_A]=0x16; ASSERT_EQ((int32_t)call_axsreg(), 3);
    PASS();
}

/* ── Transcendantes ─────────────────────────────────────────────────────── */
TEST(t_fsqrt) { begin(0x20); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x20; ASSERT_TRUE(approx(call_f32(), sqrtf(2.0f))); PASS(); }
TEST(t_fsin_fcos) {
    begin(0x21); push_f32((float)M_PI/2); L.regs[LOCI_REG_API_A]=0x21; ASSERT_TRUE(approx(call_f32(), 1.0f));
    begin(0x22); push_f32(0.0f);          L.regs[LOCI_REG_API_A]=0x22; ASSERT_TRUE(approx(call_f32(), 1.0f));
    PASS();
}
TEST(t_fpow_flog_fexp) {
    begin(0x27); push_f32(2.0f); push_f32(10.0f); L.regs[LOCI_REG_API_A]=0x27; ASSERT_TRUE(approx(call_f32(), 1024.0f));
    begin(0x26); push_f32(1.0f); L.regs[LOCI_REG_API_A]=0x26; ASSERT_TRUE(approx(call_f32(), (float)M_E));
    begin(0x25); push_f32((float)M_E); L.regs[LOCI_REG_API_A]=0x25; ASSERT_TRUE(approx(call_f32(), 1.0f));
    PASS();
}
/* log base 10 ($28) : le LOG du BASIC Oric est en base 10 (LOG(2)=.30103). */
TEST(t_flog10) {
    begin(0x28); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x28; ASSERT_TRUE(approx(call_f32(), log10f(2.0f)));
    begin(0x28); push_f32(1000.0f); L.regs[LOCI_REG_API_A]=0x28; ASSERT_TRUE(approx(call_f32(), 3.0f));
    PASS();
}
TEST(t_fatan_vs_fatan2) {
    begin(0x24); push_f32(1.0f); L.regs[LOCI_REG_API_A]=0x24; ASSERT_TRUE(approx(call_f32(), atanf(1.0f)));       /* 1 arg */
    begin(0x24); push_f32(1.0f); push_f32(1.0f); L.regs[LOCI_REG_API_A]=0x24; ASSERT_TRUE(approx(call_f32(), atan2f(1.0f,1.0f))); /* 2 args */
    PASS();
}

/* ── Pont MBF ↔ IEEE754 ─────────────────────────────────────────────────── */
TEST(t_ieee_to_mbf_one) {
    begin(0x31); push_f32(1.0f); L.regs[LOCI_REG_API_A]=0x31;
    op_math(&L);
    ASSERT_EQ(L.regs[LOCI_REG_API_A], 0);
    /* MBF(1.0) = 81 00 00 00 00, exposant (octet0) au sommet du xstack. */
    ASSERT_EQ(L.xstack[L.xstack_ptr+0], 0x81);
    ASSERT_EQ(L.xstack[L.xstack_ptr+1], 0x00);
    ASSERT_EQ(L.xstack[L.xstack_ptr+2], 0x00);
    ASSERT_EQ(L.xstack[L.xstack_ptr+3], 0x00);
    ASSERT_EQ(L.xstack[L.xstack_ptr+4], 0x00);
    PASS();
}
TEST(t_mbf_to_ieee_one) {
    begin(0x30);
    uint8_t mbf[5] = { 0x81, 0x00, 0x00, 0x00, 0x00 };  /* poussé : octet0 au sommet */
    push_bytes(mbf, 5); L.regs[LOCI_REG_API_A]=0x30;
    ASSERT_TRUE(call_f32() == 1.0f);
    PASS();
}
TEST(t_mbf_roundtrip) {
    float vals[] = { -0.5f, 3.14159f, 100.0f, -1234.5f, 0.0f };
    for (unsigned i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
        begin(0x31); push_f32(vals[i]); L.regs[LOCI_REG_API_A]=0x31; op_math(&L);
        uint8_t m[5]; memcpy(m, &L.xstack[L.xstack_ptr], 5);
        begin(0x30); push_bytes(m, 5); L.regs[LOCI_REG_API_A]=0x30;
        ASSERT_TRUE(approx(call_f32(), vals[i]));
    }
    PASS();
}
/* Golden : octets MBF CONFIRMÉS contre la vraie ROM BASIC 1.1 Oric (variable
 * stockée en $0503, lue par dump RAM headless). Prouve la compatibilité binaire
 * du pont $31 avec le flottant du BASIC Oric — pas une hypothèse. */
TEST(t_ieee_to_mbf_golden_oric_rom) {
    struct { float f; uint8_t mbf[5]; } vec[] = {
        {  1.5f, { 0x81, 0x40, 0x00, 0x00, 0x00 } },  /* A=1.5  → ROM $0503 */
        { -0.5f, { 0x80, 0x80, 0x00, 0x00, 0x00 } },  /* A=-0.5 → ROM */
        { 100.0f,{ 0x87, 0x48, 0x00, 0x00, 0x00 } },  /* A=100  → ROM */
    };
    for (unsigned i = 0; i < sizeof(vec)/sizeof(vec[0]); i++) {
        begin(0x31); push_f32(vec[i].f); L.regs[LOCI_REG_API_A]=0x31; op_math(&L);
        ASSERT_EQ(L.regs[LOCI_REG_API_A], 0);
        for (int b = 0; b < 5; b++) ASSERT_EQ(L.xstack[L.xstack_ptr+b], vec[i].mbf[b]);
    }
    PASS();
}
TEST(t_ieee_to_mbf_inf_errno) {
    begin(0x31); push_f32(INFINITY); L.regs[LOCI_REG_API_A]=0x31;
    ASSERT_EQ(call_axsreg(), 0xFFFFFFFFu);
    ASSERT_EQ(L.regs[LOCI_REG_API_ERRNO_LO], LOCI_ERANGE);
    PASS();
}

/* ── Garde opt-in ───────────────────────────────────────────────────────── */
TEST(t_disabled_returns_enosys) {
    begin(0x10); push_f32(1.0f); push_f32(2.0f); L.regs[LOCI_REG_API_A]=0x10;
    L.coproc_enabled = false;                    /* OFF */
    ASSERT_EQ(call_axsreg(), 0xFFFFFFFFu);
    ASSERT_EQ(L.regs[LOCI_REG_API_ERRNO_LO], LOCI_ENOSYS);
    PASS();
}
TEST(t_unknown_subcode_enosys) {
    begin(0x7F); L.regs[LOCI_REG_API_A]=0x7F;
    ASSERT_EQ(call_axsreg(), 0xFFFFFFFFu);
    ASSERT_EQ(L.regs[LOCI_REG_API_ERRNO_LO], LOCI_ENOSYS);
    PASS();
}

int main(void) {
    printf("\n=== LOCI coprocesseur math $A9 (EXPERIMENTAL) ===\n");
    RUN(t_mul_u16); RUN(t_mul_i16_negative); RUN(t_divmod_u16);
    RUN(t_divmod_u16_by_zero_errno); RUN(t_mul_u32); RUN(t_divmod_i32);
    RUN(t_fadd); RUN(t_fsub); RUN(t_fmul); RUN(t_fdiv); RUN(t_fcmp); RUN(t_itof_ftoi);
    RUN(t_fsqrt); RUN(t_fsin_fcos); RUN(t_fpow_flog_fexp); RUN(t_flog10); RUN(t_fatan_vs_fatan2);
    RUN(t_ieee_to_mbf_one); RUN(t_mbf_to_ieee_one); RUN(t_mbf_roundtrip);
    RUN(t_ieee_to_mbf_golden_oric_rom);
    RUN(t_ieee_to_mbf_inf_errno);
    RUN(t_disabled_returns_enosys); RUN(t_unknown_subcode_enosys);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
