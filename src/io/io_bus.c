/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file io_bus.c
 * @brief I/O bus adapter — `io_device_t` wrappers, table and dispatch.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extracted from main.c (Epic 7 / US2, Sprint 126), with identical behaviour. Confines
 * the coupling to `emulator_t` to a single adaptation layer; the peripheral
 * modules stay decoupled from `emulator.h`. See docs/architecture/io-bus.md.
 */
#include "io/io_bus.h"
#include "io/loci_emu.h"   /* co-sim backend: MIA $03xx API served by the real firmware (--loci-emu) */
#include "emulator.h"
#include "card_module.h"   /* cards as modules: their bus devices */
#include "card_ticks.h"    /* their per-cycle ticks, as direct calls */

#include <stdio.h>
#include <stddef.h>   /* offsetof */
#include <string.h>

/* ── I/O bus: registered devices (docs/architecture/io-bus.md) ─────────────
 * Each device provides claims/read/write; the dispatch walks the table.
 * Table order = priority: LOCI first (overlaps the Microdisc via
 * TAP $0315-$0317), then ACIA (owns $031C-$031F when present) before Microdisc.
 * The ULA-NG comes last: its write must always receive the byte in its window
 * (even when locked, to watch for the 'N','G' sequence) and `ula_ng_write`
 * *returns* whether it consumed it — otherwise VIA fallback; hence the separate
 * `claims_write` and the boolean return of `write`. "Strangler" pattern. */

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



/* ── Ticks: exactly the operations of the old io_bus_tick, per device ─────── */
/* Disk controllers: cards as modules (src/cards/card_{microdisc,jasmin}.c)
 * whose state stays in the machine; the core advances their FDC here, before
 * LOCI, as before (one more call per cycle cost 0.65 to 1% of instructions). */
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


/* TICK ORDER, distinct from the dispatch order and PRESERVED exactly as in
 * the old cpu_cycle_tick (microdisc → jasmin → loci), then the cards as
 * modules in cards_list.h order (acia → dtl2000 → mageco → sp0256 →
 * mea8000):
 * identical behaviour by construction. */
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

/* Tick of the timed bus devices, in io_bus_tick_order. */
void io_bus_tick(emulator_t* emu, int cycles) {
    const char* base = (const char*)emu;
    /* Called on EVERY cycle: once unrolled, the loop over a constant table
     * folds into flag tests + direct calls (measured cost: +22 % per frame
     * without unrolling). `expect(…, 0)` keeps the common case — no device —
     * on the straight-line path, with calls out of line (+7 % without it). */
#pragma GCC unroll 16
    for (size_t i = 0; i < sizeof(io_bus_tick_order) / sizeof(io_bus_tick_order[0]); i++) {
        const io_device_t* d = io_bus_tick_order[i];
        if (__builtin_expect(*(const bool*)(base + d->present_off), 0))
            d->tick(emu, cycles);
    }
    /* Cards as modules (after the core, in list order): generated direct
     * calls (card_ticks.h). */
    card_modules_tick(emu, cycles);
}
