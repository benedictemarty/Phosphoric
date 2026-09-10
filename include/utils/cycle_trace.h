/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cycle_trace.h
 * @brief Trace bus cycle par cycle (--cycle-trace) — instrument de la V2
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-10
 *
 * Une ligne par cycle CPU, avec le cycle, la nature de l'accès, l'adresse
 * émise, l'octet sur le bus de données et l'état des registres. Destinée à être
 * diffée contre un autre émulateur ou contre du matériel instrumenté, et à
 * servir de preuve dans les sprints de la V2 (docs/specs/V2_CYCLE_ACCURACY.md).
 *
 * Format (colonnes fixes, séparateur espace) :
 *   CYCLE      T ADDR  DATA PC    A  X  Y  SP P  FLAGS  IRQ
 *   0000000000 R $FFFC $A5  $0000 00 00 00 FD 34 ..-..I. .
 *
 *   T    = R lecture bus, W écriture bus, i cycle interne
 *   IRQ  = '.' aucune ligne active, sinon le masque source en hexa
 *
 * ⚠️ Au niveau de précision actuel (**N2**, cf. docs/ACCURACY.md), les cycles
 * marqués `i` sont les cycles internes **bourrés en fin d'instruction** : ils
 * sont comptés au bon endroit dans le total, mais un vrai 6502 y émet une
 * adresse (souvent un accès factice) que Phosphoric n'a pas encore. Quand
 * V2-E1 aura livré le cœur micro-séquencé, ces lignes porteront leur véritable
 * accès et la trace deviendra exploitable au cycle près.
 */

#ifndef CYCLE_TRACE_H
#define CYCLE_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu6502.h"

/**
 * @brief Ouvre le fichier de trace et arme la capture
 *
 * @param path      Chemin du fichier (écrasé)
 * @param max_lines Plafond de lignes (0 = sans limite)
 * @return true si le fichier est ouvert
 */
bool cycle_trace_open(const char* path, uint64_t max_lines);

/** @brief true si la capture est armée (et pas encore arrivée au plafond) */
bool cycle_trace_active(void);

/**
 * @brief Callback d'accès bus à passer à cpu_set_bus_callback()
 *
 * Mémorise l'accès ; la ligne est écrite au cycle correspondant par
 * cycle_trace_cycles(), de sorte que la numérotation des cycles reste exacte.
 */
void cycle_trace_bus(void* ctx, uint16_t addr, uint8_t value, bool write);

/**
 * @brief Avance la trace de `cycles` cycles (à appeler depuis le hook horloge)
 *
 * Écrit la ligne de l'accès bus mémorisé (s'il y en a un) puis une ligne
 * `i` par cycle interne restant.
 */
void cycle_trace_cycles(const cpu6502_t* cpu, int cycles);

/** @brief Ferme le fichier (idempotent) et renvoie le nombre de lignes écrites */
uint64_t cycle_trace_close(void);

#endif /* CYCLE_TRACE_H */
