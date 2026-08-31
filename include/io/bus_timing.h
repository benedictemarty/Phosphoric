/**
 * @file bus_timing.h
 * @brief Base de temps sous-cycle du bus d'extension Oric (modèle PHI2). Épic B, Phase 1.
 *
 * Le 6502 de l'Oric et les périphériques du port d'extension partagent un bus
 * **asynchrone** cadencé par PHI2. À l'échelle du cycle entier (ce que modélise
 * `cpu_step`/`cpu_tick`), tous les accès « réussissent » : la lecture 6502 et la
 * réponse du périphérique tombent dans le même cycle. Mais certains conflits sont
 * des phénomènes **sous-cycle** : la donnée doit être **stable sur le bus avant
 * l'instant de latch** du 6502 (proche du front descendant de PHI2, après le temps
 * de setup). Un périphérique **lent** (typiquement le LOCI : le RP2040 échantillonne
 * le bus par PIO à `sys_clk = PHI2×30` puis pose la donnée) peut **manquer** ce
 * latch → le 6502 latche un bus non piloté (open-bus).
 *
 * Ce module fournit la grille et le prédicat de course, indépendamment de la
 * fréquence PHI2 (tout est exprimé en **fractions de période**, donc en subticks).
 *
 * Modèle (Phase 1) :
 *   - la période PHI2 est divisée en `BUS_PHI2_SUBTICKS` (= 30, car le LOCI cadence
 *     son PIO à PHI2×30, `cpu.c:158`) ;
 *   - le 6502 **latche** la donnée lue au subtick `latch_subtick` (fin de PHI2
 *     haut moins le setup) ;
 *   - un périphérique rend sa donnée **valide** au subtick `valid_subtick` ;
 *   - la lecture est **propre** ssi `valid_subtick <= latch_subtick`, sinon la
 *     course est **perdue** (open-bus).
 *
 * Les périphériques **on-board** (RAM/ROM/VIA/ULA) sont, par définition, valides
 * tôt (`valid_subtick = 0`) → ils gagnent toujours la course → aucun impact. Seuls
 * les périphériques du port d'extension à serve lent (LOCI aujourd'hui) peuvent
 * perdre. C'est la réalisation « globale » mais à coût nul pour l'existant.
 *
 * NB : les constantes sous-cycle (`latch`, budget de serve) sont des valeurs
 * **modélisées/calibrables** dans les plages établies par l'analyse
 * (`~/loci/extensions/analyse/read-serve-et-inhibition-via.md` : serve 26-36 cyc
 * M0+, sys_clk = PHI2×30), à affiner sur matériel réel — elles ne prétendent pas
 * à l'exactitude picoseconde.
 */
#ifndef BUS_TIMING_H
#define BUS_TIMING_H

#include <stdint.h>
#include <stdbool.h>

/** Subdivisions d'une période PHI2. 30 = rapport sys_clk/PHI2 du LOCI (cpu.c:158). */
#define BUS_PHI2_SUBTICKS   30

/** Instant de latch de la donnée par le 6502, en subticks (fin du cycle moins le
 *  setup). Défaut modélisé : le 6502 échantillonne près du front descendant. */
#define BUS_LATCH_SUBTICK_DEFAULT   27

/**
 * @brief La donnée du périphérique arrive-t-elle à temps pour le latch 6502 ?
 * @param valid_subtick Subtick auquel la donnée devient stable sur le bus.
 * @param latch_subtick Subtick de latch du 6502.
 * @return true si le serve gagne la course (lecture propre), false s'il la perd
 *         (open-bus). Un périphérique on-board passe `valid_subtick = 0` → toujours true.
 */
static inline bool bus_serve_wins_race(uint16_t valid_subtick, uint8_t latch_subtick) {
    return valid_subtick <= (uint16_t)latch_subtick;
}

/* ── Jitter déterministe (Phase 2) ──────────────────────────────────────────
 * Sur le vrai bus, la marge de timing n'est pas binaire : bruit d'horloge,
 * température, tolérances → près de la frontière de latch, certains accès passent
 * et d'autres ratent (le rapport de bug le note : « occasionnel », dépend du
 * build/carte). On modélise ça par un décalage aléatoire du subtick de validité
 * du serve, tiré d'un PRNG **seedé** → reproductible (tests déterministes), pas
 * de dépendance à l'horloge murale. */

/** xorshift32 : PRNG déterministe minimal. `*state` ne doit jamais valoir 0. */
static inline uint32_t bus_jitter_rand(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/**
 * @brief Tire un décalage de jitter symétrique dans [-amp, +amp] subticks.
 * @param state PRNG (avancé à chaque appel ; à seeder via bus_jitter_seed()).
 * @param amp Amplitude (0 = pas de jitter → renvoie 0 sans avancer l'état).
 */
static inline int bus_jitter_sample(uint32_t* state, uint8_t amp) {
    if (amp == 0) return 0;
    uint32_t span = (uint32_t)amp * 2u + 1u;         /* [-amp .. +amp] */
    return (int)(bus_jitter_rand(state) % span) - (int)amp;
}

/** Seed du PRNG de jitter (jamais 0 : xorshift dégénère à 0). */
static inline uint32_t bus_jitter_seed(uint32_t seed) {
    return seed ? seed : 0xA5A5A5A5u;
}

#endif /* BUS_TIMING_H */
