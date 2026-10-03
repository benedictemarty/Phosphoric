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

/* LOCI (sodiumlb) : trois sous-fenêtres disjointes, dispatchées en interne.
 *  - MIA $03A0-$03BF (indépendant des autres périphériques) ;
 *  - TAP $0315-$0317 : remplace l'interface cassette, recouvre le Microdisc
 *    $0310-$031F → priorité (LOCI est en tête de table) ;
 *  - DSK $0310-$0314 + $0318-$0319 : seulement en l'absence de vrai Microdisc
 *    (sinon le Microdisc possède la plage). */
/* Co-sim : fenêtre + registres de l'expansion RAM $AF ($03C0-$03E4), servis par le
 * firmware (io-page) — inconnus du modèle interne. */
static bool loci_emu_ramx_claims(uint16_t addr) {
    return loci_emu_active() && addr >= 0x03C0 && addr <= 0x03E4;
}
static bool loci_dev_claims(emulator_t* emu, uint16_t addr) {
    if (!emu->has_loci) return false;
    if (loci_emu_io_page()) return addr >= 0x0310 && addr <= 0x03FF;   /* backend neo : /IO CONTROL */
    if (loci_addr_in_mia(addr) || loci_emu_ramx_claims(addr)) return true;
    if (loci_addr_in_tap(addr)) return true;
    if (!emu->has_microdisc && loci_addr_in_dsk(addr)) return true;
    return false;
}
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
static uint8_t loci_dev_read(emulator_t* emu, uint16_t addr) {
    /* Backend co-sim (--loci-emu) : la fenêtre MIA $03xx est servie par le VRAI
     * firmware RP2040 (émulateur) au lieu du backend comportemental (loci_core).
     * Pendant le boot arrière-plan (1er lancement d'un ELF), on ATTEND : sinon le
     * modèle interne répondait à la place du firmware (open("N:…") → FR_NO_FILE). */
    loci_emu_wait_boot();
    if (loci_emu_io_page()) {
        uint8_t v;
        bool drv = loci_emu_io_read(addr, &v);
        loci_emu_reflect_nirq(emu);
        return drv ? v : memory_open_bus(&emu->memory);
    }
    if (loci_emu_ramx_claims(addr)) return loci_emu_api_read(addr);
    if (loci_addr_in_mia(addr)) return loci_emu_active() ? loci_emu_api_read(addr)
                                                         : loci_read(&emu->loci, addr);
    if (loci_addr_in_tap(addr)) return loci_emu_active() ? loci_emu_tap_read(addr)
                                                         : loci_tap_read(&emu->loci, addr);
    /* DSK (claims l'a garanti). En co-sim, le WD1793 est celui du firmware (oric/dsk.c) :
     * un .dsk monté sur A: dans le VRAI menu LOCI est enfin lu par le 6502. */
    if (loci_emu_active()) {
        uint8_t v = loci_emu_dsk_read(addr);
        loci_emu_reflect_nirq(emu);   /* fin de secteur : l'IRQ naît sur la DERNIÈRE lecture DATA */
        return v;
    }
    return loci_dsk_read(&emu->loci, addr);
}
static bool loci_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    loci_emu_wait_boot();
    if (loci_emu_io_page()) { loci_emu_io_write(addr, value); loci_emu_reflect_nirq(emu); return true; }
    if (loci_emu_ramx_claims(addr))  loci_emu_api_write(addr, value);
    else if (loci_addr_in_mia(addr)) { if (loci_emu_active()) { loci_emu_api_write(addr, value);
                                                           loci_emu_reflect_nirq(emu); }
                                  else                   loci_write(&emu->loci, addr, value); }
    else if (loci_addr_in_tap(addr)) { if (loci_emu_active()) loci_emu_tap_write(addr, value);
                                       else                   loci_tap_write(&emu->loci, addr, value); }
    else if (loci_emu_active())    { loci_emu_dsk_write(addr, value);           /* DSK co-sim */
                                     loci_emu_reflect_nirq(emu); }
    else                             loci_dsk_write(&emu->loci, addr, value);  /* DSK */
    return true;
}


/* Microdisc WD1793 : $0310-$031F (l'ACIA, enregistrée avant, possède déjà
 * $031C-$031F si présente → pas de test interne ici). */
static bool microdisc_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_microdisc && addr >= 0x0310 && addr <= 0x031F;
}
static uint8_t microdisc_dev_read(emulator_t* emu, uint16_t addr) {
    return microdisc_read(&emu->microdisc, addr);
}
static bool microdisc_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles, addr, value);
    }
    microdisc_write(&emu->microdisc, addr, value);
    /* Sync overlay flags to memory system */
    emu->memory.basic_rom_disabled = emu->microdisc.romdis;
    emu->memory.overlay_active = emu->microdisc.diskrom;
    return true;
}

/* Jasmin WD177x : $03F4-$03FF (mutuellement exclusif avec DTL2000/Mageco, qui
 * recouvrent $03F8-$03FF — garde à l'activation dans main.c). */
static bool jasmin_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_jasmin && addr >= JASMIN_BASE && addr <= JASMIN_END;
}
static uint8_t jasmin_dev_read(emulator_t* emu, uint16_t addr) {
    return jasmin_read(&emu->jasmin, addr);
}
static bool jasmin_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles, addr, value);
    }
    jasmin_write(&emu->jasmin, addr, value);
    /* Sync Jasmin banking flags to the memory system. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
    return true;
}

/* Savestate (section "JAS") : émise seulement si le Jasmin est présent. Les
 * images disque passent par la section DSK (savestate.c), lue AVANT. */
static bool jasmin_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_jasmin) return false;
    return jasmin_save(&emu->jasmin, fp);
}
static void jasmin_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    jasmin_load(&emu->jasmin, fp, size);
    /* Même synchronisation des verrous vers la mémoire que jasmin_dev_write. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
}

/* ── Ticks : exactement les opérations de l'ancien io_bus_tick, par device ── */
static void microdisc_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->microdisc.fdc, cycles);
}
static void jasmin_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->jasmin.fdc, cycles);
}
static void loci_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->loci.dsk_fdc, cycles);
    loci_adj_tick(&emu->loci, cycles);
}

/* Position de chaque device dans io_bus[] (= priorité de dispatch). */
enum { DEV_LOCI, DEV_MICRODISC, DEV_JASMIN, DEV_COUNT };

#define PRESENT(flag) offsetof(emulator_t, flag)

static const io_device_t io_bus[DEV_COUNT] = {
    /* (LOCI : pas de section .ost — réserve des handles OS du backend fichiers.) */
    [DEV_LOCI] = { .name = "loci", .claims = loci_dev_claims, .read = loci_dev_read,
                   .write = loci_dev_write,
                   .present_off = PRESENT(has_loci), .tick = loci_dev_tick },
    /* Microdisc : sections FDC/MDC/DSK/BAD écrites par savestate.c (historique). */
    [DEV_MICRODISC] = { .name = "microdisc", .claims = microdisc_dev_claims,
                        .read = microdisc_dev_read, .write = microdisc_dev_write,
                        .present_off = PRESENT(has_microdisc), .tick = microdisc_dev_tick },
    [DEV_JASMIN] = { .name = "jasmin", .claims = jasmin_dev_claims, .read = jasmin_dev_read,
                     .write = jasmin_dev_write,
                     .save_tag = "JAS\0", .save = jasmin_dev_save, .load = jasmin_dev_load,
                     .present_off = PRESENT(has_jasmin), .tick = jasmin_dev_tick },
};

/* ORDRE DES TICKS, distinct de l'ordre de dispatch et PRÉSERVÉ à l'identique de
 * l'ancien cpu_cycle_tick (microdisc → jasmin → loci), puis les cartes en
 * modules dans l'ordre de cards_list.h (acia → dtl2000 → mageco → sp0256 →
 * mea8000) :
 * iso-comportement par construction. */
static const io_device_t* const io_bus_tick_order[] = {
    &io_bus[DEV_MICRODISC], &io_bus[DEV_JASMIN], &io_bus[DEV_LOCI],
};

/* Table de répartition effective : périphériques du cœur, avec les cartes en
 * modules insérées avant leur ancre (bus_before), copiés dans un tableau
 * contigu (même forme qu'avant pour savestate.c : l'ordre des sections .ost est
 * celui de cette table). Construite au premier usage. */
#define IO_BUS_MAX (DEV_COUNT + 32)
static io_device_t s_bus[IO_BUS_MAX];
static int s_bus_n = -1;

static const char* bus_anchor(int m) { return k_card_modules[m]->bus_before; }
static int bus_count(int m) { return k_card_modules[m]->bus ? 1 : 0; }
static const char* bus_key(int m, int j) { (void)j; return k_card_modules[m]->bus->name; }
static void bus_emit(int m, int j, void* ctx) {
    (void)j; (void)ctx;
    if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = *k_card_modules[m]->bus;
}
static void bus_emit_core(int i, void* ctx) {
    (void)ctx;
    if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = io_bus[i];
}

static void io_bus_build(void) {
    const char* names[DEV_COUNT];
    for (int i = 0; i < DEV_COUNT; i++) names[i] = io_bus[i].name;
    s_bus_n = 0;
    card_modules_place(names, DEV_COUNT, bus_anchor, bus_count, bus_key,
                       bus_emit, bus_emit_core, NULL);
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
