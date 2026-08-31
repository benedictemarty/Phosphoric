/**
 * @file loci_math.c
 * @brief EXPERIMENTAL — Coprocesseur arithmétique LOCI (opcode fastcall $A9).
 * @author bmarty <bmarty@mailo.com>
 *
 * Branche experiment/loci-coproc-acia-reliable — extension OPT-IN validée sur
 * banc Phosphoric avant tout matériel (l'auteur a une carte LOCI mais pas d'Oric).
 * Spec : ~/loci/extensions/coprocessor-A9/spec-coprocesseur-math.md (v0.2).
 *
 * Le 6502 n'a ni mul/div ni flottant matériels ; le RP2040 les apporte. On
 * délègue au « coprocesseur » (ici : la libm de l'hôte) via l'ABI fastcall
 * existante : UN opcode $A9, le sous-code d'opération dans API_A, les opérandes
 * sur le xstack (little-endian), retour scalaire en AX:SREG ou multi-valeurs
 * repoussées sur le xstack (cf. eng-modele-execution-loci.md).
 *
 * OFF par défaut (`coproc_enabled`) → $A9 renvoie ENOSYS, identique à un firmware
 * non patché et détectable côté 6502. Périmètre : sous-codes DÉTERMINISTES
 * (entiers, IEEE754, transcendantes, pont MBF). Les ops par bloc XRAM ($40-$42),
 * qui touchent la mémoire servie par le PIO, sont différées (roadmap).
 */
#include "io/loci.h"
#include "io/loci_internal.h"
#include "utils/logging.h"

#include <string.h>
#include <math.h>

/* ── Sous-codes (API_A) — figés d'après la spec §3 ──────────────────────────── */
enum {
    /* Entiers */
    MATH_MUL_U16 = 0x00, MATH_MUL_I16 = 0x01, MATH_DIVMOD_U16 = 0x02,
    MATH_DIVMOD_I16 = 0x03, MATH_MUL_U32 = 0x04, MATH_DIVMOD_U32 = 0x05,
    MATH_DIVMOD_I32 = 0x06,
    /* Flottant IEEE754 32 bits */
    MATH_FADD = 0x10, MATH_FSUB = 0x11, MATH_FMUL = 0x12, MATH_FDIV = 0x13,
    MATH_FCMP = 0x14, MATH_ITOF = 0x15, MATH_FTOI = 0x16,
    /* Transcendantes */
    MATH_FSQRT = 0x20, MATH_FSIN = 0x21, MATH_FCOS = 0x22, MATH_FTAN = 0x23,
    MATH_FATAN = 0x24, MATH_FLOG = 0x25, MATH_FEXP = 0x26, MATH_FPOW = 0x27,
    MATH_FLOG10 = 0x28,   /* log base 10 (le LOG du BASIC Oric est en base 10) */
    /* Pont MBF ↔ IEEE754 */
    MATH_MBF_TO_IEEE = 0x30, MATH_IEEE_TO_MBF = 0x31,
};

/* ── Dépilement xstack (top = dernier octet poussé par le 6502) ─────────────
 * On lit N octets little-endian à partir de xstack_ptr puis on avance. Pour un
 * op(a,b), le 6502 pousse a puis b (b au sommet) → on dépile b d'abord, a ensuite. */
static bool pop_n(loci_t* loci, void* out, size_t n) {
    if ((size_t)loci->xstack_ptr + n > LOCI_XSTACK_SIZE) return false;
    memcpy(out, &loci->xstack[loci->xstack_ptr], n);
    loci->xstack_ptr = (uint16_t)(loci->xstack_ptr + n);
    return true;
}
static bool pop_u16(loci_t* loci, uint16_t* v) { return pop_n(loci, v, 2); }
static bool pop_u32(loci_t* loci, uint32_t* v) { return pop_n(loci, v, 4); }
static bool pop_i16(loci_t* loci, int16_t* v)  { return pop_n(loci, v, 2); }
static bool pop_i32(loci_t* loci, int32_t* v)  { return pop_n(loci, v, 4); }
static bool pop_f32(loci_t* loci, float* v) {
    uint32_t bits; if (!pop_u32(loci, &bits)) return false;
    memcpy(v, &bits, 4); return true;
}

/* Renvoi d'un float en AX:SREG (bit-cast, comme le firmware). */
static void return_f32(loci_t* loci, float f) {
    uint32_t bits; memcpy(&bits, &f, 4);
    api_return_axsreg(loci, bits);
}

/* Renvoi multi-valeurs : xstack vidé puis on pousse `hi` (dépilé en second par le
 * 6502) puis `lo` (au sommet, dépilé en premier). api_return_ax(0) = succès. */
static void return_pair_u16(loci_t* loci, uint16_t first, uint16_t second) {
    xstack_zero(loci);
    xstack_push_n(loci, &second, 2);
    xstack_push_n(loci, &first, 2);   /* sommet = première valeur dépilée */
    api_return_ax(loci, 0);
}
static void return_pair_u32(loci_t* loci, uint32_t first, uint32_t second) {
    xstack_zero(loci);
    xstack_push_n(loci, &second, 4);
    xstack_push_n(loci, &first, 4);
    api_return_ax(loci, 0);
}

/* ── Pont MBF 5 octets (BASIC Oric, style MS/CBM) ↔ IEEE754 f32 ─────────────
 * MBF : octet0 = exposant biaisé de 128 (0 ⇒ valeur nulle) ; octets1-4 = mantisse
 * 32 bits big-endian, bit de poids fort (bit7 octet1) = SIGNE, le 1 de tête étant
 * implicite à cette position. Valeur = (-1)^signe · mantisse · 2^(exp-160).
 * Réf : MBF 1.0 = {81 00 00 00 00}. (Conversions PURES : pas d'adresse ZP/FAC,
 * cf. spec §7.1 — le point ouvert ne concerne que l'accélération BASIC, pas ces
 * opcodes.) */
static float mbf5_to_f32(const uint8_t m[5]) {
    if (m[0] == 0) return 0.0f;                       /* exposant 0 = zéro */
    uint8_t sign = m[1] & 0x80u;
    uint32_t mant = ((uint32_t)(m[1] | 0x80u) << 24) | ((uint32_t)m[2] << 16) |
                    ((uint32_t)m[3] << 8) | (uint32_t)m[4];   /* 1 implicite restauré */
    double val = ldexp((double)mant, (int)m[0] - 160);        /* mant · 2^(exp-160) */
    return (float)(sign ? -val : val);
}

/* Renvoie true si conversion OK ; false si non représentable en MBF (Inf/NaN,
 * overflow d'exposant). Underflow → 0 (MBF n'a pas de dénormaux). */
static bool f32_to_mbf5(float f, uint8_t out[5]) {
    if (isnan(f) || isinf(f)) return false;
    if (f == 0.0f) { memset(out, 0, 5); return true; }
    int sign = signbit(f) ? 0x80 : 0x00;
    double a = fabs((double)f);
    int e;
    double frac = frexp(a, &e);            /* a = frac · 2^e, frac ∈ [0.5,1) */
    /* mantisse 32 bits : frac · 2^32 ∈ [2^31, 2^32) */
    uint64_t mant = (uint64_t)llround(frac * 4294967296.0);  /* 2^32 */
    if (mant >= 0x100000000ULL) {          /* arrondi → 2^32 : renormaliser */
        mant >>= 1; e += 1;
    }
    int exp = e + 128;                     /* value = mant · 2^(e-32) = mant·2^(exp-160) */
    if (exp > 255) return false;           /* overflow */
    if (exp < 1) { memset(out, 0, 5); return true; }  /* underflow → 0 */
    out[0] = (uint8_t)exp;
    out[1] = (uint8_t)(((mant >> 24) & 0x7Fu) | (uint32_t)sign);  /* bit7 = signe */
    out[2] = (uint8_t)((mant >> 16) & 0xFFu);
    out[3] = (uint8_t)((mant >> 8) & 0xFFu);
    out[4] = (uint8_t)(mant & 0xFFu);
    return true;
}

/* ── Handler principal $A9 ──────────────────────────────────────────────────
 * Calculs bornés (quelques µs) → synchrones en un appel (sûr : sur le vrai LOCI
 * le service du bus est sur core 1, indépendant). */
void op_math(loci_t* loci) {
    if (!loci->coproc_enabled) {                      /* opt-in : sinon inerte */
        api_return_errno(loci, LOCI_ENOSYS);
        return;
    }
    uint8_t sub = loci->regs[LOCI_REG_API_A];

    switch (sub) {
    /* ── Entiers ──────────────────────────────────────────────────────── */
    case MATH_MUL_U16: {
        uint16_t b, a;
        if (!pop_u16(loci, &b) || !pop_u16(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        api_return_axsreg(loci, (uint32_t)a * (uint32_t)b);
        return;
    }
    case MATH_MUL_I16: {
        int16_t b, a;
        if (!pop_i16(loci, &b) || !pop_i16(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        api_return_axsreg(loci, (uint32_t)((int32_t)a * (int32_t)b));
        return;
    }
    case MATH_DIVMOD_U16: {
        uint16_t b, a;
        if (!pop_u16(loci, &b) || !pop_u16(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (b == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        return_pair_u16(loci, (uint16_t)(a / b), (uint16_t)(a % b));  /* quotient, reste */
        return;
    }
    case MATH_DIVMOD_I16: {
        int16_t b, a;
        if (!pop_i16(loci, &b) || !pop_i16(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (b == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        int16_t q = (int16_t)(a / b), r = (int16_t)(a % b);
        return_pair_u16(loci, (uint16_t)q, (uint16_t)r);
        return;
    }
    case MATH_MUL_U32: {
        uint32_t b, a;
        if (!pop_u32(loci, &b) || !pop_u32(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        api_return_axsreg(loci, a * b);              /* tronqué 32 bits (spec) */
        return;
    }
    case MATH_DIVMOD_U32: {
        uint32_t b, a;
        if (!pop_u32(loci, &b) || !pop_u32(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (b == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        return_pair_u32(loci, a / b, a % b);
        return;
    }
    case MATH_DIVMOD_I32: {
        int32_t b, a;
        if (!pop_i32(loci, &b) || !pop_i32(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (b == 0) { api_return_errno(loci, LOCI_EINVAL); return; }
        return_pair_u32(loci, (uint32_t)(a / b), (uint32_t)(a % b));
        return;
    }
    /* ── Flottant IEEE754 ─────────────────────────────────────────────── */
    case MATH_FADD: { float b, a; if (!pop_f32(loci,&b)||!pop_f32(loci,&a)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, a + b); return; }
    case MATH_FSUB: { float b, a; if (!pop_f32(loci,&b)||!pop_f32(loci,&a)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, a - b); return; }
    case MATH_FMUL: { float b, a; if (!pop_f32(loci,&b)||!pop_f32(loci,&a)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, a * b); return; }
    case MATH_FDIV: { float b, a; if (!pop_f32(loci,&b)||!pop_f32(loci,&a)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, a / b); return; }
    case MATH_FCMP: {
        float b, a;
        if (!pop_f32(loci, &b) || !pop_f32(loci, &a)) { api_return_errno(loci, LOCI_EINVAL); return; }
        int8_t r = (a < b) ? -1 : (a > b) ? 1 : 0;
        api_return_axsreg(loci, (uint32_t)(int32_t)r);   /* sign-extend -1 */
        return;
    }
    case MATH_ITOF: { int32_t i; if (!pop_i32(loci,&i)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, (float)i); return; }
    case MATH_FTOI: { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} api_return_axsreg(loci, (uint32_t)(int32_t)f); return; }
    /* ── Transcendantes (libm) ────────────────────────────────────────── */
    case MATH_FSQRT: { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, sqrtf(f)); return; }
    case MATH_FSIN:  { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, sinf(f));  return; }
    case MATH_FCOS:  { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, cosf(f));  return; }
    case MATH_FTAN:  { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, tanf(f));  return; }
    case MATH_FATAN: {
        /* 1 opérande → atan(x) ; 2 opérandes → atan2(y, x). Détecté par la pile. */
        float x;
        if (!pop_f32(loci, &x)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (loci->xstack_ptr < LOCI_XSTACK_SIZE) {   /* un 2e opérande présent */
            float y = x; float x2;
            if (!pop_f32(loci, &x2)) { api_return_errno(loci, LOCI_EINVAL); return; }
            return_f32(loci, atan2f(y, x2));
        } else {
            return_f32(loci, atanf(x));
        }
        return;
    }
    case MATH_FLOG:  { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, logf(f)); return; }
    case MATH_FLOG10:{ float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, log10f(f)); return; }
    case MATH_FEXP:  { float f; if (!pop_f32(loci,&f)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, expf(f)); return; }
    case MATH_FPOW:  { float b, a; if (!pop_f32(loci,&b)||!pop_f32(loci,&a)){api_return_errno(loci,LOCI_EINVAL);return;} return_f32(loci, powf(a, b)); return; }
    /* ── Pont MBF ↔ IEEE754 ───────────────────────────────────────────── */
    case MATH_MBF_TO_IEEE: {
        uint8_t m[5];
        if (!pop_n(loci, m, 5)) { api_return_errno(loci, LOCI_EINVAL); return; }
        return_f32(loci, mbf5_to_f32(m));
        return;
    }
    case MATH_IEEE_TO_MBF: {
        float f; uint8_t m[5];
        if (!pop_f32(loci, &f)) { api_return_errno(loci, LOCI_EINVAL); return; }
        if (!f32_to_mbf5(f, m)) { api_return_errno(loci, LOCI_ERANGE); return; }  /* Inf/NaN/overflow */
        xstack_zero(loci);
        xstack_push_n(loci, m, 5);          /* 5 octets MBF, octet0 (exp) au sommet */
        api_return_ax(loci, 0);
        return;
    }
    default:
        log_debug("LOCI $A9 math: sous-code $%02X inconnu → ENOSYS", sub);
        api_return_errno(loci, LOCI_ENOSYS);
        return;
    }
}

void loci_set_coproc(loci_t* loci, bool enabled) {
    loci->coproc_enabled = enabled;
}
