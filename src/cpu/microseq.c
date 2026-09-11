/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file microseq.c
 * @brief Cœur 6502 micro-séquencé — un cycle, un accès (V2-E1)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Voir include/cpu/microseq.h pour le contrat et la raison d'être.
 *
 * Principe : au premier cycle, l'opcode est lu et on construit un **plan** de
 * micro-opérations — une par cycle restant. Chaque appel de cpu_cycle()
 * exécute une micro-op, donc émet exactement un accès bus (ou un cycle interne
 * explicite). Les accès factices du NMOS sont des micro-ops de plein droit :
 * c'est ce qui distingue ce moteur du moteur historique.
 *
 * La sémantique de calcul n'est PAS réimplémentée ici : elle vient de
 * cpu_rmw_apply / cpu_op_adc / cpu_op_sbc / cpu_op_cmp / cpu_op_lax /
 * cpu_sh_unstable (opcodes.c). Un seul endroit où réside la vérité.
 */

#include "cpu/microseq.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include <string.h>

/* ─── Micro-opérations (une par cycle) ─── */
enum {
    M_END = 0,
    M_FETCH_LO,     /* ptr/adl = read(PC++) */
    M_FETCH_HI,     /* adh = read(PC++) ; addr = adh:adl ; applique l'index */
    M_DUMMY_PC,     /* read(PC), sans incrément — cycle interne du NMOS */
    M_ZP_INDEX,     /* read(ptr) FACTICE, puis ptr += X ou Y (reste 8 bits) */
    M_PTR_LO,       /* adl = read(ptr) */
    M_PTR_HI,       /* adh = read(ptr+1 sur 8 bits) ; addr = adh:adl ; index */
    M_DUMMY_UNFIXED,/* read à l'adresse NON corrigée (factice d'indexation) */
    M_READ_FIXUP,   /* lecture : factice non corrigée si traversée, sinon rien */
    M_READ_DATA,    /* data = read(addr) puis ALU de lecture */
    M_WRITE_DATA,   /* write(addr, valeur du store) */
    M_RMW_ORIG,     /* write(addr, valeur lue) — écriture-retour NMOS */
    M_RMW_NEW,      /* applique l'opération RMW puis write(addr, résultat) */
    M_IMPLIED,      /* read(PC) factice + opération sur registres */
    M_IMM,          /* data = read(PC++) puis ALU de lecture */
    M_BRANCH_TAKEN, /* read(PC) factice ; applique l'octet bas du saut */
    M_BRANCH_PAGE,  /* read à l'adresse partiellement corrigée ; corrige PCH */
    M_JMP_HI,       /* adh = read(PC++) ; PC = adh:adl */
    M_JMP_IND_LO,   /* adl = read(ptr16) */
    M_JMP_IND_HI,   /* adh = read(ptr16 avec bug de page) ; PC = adh:adl */
    M_STACK_DUMMY,  /* read($0100+SP) factice */
    M_PUSH_PCH, M_PUSH_PCL, M_PUSH_P, M_PUSH_P_BRK, M_PUSH_A,
    M_PULL_PCL, M_PULL_PCH, M_PULL_P, M_PULL_A,
    M_RTS_FIXUP,    /* read(PC) factice puis PC++ (RTS) */
    M_JSR_HI,       /* adh = read(PC++) ; PC = adh:adl (après les empilements) */
    M_BRK_SIGN,     /* read(PC++) : l'octet de signature de BRK */
    M_VEC_LO,       /* PCL = read(vecteur) */
    M_VEC_HI,       /* PCH = read(vecteur+1) */
    M_JAM           /* gèle le CPU */
};

/* ─── Classes d'instruction ─── */
enum {
    C_IMPLIED = 0, C_READ, C_WRITE, C_WRITE_UNSTABLE, C_RMW,
    C_BRANCH, C_JMP, C_JMP_IND, C_JSR, C_RTS, C_RTI,
    C_PUSH_A, C_PUSH_P, C_PULL_A, C_PULL_P, C_BRK, C_JAM
};

typedef struct {
    uint8_t cls;
    uint8_t rmw;    /* cpu_rmw_t, pour C_RMW */
} ms_info_t;

/* Vecteur d'interruption en cours (NMI = $FFFA, IRQ/BRK = $FFFE) */
static uint16_t ms_vector;

/* ════════════════════════════════════════════════════════════════════
 *  Classement des 256 opcodes
 *
 *  Le MODE d'adressage vient d'opcode_table (opcodes.c) — une seule source.
 *  Ici on ne donne que la CLASSE (quelle forme de séquence) et, pour les RMW,
 *  quelle opération appliquer.
 * ══════════════════════════════════════════════════════════════════ */
static ms_info_t ms_table[256];
static bool ms_table_ready = false;

static void ms_set(uint8_t op, uint8_t cls, uint8_t rmw) {
    ms_table[op].cls = cls;
    ms_table[op].rmw = rmw;
}

static void ms_build_table(void) {
    /* Par défaut : implicite (couvre CLC/SEC/TAX/NOP/ASL A… et sert de repli) */
    for (int i = 0; i < 256; i++) ms_set((uint8_t)i, C_IMPLIED, 0);

    /* ─── Lectures (l'opérande est lu, puis replié dans un registre/des drapeaux) */
    static const uint8_t reads[] = {
        /* ORA */ 0x09,0x05,0x15,0x0D,0x1D,0x19,0x01,0x11,
        /* AND */ 0x29,0x25,0x35,0x2D,0x3D,0x39,0x21,0x31,
        /* EOR */ 0x49,0x45,0x55,0x4D,0x5D,0x59,0x41,0x51,
        /* ADC */ 0x69,0x65,0x75,0x6D,0x7D,0x79,0x61,0x71,
        /* SBC */ 0xE9,0xEB,0xE5,0xF5,0xED,0xFD,0xF9,0xE1,0xF1,
        /* CMP */ 0xC9,0xC5,0xD5,0xCD,0xDD,0xD9,0xC1,0xD1,
        /* CPX */ 0xE0,0xE4,0xEC,
        /* CPY */ 0xC0,0xC4,0xCC,
        /* LDA */ 0xA9,0xA5,0xB5,0xAD,0xBD,0xB9,0xA1,0xB1,
        /* LDX */ 0xA2,0xA6,0xB6,0xAE,0xBE,
        /* LDY */ 0xA0,0xA4,0xB4,0xAC,0xBC,
        /* BIT */ 0x24,0x2C,
        /* LAX */ 0xA7,0xB7,0xAF,0xBF,0xA3,0xB3,
        /* immédiats « magiques » */ 0x0B,0x2B,0x4B,0x6B,0x8B,0xAB,0xCB,
        /* LAS */ 0xBB,
        /* NOP à opérande (lecture factice réelle) */
        0x80,0x82,0x89,0xC2,0xE2,
        0x04,0x44,0x64,
        0x14,0x34,0x54,0x74,0xD4,0xF4,
        0x0C,
        0x1C,0x3C,0x5C,0x7C,0xDC,0xFC
    };
    for (size_t i = 0; i < sizeof(reads); i++) ms_set(reads[i], C_READ, 0);

    /* ─── Écritures */
    static const uint8_t writes[] = {
        /* STA */ 0x85,0x95,0x8D,0x9D,0x99,0x81,0x91,
        /* STX */ 0x86,0x96,0x8E,
        /* STY */ 0x84,0x94,0x8C,
        /* SAX */ 0x87,0x97,0x8F,0x83
    };
    for (size_t i = 0; i < sizeof(writes); i++) ms_set(writes[i], C_WRITE, 0);

    /* Stores instables : la destination dépend de la valeur en cas de traversée */
    static const uint8_t unstable[] = { 0x93,0x9B,0x9C,0x9E,0x9F };
    for (size_t i = 0; i < sizeof(unstable); i++) ms_set(unstable[i], C_WRITE_UNSTABLE, 0);

    /* ─── Lecture-modification-écriture */
    struct { uint8_t op; uint8_t rmw; } rmws[] = {
        {0x06,RMW_ASL},{0x16,RMW_ASL},{0x0E,RMW_ASL},{0x1E,RMW_ASL},
        {0x46,RMW_LSR},{0x56,RMW_LSR},{0x4E,RMW_LSR},{0x5E,RMW_LSR},
        {0x26,RMW_ROL},{0x36,RMW_ROL},{0x2E,RMW_ROL},{0x3E,RMW_ROL},
        {0x66,RMW_ROR},{0x76,RMW_ROR},{0x6E,RMW_ROR},{0x7E,RMW_ROR},
        {0xE6,RMW_INC},{0xF6,RMW_INC},{0xEE,RMW_INC},{0xFE,RMW_INC},
        {0xC6,RMW_DEC},{0xD6,RMW_DEC},{0xCE,RMW_DEC},{0xDE,RMW_DEC},
        {0x07,RMW_SLO},{0x17,RMW_SLO},{0x0F,RMW_SLO},{0x1F,RMW_SLO},{0x1B,RMW_SLO},{0x03,RMW_SLO},{0x13,RMW_SLO},
        {0x27,RMW_RLA},{0x37,RMW_RLA},{0x2F,RMW_RLA},{0x3F,RMW_RLA},{0x3B,RMW_RLA},{0x23,RMW_RLA},{0x33,RMW_RLA},
        {0x47,RMW_SRE},{0x57,RMW_SRE},{0x4F,RMW_SRE},{0x5F,RMW_SRE},{0x5B,RMW_SRE},{0x43,RMW_SRE},{0x53,RMW_SRE},
        {0x67,RMW_RRA},{0x77,RMW_RRA},{0x6F,RMW_RRA},{0x7F,RMW_RRA},{0x7B,RMW_RRA},{0x63,RMW_RRA},{0x73,RMW_RRA},
        {0xC7,RMW_DCP},{0xD7,RMW_DCP},{0xCF,RMW_DCP},{0xDF,RMW_DCP},{0xDB,RMW_DCP},{0xC3,RMW_DCP},{0xD3,RMW_DCP},
        {0xE7,RMW_ISC},{0xF7,RMW_ISC},{0xEF,RMW_ISC},{0xFF,RMW_ISC},{0xFB,RMW_ISC},{0xE3,RMW_ISC},{0xF3,RMW_ISC}
    };
    for (size_t i = 0; i < sizeof(rmws) / sizeof(rmws[0]); i++)
        ms_set(rmws[i].op, C_RMW, rmws[i].rmw);

    /* ─── Contrôle et pile */
    static const uint8_t branches[] = { 0x10,0x30,0x50,0x70,0x90,0xB0,0xD0,0xF0 };
    for (size_t i = 0; i < sizeof(branches); i++) ms_set(branches[i], C_BRANCH, 0);
    ms_set(0x4C, C_JMP, 0);
    ms_set(0x6C, C_JMP_IND, 0);
    ms_set(0x20, C_JSR, 0);
    ms_set(0x60, C_RTS, 0);
    ms_set(0x40, C_RTI, 0);
    ms_set(0x00, C_BRK, 0);
    ms_set(0x48, C_PUSH_A, 0);
    ms_set(0x08, C_PUSH_P, 0);
    ms_set(0x68, C_PULL_A, 0);
    ms_set(0x28, C_PULL_P, 0);

    static const uint8_t jams[] = { 0x02,0x12,0x22,0x32,0x42,0x52,
                                    0x62,0x72,0x92,0xB2,0xD2,0xF2 };
    for (size_t i = 0; i < sizeof(jams); i++) ms_set(jams[i], C_JAM, 0);

    ms_table_ready = true;
}

/* ════════════════════════════════════════════════════════════════════
 *  Construction du plan
 * ══════════════════════════════════════════════════════════════════ */

static void plan_add(cpu6502_t* cpu, uint8_t mop) {
    if (cpu->ms_len < sizeof(cpu->ms_plan)) cpu->ms_plan[cpu->ms_len++] = mop;
}

/* Phase d'adressage commune. `dummy_always` : les écritures et les RMW paient
 * TOUJOURS le cycle factice d'indexation, les lectures seulement en cas de
 * traversée de page (micro-op conditionnelle M_READ_FIXUP). */
static void plan_addressing(cpu6502_t* cpu, addressing_mode_t mode, bool dummy_always) {
    switch (mode) {
    case ADDR_IMMEDIATE:
        break;                                   /* l'opérande est lu par M_IMM */
    case ADDR_ZERO_PAGE:
        plan_add(cpu, M_FETCH_LO);
        break;
    case ADDR_ZERO_PAGE_X:
    case ADDR_ZERO_PAGE_Y:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_ZP_INDEX);
        break;
    case ADDR_ABSOLUTE:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_FETCH_HI);
        break;
    case ADDR_ABSOLUTE_X:
    case ADDR_ABSOLUTE_Y:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_FETCH_HI);
        plan_add(cpu, dummy_always ? M_DUMMY_UNFIXED : M_READ_FIXUP);
        break;
    case ADDR_INDEXED_INDIRECT:                  /* ($nn,X) */
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_ZP_INDEX);
        plan_add(cpu, M_PTR_LO);
        plan_add(cpu, M_PTR_HI);
        break;
    case ADDR_INDIRECT_INDEXED:                  /* ($nn),Y */
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_PTR_LO);
        plan_add(cpu, M_PTR_HI);
        plan_add(cpu, dummy_always ? M_DUMMY_UNFIXED : M_READ_FIXUP);
        break;
    default:
        break;
    }
}

static void ms_build_plan(cpu6502_t* cpu, uint8_t opcode) {
    if (!ms_table_ready) ms_build_table();
    const ms_info_t* info = &ms_table[opcode];
    addressing_mode_t mode = opcode_table[opcode].mode;

    cpu->ms_len = 0;
    cpu->ms_pc = 0;
    cpu->ms_opcode = opcode;
    cpu->ms_rmw = info->rmw;
    cpu->ms_crossed = false;
    cpu->ms_idx = 0;
    if (mode == ADDR_ABSOLUTE_X || mode == ADDR_ZERO_PAGE_X ||
        mode == ADDR_INDEXED_INDIRECT)
        cpu->ms_idx = 1;
    else if (mode == ADDR_ABSOLUTE_Y || mode == ADDR_ZERO_PAGE_Y ||
             mode == ADDR_INDIRECT_INDEXED)
        cpu->ms_idx = 2;

    switch (info->cls) {
    case C_READ:
        if (mode == ADDR_IMMEDIATE) {
            plan_add(cpu, M_IMM);
        } else {
            plan_addressing(cpu, mode, false);
            plan_add(cpu, M_READ_DATA);
        }
        break;
    case C_WRITE:
    case C_WRITE_UNSTABLE:
        plan_addressing(cpu, mode, true);
        plan_add(cpu, M_WRITE_DATA);
        break;
    case C_RMW:
        if (mode == ADDR_ACCUMULATOR) {
            plan_add(cpu, M_IMPLIED);
        } else {
            plan_addressing(cpu, mode, true);
            plan_add(cpu, M_READ_DATA);
            plan_add(cpu, M_RMW_ORIG);
            plan_add(cpu, M_RMW_NEW);
        }
        break;
    case C_BRANCH:
        plan_add(cpu, M_FETCH_LO);               /* l'offset */
        plan_add(cpu, M_BRANCH_TAKEN);           /* sauté si la condition est fausse */
        plan_add(cpu, M_BRANCH_PAGE);            /* sauté s'il n'y a pas de traversée */
        break;
    case C_JMP:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_JMP_HI);
        break;
    case C_JMP_IND:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_FETCH_HI);
        plan_add(cpu, M_JMP_IND_LO);
        plan_add(cpu, M_JMP_IND_HI);
        break;
    case C_JSR:
        plan_add(cpu, M_FETCH_LO);
        plan_add(cpu, M_STACK_DUMMY);
        plan_add(cpu, M_PUSH_PCH);
        plan_add(cpu, M_PUSH_PCL);
        plan_add(cpu, M_JSR_HI);
        break;
    case C_RTS:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_STACK_DUMMY);
        plan_add(cpu, M_PULL_PCL);
        plan_add(cpu, M_PULL_PCH);
        plan_add(cpu, M_RTS_FIXUP);
        break;
    case C_RTI:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_STACK_DUMMY);
        plan_add(cpu, M_PULL_P);
        plan_add(cpu, M_PULL_PCL);
        plan_add(cpu, M_PULL_PCH);
        break;
    case C_PUSH_A:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_PUSH_A);
        break;
    case C_PUSH_P:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_PUSH_P_BRK);             /* PHP empile B à 1 */
        break;
    case C_PULL_A:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_STACK_DUMMY);
        plan_add(cpu, M_PULL_A);
        break;
    case C_PULL_P:
        plan_add(cpu, M_DUMMY_PC);
        plan_add(cpu, M_STACK_DUMMY);
        plan_add(cpu, M_PULL_P);
        break;
    case C_BRK:
        plan_add(cpu, M_BRK_SIGN);
        plan_add(cpu, M_PUSH_PCH);
        plan_add(cpu, M_PUSH_PCL);
        plan_add(cpu, M_PUSH_P_BRK);
        plan_add(cpu, M_VEC_LO);
        plan_add(cpu, M_VEC_HI);
        ms_vector = 0xFFFE;
        break;
    case C_JAM:
        plan_add(cpu, M_JAM);
        break;
    case C_IMPLIED:
    default:
        plan_add(cpu, M_IMPLIED);
        break;
    }
}

/* Séquence d'interruption matérielle : 7 cycles (2 cycles internes, 3
 * empilements, 2 lectures de vecteur). */
static void ms_build_interrupt(cpu6502_t* cpu, uint16_t vector) {
    cpu->ms_len = 0;
    cpu->ms_pc = 0;
    cpu->ms_opcode = 0;
    cpu->ms_crossed = false;
    ms_vector = vector;
    cpu->ms_base = cpu->PC;   /* PC d'avant l'interruption, pour --trace-irq */
    /* 7 cycles au total : le premier (lecture morte) est émis par le bloc de
     * démarrage de cpu_cycle(), ce plan porte les 6 suivants. */
    plan_add(cpu, M_DUMMY_PC);
    plan_add(cpu, M_PUSH_PCH);
    plan_add(cpu, M_PUSH_PCL);
    plan_add(cpu, M_PUSH_P);
    plan_add(cpu, M_VEC_LO);
    plan_add(cpu, M_VEC_HI);
}

/* ════════════════════════════════════════════════════════════════════
 *  Sémantique : repliement de l'opérande, valeur des stores, implicites
 *  (le calcul vient d'opcodes.c — ici on ne fait que router)
 * ══════════════════════════════════════════════════════════════════ */

static void ms_alu_read(cpu6502_t* cpu, uint8_t op, uint8_t v) {
    switch (op) {
    /* ORA / AND / EOR */
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19:
    case 0x01: case 0x11:
        cpu->A |= v; cpu_update_nz(cpu, cpu->A); break;
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39:
    case 0x21: case 0x31:
        cpu->A &= v; cpu_update_nz(cpu, cpu->A); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59:
    case 0x41: case 0x51:
        cpu->A ^= v; cpu_update_nz(cpu, cpu->A); break;
    /* ADC / SBC */
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79:
    case 0x61: case 0x71:
        cpu_op_adc(cpu, v); break;
    case 0xE9: case 0xEB: case 0xE5: case 0xF5: case 0xED: case 0xFD:
    case 0xF9: case 0xE1: case 0xF1:
        cpu_op_sbc(cpu, v); break;
    /* Comparaisons */
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9:
    case 0xC1: case 0xD1:
        cpu_op_cmp(cpu, cpu->A, v); break;
    case 0xE0: case 0xE4: case 0xEC: cpu_op_cmp(cpu, cpu->X, v); break;
    case 0xC0: case 0xC4: case 0xCC: cpu_op_cmp(cpu, cpu->Y, v); break;
    /* Chargements */
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9:
    case 0xA1: case 0xB1:
        cpu->A = v; cpu_update_nz(cpu, v); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        cpu->X = v; cpu_update_nz(cpu, v); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        cpu->Y = v; cpu_update_nz(cpu, v); break;
    /* BIT */
    case 0x24: case 0x2C:
        cpu_set_flag(cpu, FLAG_ZERO, (cpu->A & v) == 0);
        cpu_set_flag(cpu, FLAG_OVERFLOW, (v & 0x40) != 0);
        cpu_set_flag(cpu, FLAG_NEGATIVE, (v & 0x80) != 0);
        break;
    /* LAX */
    case 0xA7: case 0xB7: case 0xAF: case 0xBF: case 0xA3: case 0xB3:
        cpu_op_lax(cpu, v); break;
    /* ANC : AND puis C = N */
    case 0x0B: case 0x2B:
        cpu->A &= v; cpu_update_nz(cpu, cpu->A);
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x80) != 0);
        break;
    /* ALR : AND puis LSR A */
    case 0x4B:
        cpu->A &= v;
        cpu_set_flag(cpu, FLAG_CARRY, (cpu->A & 0x01) != 0);
        cpu->A = (uint8_t)(cpu->A >> 1);
        cpu_update_nz(cpu, cpu->A);
        break;
    /* ARR : AND puis ROR, avec la correction BCD du NMOS */
    case 0x6B: {
        uint8_t t = (uint8_t)(cpu->A & v);
        uint8_t c = cpu_get_flag(cpu, FLAG_CARRY) ? 0x80 : 0;
        uint8_t r = (uint8_t)((t >> 1) | c);
        cpu_update_nz(cpu, r);
        cpu_set_flag(cpu, FLAG_OVERFLOW, ((r ^ t) & 0x40) != 0);
        if (cpu_get_flag(cpu, FLAG_DECIMAL)) {
            if (((t & 0x0F) + (t & 0x01)) > 0x05)
                r = (uint8_t)((r & 0xF0) | ((r + 0x06) & 0x0F));
            if (((t & 0xF0) + (t & 0x10)) > 0x50) {
                r = (uint8_t)(r + 0x60);
                cpu_set_flag(cpu, FLAG_CARRY, true);
            } else {
                cpu_set_flag(cpu, FLAG_CARRY, false);
            }
        } else {
            cpu_set_flag(cpu, FLAG_CARRY, (t & 0x80) != 0);
        }
        cpu->A = r;
        break;
    }
    /* ANE/XAA : A = (A | magie) & X & imm — la « magie » vaut $EE en pratique */
    case 0x8B:
        cpu->A = (uint8_t)((cpu->A | 0xEE) & cpu->X & v);
        cpu_update_nz(cpu, cpu->A);
        break;
    /* LXA/ATX : A = X = (A | magie) & imm */
    case 0xAB:
        cpu->A = (uint8_t)((cpu->A | 0xEE) & v);
        cpu->X = cpu->A;
        cpu_update_nz(cpu, cpu->A);
        break;
    /* SBX/AXS : X = (A & X) - imm, C comme une comparaison */
    case 0xCB: {
        uint8_t tmp = (uint8_t)(cpu->A & cpu->X);
        cpu_set_flag(cpu, FLAG_CARRY, tmp >= v);
        cpu->X = (uint8_t)(tmp - v);
        cpu_update_nz(cpu, cpu->X);
        break;
    }
    /* LAS/LAR : A = X = SP = mem & SP */
    case 0xBB: {
        uint8_t r = (uint8_t)(v & cpu->SP);
        cpu->A = r; cpu->X = r; cpu->SP = r;
        cpu_update_nz(cpu, r);
        break;
    }
    default:
        break;   /* NOP à opérande : la lecture a eu lieu, rien d'autre */
    }
}

static uint8_t ms_store_value(const cpu6502_t* cpu, uint8_t op) {
    switch (op) {
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99:
    case 0x81: case 0x91: return cpu->A;
    case 0x86: case 0x96: case 0x8E: return cpu->X;
    case 0x84: case 0x94: case 0x8C: return cpu->Y;
    case 0x87: case 0x97: case 0x8F: case 0x83: return (uint8_t)(cpu->A & cpu->X);
    default: return cpu->A;
    }
}

static void ms_implied(cpu6502_t* cpu, uint8_t op) {
    switch (op) {
    case 0x18: cpu_set_flag(cpu, FLAG_CARRY, false); break;       /* CLC */
    case 0x38: cpu_set_flag(cpu, FLAG_CARRY, true); break;        /* SEC */
    case 0x58: cpu_set_flag(cpu, FLAG_INTERRUPT, false); break;   /* CLI */
    case 0x78: cpu_set_flag(cpu, FLAG_INTERRUPT, true); break;    /* SEI */
    case 0xB8: cpu_set_flag(cpu, FLAG_OVERFLOW, false); break;    /* CLV */
    case 0xD8: cpu_set_flag(cpu, FLAG_DECIMAL, false); break;     /* CLD */
    case 0xF8: cpu_set_flag(cpu, FLAG_DECIMAL, true); break;      /* SED */
    case 0xAA: cpu->X = cpu->A; cpu_update_nz(cpu, cpu->X); break;  /* TAX */
    case 0x8A: cpu->A = cpu->X; cpu_update_nz(cpu, cpu->A); break;  /* TXA */
    case 0xA8: cpu->Y = cpu->A; cpu_update_nz(cpu, cpu->Y); break;  /* TAY */
    case 0x98: cpu->A = cpu->Y; cpu_update_nz(cpu, cpu->A); break;  /* TYA */
    case 0xBA: cpu->X = cpu->SP; cpu_update_nz(cpu, cpu->X); break; /* TSX */
    case 0x9A: cpu->SP = cpu->X; break;                             /* TXS */
    case 0xE8: cpu->X++; cpu_update_nz(cpu, cpu->X); break;         /* INX */
    case 0xCA: cpu->X--; cpu_update_nz(cpu, cpu->X); break;         /* DEX */
    case 0xC8: cpu->Y++; cpu_update_nz(cpu, cpu->Y); break;         /* INY */
    case 0x88: cpu->Y--; cpu_update_nz(cpu, cpu->Y); break;         /* DEY */
    case 0x0A: cpu->A = cpu_rmw_apply(cpu, RMW_ASL, cpu->A); break; /* ASL A */
    case 0x4A: cpu->A = cpu_rmw_apply(cpu, RMW_LSR, cpu->A); break; /* LSR A */
    case 0x2A: cpu->A = cpu_rmw_apply(cpu, RMW_ROL, cpu->A); break; /* ROL A */
    case 0x6A: cpu->A = cpu_rmw_apply(cpu, RMW_ROR, cpu->A); break; /* ROR A */
    default: break;                                                 /* NOP implicites */
    }
}

static bool ms_branch_taken(const cpu6502_t* cpu, uint8_t op) {
    switch (op) {
    case 0x10: return !cpu_get_flag(cpu, FLAG_NEGATIVE);
    case 0x30: return  cpu_get_flag(cpu, FLAG_NEGATIVE);
    case 0x50: return !cpu_get_flag(cpu, FLAG_OVERFLOW);
    case 0x70: return  cpu_get_flag(cpu, FLAG_OVERFLOW);
    case 0x90: return !cpu_get_flag(cpu, FLAG_CARRY);
    case 0xB0: return  cpu_get_flag(cpu, FLAG_CARRY);
    case 0xD0: return !cpu_get_flag(cpu, FLAG_ZERO);
    case 0xF0: return  cpu_get_flag(cpu, FLAG_ZERO);
    default: return false;
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Exécution d'un cycle
 * ══════════════════════════════════════════════════════════════════ */

static void ms_apply_index(cpu6502_t* cpu) {
    uint8_t idx = (cpu->ms_idx == 1) ? cpu->X : (cpu->ms_idx == 2) ? cpu->Y : 0;
    cpu->ms_base = cpu->ms_addr;
    cpu->ms_addr = (uint16_t)(cpu->ms_addr + idx);
    cpu->ms_crossed = ((cpu->ms_base & 0xFF00) != (cpu->ms_addr & 0xFF00));
}

static uint16_t ms_unfixed(const cpu6502_t* cpu) {
    return (uint16_t)((cpu->ms_base & 0xFF00) | (cpu->ms_addr & 0x00FF));
}

/* Détournement par la NMI : si /NMI tombe avant le cycle qui empile P d'un BRK
 * ou d'une séquence d'IRQ, c'est le vecteur NMI qui est lu — l'interruption
 * basse priorité est « détournée ». Le drapeau B empilé reste celui de la
 * séquence d'origine (BRK garde son B à 1). */
static void ms_nmi_hijack(cpu6502_t* cpu) {
    if (cpu->nmi_pending && ms_vector != 0xFFFA) {
        ms_vector = 0xFFFA;
        cpu->nmi_pending = false;
        cpu->ms_nmi_sampled = false;
    }
}

void cpu_set_microseq(cpu6502_t* cpu, bool enabled) {
    cpu->ms_enabled = enabled;
    cpu->ms_active = false;
    if (!ms_table_ready) ms_build_table();
}

bool cpu_microseq_enabled(const cpu6502_t* cpu) {
    return cpu->ms_enabled;
}

/* Échantillonne /NMI et /IRQ à la fin d'un cycle. Le dernier cycle d'une
 * instruction n'échantillonne PAS : la décision qui suit doit se fonder sur
 * l'état du cycle pénultième (US1.3). */
static void ms_sample_interrupts(cpu6502_t* cpu, bool last_cycle) {
    if (last_cycle) return;
    cpu->ms_nmi_sampled = cpu->nmi_pending;
    cpu->ms_irq_sampled = (cpu->irq || cpu->irq_pulse) &&
                          !cpu_get_flag(cpu, FLAG_INTERRUPT);
}

bool cpu_cycle(cpu6502_t* cpu) {
    if (cpu->halted) return true;

    /* ─── Premier cycle : interruption ou lecture de l'opcode ─── */
    if (!cpu->ms_active) {
        /* Décision prise sur l'échantillon du cycle PÉNULTIÈME de l'instruction
         * précédente, pas sur l'état courant des lignes. */
        if (cpu->ms_nmi_sampled && cpu->nmi_pending) {
            cpu->nmi_pending = false;
            cpu->ms_nmi_sampled = false;
            ms_build_interrupt(cpu, 0xFFFA);
            (void)cpu_mem_read(cpu, cpu->PC);    /* cycle 1 : lecture morte */
            cpu->ms_active = true;
            ms_sample_interrupts(cpu, false);
            return false;
        }
        /* Noter l'absence de test sur FLAG_INTERRUPT : le masque a déjà été
         * pris en compte au moment de l'échantillonnage (cycle pénultième). Le
         * retester ici ferait protéger l'instruction suivante par un SEI, ce que
         * le matériel ne fait pas. */
        if (cpu->ms_irq_sampled) {
            if (cpu->irq_pulse) cpu->irq_pulse--;
            cpu->ms_irq_sampled = false;
            ms_build_interrupt(cpu, 0xFFFE);
            (void)cpu_mem_read(cpu, cpu->PC);
            cpu->ms_active = true;
            ms_sample_interrupts(cpu, false);
            return false;
        }
        uint8_t opcode = cpu_fetch_byte(cpu);    /* cycle 1 : fetch opcode */
        ms_build_plan(cpu, opcode);
        cpu->ms_active = true;
        /* Une instruction d'un seul cycle n'existe pas : ce cycle n'est jamais
         * le dernier, on échantillonne donc toujours. */
        ms_sample_interrupts(cpu, false);
        return false;
    }

    uint8_t mop = cpu->ms_plan[cpu->ms_pc];
    uint8_t op  = cpu->ms_opcode;
    bool    last;

    switch (mop) {
    case M_FETCH_LO:
        cpu->ms_ptr = cpu_fetch_byte(cpu);
        cpu->ms_adl = cpu->ms_ptr;
        cpu->ms_addr = cpu->ms_ptr;              /* utile en page zéro */
        cpu->ms_base = cpu->ms_addr;
        /* Branchement non pris : l'instruction fait 2 cycles et CELUI-CI est le
         * dernier — la décision se prend ici, pas au cycle suivant. Décider un
         * cycle plus tard coûtait un appel à cpu_cycle() sans accès bus : le
         * compteur CPU restait juste (l'oracle ne voyait rien) mais l'horloge
         * maître avait avancé d'un cycle de plus — l'ULA prenait ~410 cycles
         * d'avance par trame sur le CPU et le VIA (V2-E7, US7.3). */
        if (ms_table[op].cls == C_BRANCH && !ms_branch_taken(cpu, op)) {
            cpu->ms_active = false;
            ms_sample_interrupts(cpu, true);
            return true;
        }
        break;
    case M_FETCH_HI:
        cpu->ms_addr = (uint16_t)((cpu_fetch_byte(cpu) << 8) | cpu->ms_adl);
        ms_apply_index(cpu);
        break;
    case M_ZP_INDEX: {
        (void)cpu_mem_read(cpu, cpu->ms_ptr);    /* lecture FACTICE à la base */
        uint8_t idx = (cpu->ms_idx == 1) ? cpu->X : cpu->Y;
        cpu->ms_ptr = (uint8_t)(cpu->ms_ptr + idx);
        cpu->ms_addr = cpu->ms_ptr;              /* reste en page zéro */
        cpu->ms_base = cpu->ms_addr;
        cpu->ms_crossed = false;
        break;
    }
    case M_PTR_LO:
        cpu->ms_adl = cpu_mem_read(cpu, cpu->ms_ptr);
        break;
    case M_PTR_HI:
        cpu->ms_addr = (uint16_t)((cpu_mem_read(cpu, (uint8_t)(cpu->ms_ptr + 1)) << 8)
                                  | cpu->ms_adl);
        if (cpu->ms_idx == 2) ms_apply_index(cpu);
        else { cpu->ms_base = cpu->ms_addr; cpu->ms_crossed = false; }
        break;
    case M_DUMMY_UNFIXED:
        (void)cpu_mem_read(cpu, ms_unfixed(cpu));
        break;
    case M_READ_FIXUP:
        /* Lecture indexée : le cycle n'existe QUE si la page a été franchie ;
         * sinon on enchaîne immédiatement sur la vraie lecture. */
        if (cpu->ms_crossed) {
            (void)cpu_mem_read(cpu, ms_unfixed(cpu));
            break;
        }
        cpu->ms_pc++;                            /* pas de cycle : passe à M_READ_DATA */
        return cpu_cycle(cpu);
    case M_READ_DATA:
        cpu->ms_data = cpu_mem_read(cpu, cpu->ms_addr);
        if (ms_table[op].cls == C_READ) ms_alu_read(cpu, op, cpu->ms_data);
        break;
    case M_WRITE_DATA:
        if (ms_table[op].cls == C_WRITE_UNSTABLE) {
            uint8_t reg = (op == 0x9C) ? cpu->Y :
                          (op == 0x9E) ? cpu->X :
                          (op == 0x9B) ? (cpu->SP = (uint8_t)(cpu->A & cpu->X))
                                       : (uint8_t)(cpu->A & cpu->X);
            uint8_t value; uint16_t target;
            cpu_sh_unstable(cpu->ms_base, cpu->ms_addr, reg, &value, &target);
            cpu_mem_write(cpu, target, value);
        } else {
            cpu_mem_write(cpu, cpu->ms_addr, ms_store_value(cpu, op));
        }
        break;
    case M_RMW_ORIG:
        cpu_mem_write(cpu, cpu->ms_addr, cpu->ms_data);   /* écriture-retour NMOS */
        break;
    case M_RMW_NEW:
        cpu->ms_data = cpu_rmw_apply(cpu, (cpu_rmw_t)cpu->ms_rmw, cpu->ms_data);
        cpu_mem_write(cpu, cpu->ms_addr, cpu->ms_data);
        break;
    case M_IMM:
        cpu->ms_data = cpu_fetch_byte(cpu);
        ms_alu_read(cpu, op, cpu->ms_data);
        break;
    case M_IMPLIED:
        (void)cpu_mem_read(cpu, cpu->PC);        /* lecture morte, PC inchangé */
        ms_implied(cpu, op);
        break;
    case M_BRANCH_TAKEN:                         /* la condition est vraie (cf. M_FETCH_LO) */
        (void)cpu_mem_read(cpu, cpu->PC);        /* lecture morte de l'opcode suivant */
        {
            uint16_t target = (uint16_t)(cpu->PC + (int8_t)cpu->ms_ptr);
            cpu->ms_crossed = ((cpu->PC & 0xFF00) != (target & 0xFF00));
            cpu->ms_base = cpu->PC;
            cpu->PC = (uint16_t)((cpu->PC & 0xFF00) | (target & 0x00FF));
            cpu->ms_addr = target;
        }
        if (!cpu->ms_crossed) {
            cpu->ms_active = false;              /* 3 cycles */
            ms_sample_interrupts(cpu, true);
            return true;
        }
        break;
    case M_BRANCH_PAGE:
        (void)cpu_mem_read(cpu, cpu->PC);        /* lecture à l'adresse mal corrigée */
        cpu->PC = cpu->ms_addr;
        break;
    case M_JMP_HI:
        cpu->PC = (uint16_t)((cpu_fetch_byte(cpu) << 8) | cpu->ms_adl);
        break;
    case M_JMP_IND_LO:
        cpu->ms_data = cpu_mem_read(cpu, cpu->ms_addr);
        break;
    case M_JMP_IND_HI: {
        /* Bug de page du 6502 : l'octet haut est lu dans la MÊME page. */
        uint16_t hi_addr = (uint16_t)((cpu->ms_addr & 0xFF00) |
                                      ((cpu->ms_addr + 1) & 0x00FF));
        cpu->PC = (uint16_t)((cpu_mem_read(cpu, hi_addr) << 8) | cpu->ms_data);
        break;
    }
    case M_DUMMY_PC:
        (void)cpu_mem_read(cpu, cpu->PC);
        break;
    case M_BRK_SIGN:
        (void)cpu_fetch_byte(cpu);               /* l'octet de signature, ignoré */
        break;
    case M_STACK_DUMMY:
        (void)cpu_mem_read(cpu, (uint16_t)(0x0100 + cpu->SP));
        break;
    case M_PUSH_PCH:
        cpu_mem_write(cpu, (uint16_t)(0x0100 + cpu->SP), (uint8_t)(cpu->PC >> 8));
        cpu->SP--;
        break;
    case M_PUSH_PCL:
        cpu_mem_write(cpu, (uint16_t)(0x0100 + cpu->SP), (uint8_t)(cpu->PC & 0xFF));
        cpu->SP--;
        break;
    case M_PUSH_P:
        cpu_mem_write(cpu, (uint16_t)(0x0100 + cpu->SP),
                      (uint8_t)((cpu->P & ~FLAG_BREAK) | FLAG_UNUSED));
        cpu->SP--;
        cpu_set_flag(cpu, FLAG_INTERRUPT, true);
        ms_nmi_hijack(cpu);
        break;
    case M_PUSH_P_BRK:
        cpu_mem_write(cpu, (uint16_t)(0x0100 + cpu->SP),
                      (uint8_t)(cpu->P | FLAG_BREAK | FLAG_UNUSED));
        cpu->SP--;
        if (op == 0x00) {                          /* BRK, pas PHP */
            cpu_set_flag(cpu, FLAG_INTERRUPT, true);
            ms_nmi_hijack(cpu);
        }
        break;
    case M_PUSH_A:
        cpu_mem_write(cpu, (uint16_t)(0x0100 + cpu->SP), cpu->A);
        cpu->SP--;
        break;
    case M_PULL_A:
        cpu->SP++;
        cpu->A = cpu_mem_read(cpu, (uint16_t)(0x0100 + cpu->SP));
        cpu_update_nz(cpu, cpu->A);
        break;
    case M_PULL_P:
        cpu->SP++;
        cpu->P = (uint8_t)((cpu_mem_read(cpu, (uint16_t)(0x0100 + cpu->SP))
                            & ~FLAG_BREAK) | FLAG_UNUSED);
        break;
    case M_PULL_PCL:
        cpu->SP++;
        cpu->ms_adl = cpu_mem_read(cpu, (uint16_t)(0x0100 + cpu->SP));
        break;
    case M_PULL_PCH:
        cpu->SP++;
        cpu->PC = (uint16_t)((cpu_mem_read(cpu, (uint16_t)(0x0100 + cpu->SP)) << 8)
                             | cpu->ms_adl);
        if (op == 0x40) cpu_irq_trace_rti(cpu);   /* RTI */
        break;
    case M_RTS_FIXUP:
        (void)cpu_mem_read(cpu, cpu->PC);        /* lecture morte, puis PC++ */
        cpu->PC++;
        break;
    case M_JSR_HI:
        cpu->PC = (uint16_t)((cpu_fetch_byte(cpu) << 8) | cpu->ms_adl);
        break;
    case M_VEC_LO:
        cpu->ms_adl = cpu_mem_read(cpu, ms_vector);
        break;
    case M_VEC_HI:
        cpu->PC = (uint16_t)((cpu_mem_read(cpu, (uint16_t)(ms_vector + 1)) << 8)
                             | cpu->ms_adl);
        /* --trace-irq : même ligne que sur le moteur historique (BRK exclu,
         * qui n'est pas une interruption matérielle). */
        if (op != 0x00) cpu_irq_trace_entry(cpu, cpu->ms_base);
        break;
    case M_JAM:
        cpu->halted = true;
        cpu->ms_active = false;
        ms_sample_interrupts(cpu, true);
        return true;
    case M_END:
    default:
        break;
    }

    cpu->ms_pc++;
    last = (cpu->ms_pc >= cpu->ms_len);
    if (last) cpu->ms_active = false;
    ms_sample_interrupts(cpu, last);
    return last;
}
