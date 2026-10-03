/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file io_bus.c
 * @brief Adaptateur bus I/O — wrappers `io_device_t`, table et dispatch.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US2, Sprint 126), à iso-comportement. Confine le
 * couplage à `emulator_t` dans une seule couche d'adaptation ; les modules de
 * périphériques restent découplés d'`emulator.h`. Voir docs/architecture/io-bus.md.
 */
#include "io/io_bus.h"
#include "io/loci_emu.h"   /* backend co-sim : API MIA $03xx servie par le vrai firmware (--loci-emu) */
#include "emulator.h"
#include "card_module.h"   /* cartes en modules : leurs périphériques de bus */
#include "card_ticks.h"    /* leurs ticks par cycle, en appels directs */

#include <stdio.h>
#include <stddef.h>   /* offsetof */
#include <string.h>

/* ── Bus I/O : périphériques enregistrés (docs/architecture/io-bus.md) ──────
 * Chaque périphérique fournit claims/read/write ; le dispatch parcourt la table.
 * L'ordre de la table = la priorité : LOCI en tête (recouvre le Microdisc via
 * TAP $0315-$0317), puis ACIA (possède $031C-$031F si présente) avant Microdisc.
 * L'ULA-NG est en dernier : son écriture doit toujours recevoir l'octet en
 * fenêtre (même verrouillée, pour guetter la séquence 'N','G') et `ula_ng_write`
 * *renvoie* si elle a consommé — sinon repli VIA ; d'où le `claims_write` distinct
 * et le retour booléen de `write`. Pattern « strangler ». */

/* Réflexion du nIRQ synchrone (backend co-sim --loci-emu). Le firmware RP2040
 * PULSE la ligne nIRQ (ext_put(EXT_IRQ,true) puis false) en réaction à une
 * transaction MIA (l'écriture fait tourner core0+core1 le temps du dialogue bus) :
 * un poll de NIVEAU le manquerait (déjà retombé). L'émulateur latche chaque front
 * montant ; on draine ces pulses ici, juste après la transaction, et on les délivre
 * au 6502 en EDGE / TIR UNIQUE (cpu_irq_pulse) — une IRQ par pulse, sans maintien
 * de niveau donc sans tempête. main.c draine aussi une fois par frame (filet pour
 * les pulses hors écriture MIA, ex. trap IRQ sur bouton). */
void loci_emu_reflect_nirq(emulator_t* emu) {
    int pulses = loci_emu_irq_take();
    for (int i = 0; i < pulses; i++) cpu_irq_pulse(&emu->cpu);
}



/* ── Ticks : exactement les opérations de l'ancien io_bus_tick, par device ── */
/* Contrôleurs disque : cartes en modules (src/cards/card_{microdisc,jasmin}.c)
 * dont l'état reste dans la machine ; le cœur avance leur FDC ici, avant LOCI,
 * comme avant (un appel de plus par cycle coûtait 0,65 à 1 % d'instructions). */
static void microdisc_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->microdisc.fdc, cycles);
}
static void jasmin_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->jasmin.fdc, cycles);
}
static const io_device_t k_microdisc_tick = {
    .name = "microdisc", .present_off = offsetof(emulator_t, card_on[CARD_IDX_microdisc]),
    .tick = microdisc_dev_tick };

static void loci_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->loci.dsk_fdc, cycles);
    loci_adj_tick(&emu->loci, cycles);
}
static const io_device_t k_jasmin_tick = {
    .name = "jasmin", .present_off = offsetof(emulator_t, card_on[CARD_IDX_jasmin]),
    .tick = jasmin_dev_tick };
/* LOCI (carte en module, src/cards/card_loci.c) : son état et son horloge restent
 * dans la machine ; le cœur l'avance ici, après les contrôleurs disque. */
static const io_device_t k_loci_tick = {
    .name = "loci", .present_off = offsetof(emulator_t, card_on[CARD_IDX_loci]),
    .tick = loci_dev_tick };


#define PRESENT(flag) offsetof(emulator_t, flag)


/* ORDRE DES TICKS, distinct de l'ordre de dispatch et PRÉSERVÉ à l'identique de
 * l'ancien cpu_cycle_tick (microdisc → jasmin → loci), puis les cartes en
 * modules dans l'ordre de cards_list.h (acia → dtl2000 → mageco → sp0256 →
 * mea8000) :
 * iso-comportement par construction. */
static const io_device_t* const io_bus_tick_order[] = {
    &k_microdisc_tick, &k_jasmin_tick, &k_loci_tick,
};

/* Table de répartition effective : périphériques du cœur, avec les cartes en
 * modules insérées avant leur ancre (bus_before), copiés dans un tableau
 * contigu (même forme qu'avant pour savestate.c : l'ordre des sections .ost est
 * celui de cette table). Construite au premier usage. */
#define IO_BUS_MAX 32
static io_device_t s_bus[IO_BUS_MAX];
static int s_bus_n = -1;

static const char* bus_anchor(int m) { return k_card_modules[m]->bus_before; }
static int bus_count(int m) { return k_card_modules[m]->bus ? 1 : 0; }
static const char* bus_key(int m, int j) { (void)j; return k_card_modules[m]->bus->name; }
static void bus_emit(int m, int j, void* ctx) {
    (void)j; (void)ctx;
    if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = *k_card_modules[m]->bus;
}

/* Tous les périphériques du bus sont désormais des cartes en modules : la table
 * se construit de leurs seules ancres (aucun élément du cœur). */
static void io_bus_build(void) {
    s_bus_n = 0;
    card_modules_place(NULL, 0, bus_anchor, bus_count, bus_key, bus_emit, NULL, NULL);
}

const io_device_t* io_bus_find(emulator_t* emu, uint16_t addr) {
    if (__builtin_expect(s_bus_n < 0, 0)) io_bus_build();
    for (int i = 0; i < s_bus_n; i++)
        if (s_bus[i].claims(emu, addr))
            return &s_bus[i];
    return NULL;
}

const io_device_t* io_bus_find_write(emulator_t* emu, uint16_t addr) {
    if (__builtin_expect(s_bus_n < 0, 0)) io_bus_build();
    for (int i = 0; i < s_bus_n; i++) {
        bool (*cw)(emulator_t*, uint16_t) = s_bus[i].claims_write ? s_bus[i].claims_write
                                                                  : s_bus[i].claims;
        if (cw(emu, addr))
            return &s_bus[i];
    }
    return NULL;
}

const io_device_t* io_bus_devices(int* count) {
    if (s_bus_n < 0) io_bus_build();
    if (count) *count = s_bus_n;
    return s_bus;
}

/* Tick des périphériques de bus temporisés, dans io_bus_tick_order. */
void io_bus_tick(emulator_t* emu, int cycles) {
    const char* base = (const char*)emu;
    /* Appelé à CHAQUE cycle : déroulée, la boucle sur une table constante se
     * replie en tests de drapeaux + appels directs (coût mesuré : +22 % par
     * trame sans déroulage). `expect(…, 0)` garde le cas courant — aucun
     * périphérique — en ligne droite, appels hors chemin (+7 % sans). */
#pragma GCC unroll 16
    for (size_t i = 0; i < sizeof(io_bus_tick_order) / sizeof(io_bus_tick_order[0]); i++) {
        const io_device_t* d = io_bus_tick_order[i];
        if (__builtin_expect(*(const bool*)(base + d->present_off), 0))
            d->tick(emu, cycles);
    }
    /* Cartes en modules (après le cœur, dans l'ordre de la liste) : appels
     * directs générés (card_ticks.h). */
    card_modules_tick(emu, cycles);
}
