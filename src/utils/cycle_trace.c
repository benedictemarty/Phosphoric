/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cycle_trace.c
 * @brief Trace bus cycle par cycle (--cycle-trace)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-10
 *
 * Voir include/utils/cycle_trace.h pour le format et ses limites au niveau N2.
 */

#include "utils/cycle_trace.h"
#include <stdio.h>

static FILE*    ct_fp = NULL;
static uint64_t ct_lines = 0;
static uint64_t ct_max = 0;

/* Accès bus en attente d'écriture : posé par cycle_trace_bus(), consommé par
 * le premier cycle de cycle_trace_cycles() — l'ordre d'appel du cœur est
 * toujours « accès puis tick » (cpu_mem_read/cpu_mem_write). */
static bool     ct_pending = false;
static uint16_t ct_addr;
static uint8_t  ct_val;
static bool     ct_write;

bool cycle_trace_open(const char* path, uint64_t max_lines) {
    ct_fp = fopen(path, "w");
    if (!ct_fp) return false;
    ct_max = max_lines;
    ct_lines = 0;
    ct_pending = false;
    fprintf(ct_fp,
            "# Phosphoric — trace bus cycle par cycle\n"
            "# T: R=lecture W=ecriture i=cycle interne (bourrage N2, sans adresse)\n"
            "# CYCLE      T ADDR  DATA PC    A  X  Y  SP P  NV-BDIZC IRQ\n");
    return true;
}

bool cycle_trace_active(void) {
    return ct_fp != NULL && (ct_max == 0 || ct_lines < ct_max);
}

void cycle_trace_bus(void* ctx, uint16_t addr, uint8_t value, bool write) {
    (void)ctx;
    if (!cycle_trace_active()) return;
    ct_pending = true;
    ct_addr = addr;
    ct_val = value;
    ct_write = write;
}

static void ct_line(const cpu6502_t* cpu, uint64_t cycle, char type) {
    uint8_t p = cpu->P;
    char flags[9];
    const char* names = "NV-BDIZC";
    for (int i = 0; i < 8; i++)
        flags[i] = (p & (0x80 >> i)) ? names[i] : '.';
    flags[8] = '\0';

    if (type == 'i') {
        fprintf(ct_fp, "%010llu i ----- --   $%04X %02X %02X %02X %02X %02X %s ",
                (unsigned long long)cycle, cpu->PC, cpu->A, cpu->X, cpu->Y,
                cpu->SP, p, flags);
    } else {
        fprintf(ct_fp, "%010llu %c $%04X $%02X $%04X %02X %02X %02X %02X %02X %s ",
                (unsigned long long)cycle, type, ct_addr, ct_val, cpu->PC,
                cpu->A, cpu->X, cpu->Y, cpu->SP, p, flags);
    }
    if (cpu->irq)      fprintf(ct_fp, "%02X\n", cpu->irq);
    else if (cpu->nmi_pending) fprintf(ct_fp, "N\n");
    else               fprintf(ct_fp, ".\n");
    ct_lines++;
}

void cycle_trace_cycles(const cpu6502_t* cpu, int cycles) {
    if (!cycle_trace_active() || cycles <= 0) return;
    /* cpu->cycles a déjà été avancé de `cycles` par cpu_tick() : le premier
     * cycle de ce lot porte donc le numéro cycles_courants - cycles. */
    uint64_t base = cpu->cycles - (uint64_t)cycles;
    int i = 0;
    if (ct_pending) {
        ct_line(cpu, base, ct_write ? 'W' : 'R');
        ct_pending = false;
        i = 1;
    }
    for (; i < cycles && cycle_trace_active(); i++)
        ct_line(cpu, base + (uint64_t)i, 'i');
}

uint64_t cycle_trace_close(void) {
    if (ct_fp) {
        fclose(ct_fp);
        ct_fp = NULL;
    }
    return ct_lines;
}
