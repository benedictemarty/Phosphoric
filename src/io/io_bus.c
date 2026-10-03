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
static void loci_emu_reflect_nirq(emulator_t* emu) {
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

/* ACIA 6551 ($031C-$031F par défaut, base configurable). */
static bool acia_dev_claims(emulator_t* emu, uint16_t addr) {
    /* Co-sim : le firmware sert sa fenêtre ACIA dès le boot, dongle ou non
     * (sans modem : $0381 = $70). Sans ce claim, --loci-emu sans --loci-cdc
     * laissait le miroir du VIA répondre en $0380 — infidèle au matériel. */
    if (loci_emu_active() && loci_emu_acia_served(addr)) return true;
    return emu->has_serial && addr >= emu->acia_base_addr && addr <= (emu->acia_base_addr + 3);
}
/* picowifi-over-LOCI : l'ACIA 6551 émulée vit à $0380, servie par une COURSE
 * PHI2. Le MIA (RP2040 : PIO core1 + serve logiciel lent, ~I²C/DMA) doit poser
 * l'octet sur le data bus AVANT le front PHI2 montant du 6502. Si la marge de
 * timing `tior` est mal réglée (hors fenêtre auto-tunée par ADJ_SCAN), le serve
 * perd la course. Le VIA étant décodé-inhibé symétriquement sur tout $03x0-$03xF
 * (IO_CONTROL = IO·(A4+A5+A6+A7), prouvé matériellement), RIEN ne pilote alors
 * le bus → le 6502 latche l'OPEN-BUS (dernier octet piloté), PAS le VIA.
 *
 * Asymétrie fidèle au HW (rapport de bug + spec-acia-fiable) :
 *  - ÉCRITURE toujours fiable : `write_enable_map = 0xFFFFFFFF` → une write
 *    $0380-$0383 atteint TOUJOURS l'ACIA, course perdue ou non.
 *  - LECTURE fragile ET, sur le registre DATA, DESTRUCTIVE côté LOCI : le serve
 *    exécute `acia_read()` « en aveugle » (il consomme l'octet RX) pendant que
 *    le 6502 ne latche que du bus flottant → OCTET PERDU, non relisable. C'est
 *    précisément le « modem injoignable ».
 *  - STAT/CMD/CTRL sont idempotents (relisibles) → une course perdue renvoie du
 *    bus flottant CE tour-ci mais le registre reste lisible au suivant (raté
 *    pardonné, comme le polling disque/MIA). D'où : disque OK / modem KO sous la
 *    MÊME marge, sans avoir besoin d'un modèle probabiliste. */
static inline bool acia_serve_lost(const emulator_t* emu) {
    return emu->has_loci && emu->acia_base_addr == 0x0380 &&
           !loci_mia_io_reliable(&emu->loci);
}
static uint8_t acia_dev_read(emulator_t* emu, uint16_t addr) {
    /* Backend co-sim (--loci-cdc) : l'ACIA $0380 est servie par le VRAI firmware
     * (oric/acia.c ↔ modem USB CDC) au lieu du 6551 comportemental. */
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr))) {
        uint8_t v = loci_emu_acia_read(addr);
        loci_emu_reflect_nirq(emu);
        return v;
    }
    /* Chemin CPU : échantillonne la course AVEC jitter (avance le PRNG). Le jitter
     * n'a d'effet qu'en modèle PHASE près du latch ; sinon c'est la décision
     * nominale déterministe. */
    bool lost = emu->has_loci && emu->acia_base_addr == 0x0380 &&
                loci_mia_serve_lost_sampled(&emu->loci);
    if (lost) {
        /* Course perdue : sur DATA, LOCI a consommé l'octet en aveugle (perdu) ;
         * le 6502 latche l'open-bus. Sur STAT/CMD/CTRL, rien n'est consommé. */
        if ((addr & ACIA_ADDR_MASK) == ACIA_REG_DATA)
            (void)acia_read(&emu->acia, addr);   /* consomme et jette : octet perdu */
        return memory_open_bus(&emu->memory);
    }
    return acia_read(&emu->acia, addr);
}
static bool acia_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    /* Backend co-sim (--loci-cdc) : écriture $0380-$0383 traitée par le vrai firmware. */
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr))) {
        loci_emu_acia_write(addr, value);
        loci_emu_reflect_nirq(emu);
        return true;
    }
    /* Écriture toujours fiable (write_enable_map = 0xFFFFFFFF sur le vrai LOCI) :
     * elle passe même course perdue. */
    acia_write(&emu->acia, addr, value);
    return true;
}
/* Lecture d'observation non destructive (débogueur/moniteur/dump/déporté) :
 * ne vide PAS RDRF, ne pope PAS la FIFO, n'efface PAS l'IRQ. Modélise l'open-bus
 * SANS consommer (un observateur ne participe pas à la course PHI2 du 6502). */
static uint8_t acia_dev_peek(emulator_t* emu, uint16_t addr) {
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr)))   /* co-sim : peek io-page */
        return loci_emu_acia_peek(addr);
    if (acia_serve_lost(emu))
        return memory_open_bus(&emu->memory);
    return acia_peek(&emu->acia, addr);
}

/* Mageco / ORICON MIDI (ACIA 6850) : $03FE-$03FF ou $031C-$031E. */
static bool mageco_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_mageco && mageco_addr_in_range(&emu->mageco, addr);
}
static uint8_t mageco_dev_read(emulator_t* emu, uint16_t addr) {
    return mageco_read(&emu->mageco, addr);
}
static bool mageco_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    mageco_write(&emu->mageco, addr, value);
    return true;
}
/* Savestate (section "MAG") : émise seulement si le Mageco est présent →
 * .ost inchangé sinon. Transport hôte non restauré (cf. mageco_save). */
static bool mageco_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_mageco) return false;
    return mageco_save(&emu->mageco, fp);
}
static void mageco_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    mageco_load(&emu->mageco, fp, size);
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
/* SP0256 Mageco "Synthétiseur Vocal" (GI SP0256-AL2) : port unique à
 * emu->sp0256.base_addr (défaut $03F1). Sortie audio mixée au PSG. */
static bool sp0256_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_sp0256 && addr == emu->sp0256.base_addr;
}
static uint8_t sp0256_dev_read(emulator_t* emu, uint16_t addr) {
    return sp0256_read(&emu->sp0256, addr);
}
static bool sp0256_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    sp0256_write(&emu->sp0256, addr, value);
    return true;
}

/* Savestate (sections "SPO" / "MEA") : émises seulement si la carte est présente. */
static bool sp0256_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_sp0256) return false;
    return sp0256_save(&emu->sp0256, fp);
}
static void sp0256_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    sp0256_load(&emu->sp0256, fp, size);
}

/* Digitelec DTL 2000 (PIA 6821 + ACIA 6850) : $03F8-$03FD (plage exclusive). */
static bool dtl2000_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_dtl2000 && dtl2000_addr_in_range(&emu->dtl2000, addr);
}
static uint8_t dtl2000_dev_read(emulator_t* emu, uint16_t addr) {
    return dtl2000_read(&emu->dtl2000, addr);
}
static bool dtl2000_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    dtl2000_write(&emu->dtl2000, addr, value);
    return true;
}
/* Savestate (section "DTL") : émise seulement si le DTL2000 est présent →
 * .ost inchangé sinon. Transport hôte non restauré (cf. dtl2000_save). */
static bool dtl2000_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_dtl2000) return false;
    return dtl2000_save(&emu->dtl2000, fp);
}
static void dtl2000_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    dtl2000_load(&emu->dtl2000, fp, size);
}

/* ULA-NG $0340-$035F : dernier périphérique du bus, avant le repli VIA.
 *  - Lecture : ne répond que déverrouillée (`claims`) ; verrouillée, la fenêtre
 *    retombe sur le miroir VIA (indiscernable).
 *  - Écriture : `claims_write` = fenêtre seule → l'ULA-NG voit les écritures
 *    même verrouillée pour guetter la séquence 'N','G'. `ula_ng_write` renvoie
 *    si elle a consommé ; sinon le dispatch retombe sur le VIA (bit-à-bit). */
static bool ula_ng_dev_claims(emulator_t* emu, uint16_t addr) {
    return ula_ng_active(&emu->ula_ng) && ula_ng_addr_in_window(addr);
}
static bool ula_ng_dev_claims_write(emulator_t* emu, uint16_t addr) {
    (void)emu;
    return ula_ng_addr_in_window(addr);
}
static uint8_t ula_ng_dev_read(emulator_t* emu, uint16_t addr) {
    return ula_ng_read(&emu->ula_ng, addr);
}
static bool ula_ng_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (!ula_ng_write(&emu->ula_ng, addr, value))
        return false;   /* non consommée (verrouillée, octet neutre) → repli VIA */
    /* Écriture consommée : synchroniser la ligne d'IRQ raster (un write de
     * NG_STATUS acquitte → désassertion). */
    if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
    else                          cpu_irq_clear(&emu->cpu, IRQF_ULANG);
    return true;
}
/* Savestate (section "UNG") : délégué au module (POD, même-build, garde taille). */
static bool ula_ng_dev_save(emulator_t* emu, FILE* fp) {
    return ula_ng_save(&emu->ula_ng, fp);
}
static void ula_ng_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    ula_ng_load(&emu->ula_ng, fp, size);
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
static void acia_dev_tick(emulator_t* emu, int cycles) {
    acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
    acia_tick(&emu->acia, cycles);
}
static void dtl2000_dev_tick(emulator_t* emu, int cycles) { dtl2000_tick(&emu->dtl2000, cycles); }
static void mageco_dev_tick(emulator_t* emu, int cycles)  { mageco_tick(&emu->mageco, cycles); }
static void sp0256_dev_tick(emulator_t* emu, int cycles)  { sp0256_tick(&emu->sp0256, cycles); }

/* Position de chaque device dans io_bus[] (= priorité de dispatch). */
enum { DEV_LOCI, DEV_ACIA, DEV_MAGECO, DEV_MICRODISC, DEV_JASMIN, DEV_SP0256,
       DEV_DTL2000, DEV_ULA_NG, DEV_COUNT };

#define PRESENT(flag) offsetof(emulator_t, flag)

static const io_device_t io_bus[DEV_COUNT] = {
    /* (LOCI : pas de section .ost — réserve des handles OS du backend fichiers.) */
    [DEV_LOCI] = { .name = "loci", .claims = loci_dev_claims, .read = loci_dev_read,
                   .write = loci_dev_write,
                   .present_off = PRESENT(has_loci), .tick = loci_dev_tick },
    /* ACIA : sa section « SER » est écrite par savestate.c (historique). */
    [DEV_ACIA] = { .name = "acia", .claims = acia_dev_claims, .read = acia_dev_read,
                   .write = acia_dev_write, .peek = acia_dev_peek,
                   .present_off = PRESENT(has_serial), .tick = acia_dev_tick },
    [DEV_MAGECO] = { .name = "mageco", .claims = mageco_dev_claims, .read = mageco_dev_read,
                     .write = mageco_dev_write,
                     .save_tag = "MAG\0", .save = mageco_dev_save, .load = mageco_dev_load,
                     .present_off = PRESENT(has_mageco), .tick = mageco_dev_tick },
    /* Microdisc : sections FDC/MDC/DSK/BAD écrites par savestate.c (historique). */
    [DEV_MICRODISC] = { .name = "microdisc", .claims = microdisc_dev_claims,
                        .read = microdisc_dev_read, .write = microdisc_dev_write,
                        .present_off = PRESENT(has_microdisc), .tick = microdisc_dev_tick },
    [DEV_JASMIN] = { .name = "jasmin", .claims = jasmin_dev_claims, .read = jasmin_dev_read,
                     .write = jasmin_dev_write,
                     .save_tag = "JAS\0", .save = jasmin_dev_save, .load = jasmin_dev_load,
                     .present_off = PRESENT(has_jasmin), .tick = jasmin_dev_tick },
    [DEV_SP0256] = { .name = "sp0256", .claims = sp0256_dev_claims, .read = sp0256_dev_read,
                     .write = sp0256_dev_write,
                     .save_tag = "SPO\0", .save = sp0256_dev_save, .load = sp0256_dev_load,
                     .present_off = PRESENT(has_sp0256), .tick = sp0256_dev_tick },
    [DEV_DTL2000] = { .name = "dtl2000", .claims = dtl2000_dev_claims, .read = dtl2000_dev_read,
                      .write = dtl2000_dev_write,
                      .save_tag = "DTL\0", .save = dtl2000_dev_save, .load = dtl2000_dev_load,
                      .present_off = PRESENT(has_dtl2000), .tick = dtl2000_dev_tick },
    /* ULA-NG en dernier (repli avant VIA). claims_write distinct : voit les
     * écritures de sa fenêtre même verrouillée (guet 'N','G'). Sérialisée via la
     * section "UNG" (émise seulement si déverrouillée → .ost inchangé sinon).
     * Pas de tick : l'ULA-NG avance avec la vidéo. */
    [DEV_ULA_NG] = { .name = "ula-ng", .claims = ula_ng_dev_claims, .read = ula_ng_dev_read,
                     .write = ula_ng_dev_write, .claims_write = ula_ng_dev_claims_write,
                     .save_tag = "UNG\0", .save = ula_ng_dev_save, .load = ula_ng_dev_load },
};

/* ORDRE DES TICKS, distinct de l'ordre de dispatch et PRÉSERVÉ à l'identique de
 * l'ancien cpu_cycle_tick (microdisc → jasmin → loci → acia → dtl → mageco →
 * sp0256), puis les cartes en modules dans l'ordre de k_card_modules (mea8000) :
 * iso-comportement par construction. */
static const io_device_t* const io_bus_tick_order[] = {
    &io_bus[DEV_MICRODISC], &io_bus[DEV_JASMIN], &io_bus[DEV_LOCI], &io_bus[DEV_ACIA],
    &io_bus[DEV_DTL2000], &io_bus[DEV_MAGECO], &io_bus[DEV_SP0256],
};

/* Table de répartition effective : périphériques du cœur, avec les cartes en
 * modules insérées avant leur ancre (bus_before), copiés dans un tableau
 * contigu (même forme qu'avant pour savestate.c : l'ordre des sections .ost est
 * celui de cette table). Construite au premier usage. */
#define IO_BUS_MAX (DEV_COUNT + 32)
static io_device_t s_bus[IO_BUS_MAX];
static int s_bus_n = -1;

static void bus_add_modules_before(const char* name, bool* placed) {
    for (int m = 0; m < k_card_module_count; m++) {
        const card_module_t* mod = k_card_modules[m];
        if (placed[m] || !mod->bus) continue;
        if (name ? (mod->bus_before && strcmp(mod->bus_before, name) == 0) : true) {
            placed[m] = true;
            bus_add_modules_before(mod->bus->name, placed);
            if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = *mod->bus;
        }
    }
}

static void io_bus_build(void) {
    bool placed[64] = { false };
    s_bus_n = 0;
    for (int i = 0; i < DEV_COUNT; i++) {
        bus_add_modules_before(io_bus[i].name, placed);
        s_bus[s_bus_n++] = io_bus[i];
    }
    bus_add_modules_before(NULL, placed);          /* sans ancre : en fin de table */
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
