/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file microseq.h
 * @brief Cœur 6502 micro-séquencé : un cycle = un état (V2-E1)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Le moteur historique (`cpu_execute_opcode`, opcodes.c) exécute une
 * instruction d'un bloc : les accès bus sortent dans le bon ordre, mais les
 * cycles internes sont rattrapés par bourrage en fin d'instruction et les accès
 * **factices** du NMOS n'existent pas. C'est le niveau **N2** (docs/ACCURACY.md).
 *
 * Ce moteur-ci décompose chaque instruction en **un plan de micro-opérations,
 * une par cycle** : chaque cycle émet exactement son accès bus — y compris les
 * accès factices (index page zéro, traversée de page, écriture-retour RMW,
 * lectures de pile mortes) — ou est un cycle interne explicite. C'est le
 * niveau **N3** visé par la V2.
 *
 * Les deux moteurs partagent la **même sémantique** : le calcul (drapeaux, BCD,
 * opcodes illégaux) vient des mêmes fonctions (`cpu_rmw_apply` & co. dans
 * opcodes.c). Ils ne diffèrent que par l'ordonnancement.
 *
 * Tant que la migration n'est pas terminée, le moteur est **opt-in**
 * (`cpu_set_microseq()`, CLI `--cpu-microseq`) : le chemin par défaut reste
 * l'historique, donc aucune régression possible. L'oracle `make test-cycle`
 * mesure les deux (`CYCLE_ENGINE=microseq`).
 */

#ifndef CPU_MICROSEQ_H
#define CPU_MICROSEQ_H

#include <stdbool.h>
#include "cpu/cpu6502.h"

/**
 * @brief Active ou désactive le moteur micro-séquencé
 *
 * À n'appeler qu'en frontière d'instruction (sinon l'instruction en cours
 * serait abandonnée). Sans effet sur la sémantique, seulement sur
 * l'ordonnancement des cycles.
 */
void cpu_set_microseq(cpu6502_t* cpu, bool enabled);

/** @brief true si le moteur micro-séquencé est actif */
bool cpu_microseq_enabled(const cpu6502_t* cpu);

/**
 * @brief Exécute EXACTEMENT un cycle CPU
 *
 * Au premier cycle d'une instruction, décide d'abord s'il faut prendre une
 * interruption (NMI/IRQ) ; sinon lit l'opcode et construit son plan.
 *
 * @return true si le cycle exécuté était le DERNIER de l'instruction (ou de la
 *         séquence d'interruption), false si l'instruction continue.
 */
bool cpu_cycle(cpu6502_t* cpu);

#endif /* CPU_MICROSEQ_H */
