/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file bus_timing.h
 * @brief Chronologie sous-cycle d'une lecture LOCI sur le bus d'extension Oric.
 *
 * Le 6502 de l'Oric et les périphériques du port d'extension partagent un bus
 * **asynchrone** cadencé par PHI2, sans RDY : un périphérique ne peut pas ralentir
 * le cycle, il doit poser sa donnée avant que le 6502 la capture au front
 * descendant de PHI2 (moins le temps d'établissement tDSR). À l'échelle du cycle
 * entier (`cpu_step`/`cpu_tick`), toutes les lectures « réussissent » ; un
 * périphérique trop lent fait en réalité lire un bus non piloté (open-bus).
 *
 * Chronologie d'une lecture `$03xx` servie par la LOCI, en picosecondes depuis le
 * front descendant de PHI2 qui ouvre le cycle (2.23.0) :
 *   - le PIO de la LOCI tourne à Φ2cfg × 30, où Φ2cfg est un RÉGLAGE du firmware,
 *     4000 kHz par défaut (cpu.c, configuration vide) → 1 tick = 8,33 ns. Ce n'est
 *     pas l'horloge de l'Oric (l'ancienne grille « PHI2×30 » de l'Oric était fausse) ;
 *   - `mia_action` pousse le mot dans la FIFO (22 + tior) ticks après le front, plus
 *     2 cycles sys de synchroniseur d'entrée (comptes lus dans mia.pio, estimés) ;
 *   - `act_loop` le prend (poll, non mesuré) et, SERVE cycles sys plus tard,
 *     déclenche la DMA de read-serve et l'IRQ 5 de `mia_io_read` ;
 *   - `mia_io_read` attend PHI2 haut puis pilote le bus (3 + tiod) ticks après :
 *     donnée = max(prête, montée de PHI2 + synchro) + (3 + tiod) ticks ;
 *   - PHI2 de l'Oric : haut le dernier tiers du cycle (l'ULA donne 2/3 bas, 1/3
 *     haut ; forum Defence Force t=2583) ;
 *   - échéance du 6502 : fin du cycle moins tDSR (100 ns, fiche 6502 à 1 MHz).
 *
 * Les périphériques **on-board** (RAM/ROM/VIA/ULA) ne passent pas par ce modèle :
 * ils sont toujours à temps. Restent des estimations, à confirmer sur bus réel :
 * les comptes PIO, le poll d'act_loop, le tDSR du 6502 à 2 MHz de l'Oric.
 * Mesure (Feather 5723, firmware LOCI_USB) : serve 23 cycles → donnée à ≈ 708 ns.
 */
#ifndef BUS_TIMING_H
#define BUS_TIMING_H

#include <stdint.h>
#include <stdbool.h>

/** Comptes PIO de mia.pio (mia_action jusqu'au push, mia_io_read après l'IRQ). */
#define BUS_LOCI_PUSH_TICKS        22
#define BUS_LOCI_OUT_TICKS         3
/** Réglage Φ2 du firmware par défaut (kHz) et horloge système qui en découle. */
#define BUS_LOCI_PHI2CFG_KHZ       4000u
#define BUS_LOCI_SYS_KHZ           120000u
/** Oric : période PHI2 1 µs, haut le dernier tiers ; tDSR du 6502. */
#define BUS_ORIC_PERIOD_PS         1000000
#define BUS_ORIC_TDSR_NS_DEFAULT   100

typedef struct {
    uint32_t sys_khz;      /* horloge du cœur 1 (cycles de serve) */
    uint32_t pio_khz;      /* horloge PIO = Φ2cfg × 30 */
    int64_t  period_ps;    /* période PHI2 de l'Oric */
    int64_t  high_ps;      /* durée de PHI2 haut */
    int64_t  tdsr_ps;      /* établissement des données du 6502 */
    int64_t  poll_ps;      /* FIFO → act_loop (non mesuré) */
} bus_loci_timing_t;

static inline bus_loci_timing_t bus_loci_timing_default(void) {
    bus_loci_timing_t t;
    t.sys_khz   = BUS_LOCI_SYS_KHZ;
    t.pio_khz   = BUS_LOCI_PHI2CFG_KHZ * 30u;
    t.period_ps = BUS_ORIC_PERIOD_PS;
    t.high_ps   = BUS_ORIC_PERIOD_PS / 3;
    t.tdsr_ps   = (int64_t)BUS_ORIC_TDSR_NS_DEFAULT * 1000;
    t.poll_ps   = 0;
    return t;
}

/** n périodes d'une horloge de `khz` kHz, en ps (sans cumul d'arrondi). */
static inline int64_t bus_ps(int64_t n, uint32_t khz) {
    return n * 1000000000LL / (int64_t)khz;
}

/**
 * @brief Instant (ps après le front descendant de PHI2) où la donnée d'une lecture
 *        `$03xx` servie en `serve` cycles du cœur 1 est sur le bus.
 */
static inline int64_t bus_loci_read_valid_ps(const bus_loci_timing_t* t, unsigned tior,
                                             unsigned tiod, int64_t serve) {
    int64_t sync  = bus_ps(2, t->sys_khz);
    int64_t ready = bus_ps(BUS_LOCI_PUSH_TICKS + (int64_t)tior, t->pio_khz) + sync
                  + t->poll_ps + bus_ps(serve, t->sys_khz);
    int64_t rise  = t->period_ps - t->high_ps + sync;
    return (ready > rise ? ready : rise) + bus_ps(BUS_LOCI_OUT_TICKS + (int64_t)tiod, t->pio_khz);
}

/** Échéance du 6502 : fin du cycle moins tDSR. */
static inline int64_t bus_loci_deadline_ps(const bus_loci_timing_t* t) {
    return t->period_ps - t->tdsr_ps;
}

/** La donnée arrive-t-elle avant l'échéance ? (false = open-bus) */
static inline bool bus_loci_read_in_time(const bus_loci_timing_t* t, unsigned tior,
                                         unsigned tiod, int64_t serve) {
    return bus_loci_read_valid_ps(t, tior, tiod, serve) <= bus_loci_deadline_ps(t);
}

/* ── Jitter déterministe (Phase 2) ──────────────────────────────────────────
 * Sur le vrai bus, la marge de timing n'est pas binaire : bruit d'horloge,
 * température, tolérances → près de la frontière de latch, certains accès passent
 * et d'autres ratent (le rapport de bug le note : « occasionnel », dépend du
 * build/carte). On modélise ça par un décalage aléatoire de la durée de serve
 * (en cycles du cœur 1), tiré d'un PRNG **seedé** → reproductible (tests déterministes), pas
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
 * @brief Tire un décalage de jitter symétrique dans [-amp, +amp] cycles.
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
