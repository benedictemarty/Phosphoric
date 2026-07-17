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
#include "emulator.h"

#include <stdio.h>

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
static bool loci_dev_claims(emulator_t* emu, uint16_t addr) {
    if (!emu->has_loci) return false;
    if (loci_addr_in_mia(addr)) return true;
    if (loci_addr_in_tap(addr)) return true;
    if (!emu->has_microdisc && loci_addr_in_dsk(addr)) return true;
    return false;
}
static uint8_t loci_dev_read(emulator_t* emu, uint16_t addr) {
    if (loci_addr_in_mia(addr)) return loci_read(&emu->loci, addr);
    if (loci_addr_in_tap(addr)) return loci_tap_read(&emu->loci, addr);
    return loci_dsk_read(&emu->loci, addr);   /* DSK (claims l'a garanti) */
}
static bool loci_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (loci_addr_in_mia(addr))      loci_write(&emu->loci, addr, value);
    else if (loci_addr_in_tap(addr)) loci_tap_write(&emu->loci, addr, value);
    else                             loci_dsk_write(&emu->loci, addr, value);  /* DSK */
    return true;
}

/* ACIA 6551 ($031C-$031F par défaut, base configurable). */
static bool acia_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_serial && addr >= emu->acia_base_addr && addr <= (emu->acia_base_addr + 3);
}
static uint8_t acia_dev_read(emulator_t* emu, uint16_t addr) {
    /* picowifi-over-LOCI : ACIA à $0380 échantillonnée via la fenêtre MIA ;
     * une tior mal réglée corrompt la lecture (modem injoignable). */
    if (emu->has_loci && emu->acia_base_addr == 0x0380 && !loci_mia_io_reliable(&emu->loci))
        return 0xFF;
    return acia_read(&emu->acia, addr);
}
static bool acia_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    /* picowifi-over-LOCI : la write est avalée si le MIA n'est pas fiable, mais
     * elle est bien « consommée » (l'ACIA possède la plage) → true. */
    if (emu->has_loci && emu->acia_base_addr == 0x0380 && !loci_mia_io_reliable(&emu->loci))
        return true;
    acia_write(&emu->acia, addr, value);
    return true;
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

static const io_device_t io_bus[] = {
    /* (save_tag/save/load à NULL : ces devices n'ont pas encore de section .ost —
     * à migrer sur le même modèle que l'ULA-NG ; LOCI a la réserve des handles OS.) */
    { "loci",      loci_dev_claims,      loci_dev_read,      loci_dev_write,      NULL, NULL, NULL, NULL },
    { "acia",      acia_dev_claims,      acia_dev_read,      acia_dev_write,      NULL, NULL, NULL, NULL },
    { "mageco",    mageco_dev_claims,    mageco_dev_read,    mageco_dev_write,    NULL,
      "MAG\0",     mageco_dev_save,      mageco_dev_load },
    { "microdisc", microdisc_dev_claims, microdisc_dev_read, microdisc_dev_write, NULL, NULL, NULL, NULL },
    { "dtl2000",   dtl2000_dev_claims,   dtl2000_dev_read,   dtl2000_dev_write,   NULL,
      "DTL\0",     dtl2000_dev_save,     dtl2000_dev_load },
    /* ULA-NG en dernier (repli avant VIA). claims_write distinct : voit les
     * écritures de sa fenêtre même verrouillée (guet 'N','G'). Sérialisée via la
     * section "UNG" (émise seulement si déverrouillée → .ost inchangé sinon). */
    { "ula-ng",    ula_ng_dev_claims,    ula_ng_dev_read,    ula_ng_dev_write,    ula_ng_dev_claims_write,
      "UNG\0",     ula_ng_dev_save,      ula_ng_dev_load },
};
static const int io_bus_count = (int)(sizeof(io_bus) / sizeof(io_bus[0]));

const io_device_t* io_bus_find(emulator_t* emu, uint16_t addr) {
    for (int i = 0; i < io_bus_count; i++)
        if (io_bus[i].claims(emu, addr))
            return &io_bus[i];
    return NULL;
}

const io_device_t* io_bus_find_write(emulator_t* emu, uint16_t addr) {
    for (int i = 0; i < io_bus_count; i++) {
        bool (*cw)(emulator_t*, uint16_t) = io_bus[i].claims_write ? io_bus[i].claims_write
                                                                   : io_bus[i].claims;
        if (cw(emu, addr))
            return &io_bus[i];
    }
    return NULL;
}

const io_device_t* io_bus_devices(int* count) {
    if (count) *count = io_bus_count;
    return io_bus;
}

/* Tick des périphériques de bus temporisés. ORDRE HISTORIQUE PRÉSERVÉ à
 * l'identique de l'ancien cpu_cycle_tick (microdisc → loci → acia → dtl → mageco)
 * → iso-comportement par construction (et non par la simple indépendance des
 * ticks). Une boucle générique sur `io_bus[]` réordonnerait ; on ne le fait donc
 * pas ici (cf. docs/architecture/io-bus.md §6 : hooks lifecycle non uniformes). */
void io_bus_tick(emulator_t* emu, int cycles) {
    if (emu->has_microdisc) fdc_ticktock(&emu->microdisc.fdc, cycles);
    if (emu->has_loci) {
        fdc_ticktock(&emu->loci.dsk_fdc, cycles);
        loci_adj_tick(&emu->loci, cycles);
    }
    if (emu->has_serial) {
        acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
        acia_tick(&emu->acia, cycles);
    }
    if (emu->has_dtl2000) dtl2000_tick(&emu->dtl2000, cycles);
    if (emu->has_mageco)  mageco_tick(&emu->mageco, cycles);
}
