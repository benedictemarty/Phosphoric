/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cpu_internal.h
 * @brief Internal CPU functions shared between cpu6502.c, addressing.c, opcodes.c
 */

#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu/cpu6502.h"

uint8_t cpu_mem_read(cpu6502_t* cpu, uint16_t addr);
void cpu_mem_write(cpu6502_t* cpu, uint16_t addr, uint8_t val);

/* Advance the CPU clock by `cycles` and notify the per-cycle callback (if set). */
void cpu_tick(cpu6502_t* cpu, int cycles);
uint8_t cpu_fetch_byte(cpu6502_t* cpu);
uint16_t cpu_fetch_word_pc(cpu6502_t* cpu);

uint16_t addr_immediate(cpu6502_t* cpu);
uint16_t addr_zero_page(cpu6502_t* cpu);
uint16_t addr_zero_page_x(cpu6502_t* cpu);
uint16_t addr_zero_page_y(cpu6502_t* cpu);
uint16_t addr_absolute(cpu6502_t* cpu);
uint16_t addr_absolute_x(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_absolute_y(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_indirect(cpu6502_t* cpu);
uint16_t addr_indexed_indirect(cpu6502_t* cpu);
uint16_t addr_indirect_indexed(cpu6502_t* cpu, bool* page_crossed);
uint16_t addr_relative(cpu6502_t* cpu);

void cpu_push(cpu6502_t* cpu, uint8_t val);
uint8_t cpu_pull(cpu6502_t* cpu);
void cpu_push_word(cpu6502_t* cpu, uint16_t val);
uint16_t cpu_pull_word(cpu6502_t* cpu);

/* ─── Sémantique ALU partagée entre le moteur historique (opcodes.c) et le
 * micro-séquenceur cycle par cycle (microseq.c, V2-E1).
 *
 * Une seule implémentation de chaque opération : les deux moteurs diffèrent par
 * l'ORDONNANCEMENT des accès bus, jamais par le calcul. C'est ce qui garantit
 * qu'un correctif de sémantique (p. ex. les drapeaux BCD) profite aux deux. */

/** Opérations lecture-modification-écriture (officielles et illégales) */
typedef enum {
    RMW_ASL, RMW_LSR, RMW_ROL, RMW_ROR, RMW_INC, RMW_DEC,
    RMW_SLO, RMW_RLA, RMW_SRE, RMW_RRA, RMW_DCP, RMW_ISC
} cpu_rmw_t;

/**
 * @brief Applique une opération RMW à une valeur déjà lue
 *
 * Met à jour les drapeaux (et l'accumulateur pour les formes combinées
 * illégales) et renvoie la valeur à réécrire. N'effectue AUCUN accès bus :
 * l'appelant place les cycles de lecture et d'écriture lui-même.
 */
uint8_t cpu_rmw_apply(cpu6502_t* cpu, cpu_rmw_t op, uint8_t v);

/** Met à jour N et Z d'après une valeur */
void cpu_update_nz(cpu6502_t* cpu, uint8_t val);
/** ADC / SBC (mode décimal NMOS inclus) et comparaison, sans accès bus */
void cpu_op_adc(cpu6502_t* cpu, uint8_t val);
void cpu_op_sbc(cpu6502_t* cpu, uint8_t val);
void cpu_op_cmp(cpu6502_t* cpu, uint8_t reg, uint8_t val);
/** LAX : A = X = valeur */
void cpu_op_lax(cpu6502_t* cpu, uint8_t v);
/**
 * @brief Valeur et destination réelles d'un store « instable » (SHA/SHX/SHY/SHS)
 *
 * `base` = adresse avant index, `addr` = adresse indexée corrigée, `reg` = la
 * source déjà combinée. En cas de traversée de page l'octet haut émis est la
 * valeur elle-même : la destination n'est PAS `addr`.
 */
void cpu_sh_unstable(uint16_t base, uint16_t addr, uint8_t reg,
                     uint8_t* out_value, uint16_t* out_target);

/** Traces --trace-irq, partagées par les deux moteurs */
void cpu_irq_trace_entry(cpu6502_t* cpu, uint16_t pc_before);
void cpu_irq_trace_rti(cpu6502_t* cpu);

typedef struct opcode_info_s {
    const char* name;
    uint8_t cycles;
    uint8_t size;
    addressing_mode_t mode;
} opcode_info_t;

extern const opcode_info_t opcode_table[256];
int cpu_execute_opcode(cpu6502_t* cpu, uint8_t opcode);

#endif
