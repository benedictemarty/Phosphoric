/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_ula_ng.c
 * @brief ULA-NG en module : fiche du menu (toujours présente), option
 *        --ula-ng-poke et accès au bus $0340-$035F (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * Son état (emu->ula_ng) reste dans la machine : la vidéo et l'horloge le lisent
 * directement (palette, modes, raster). Le module porte ce qui la rend
 * « carte » : menu, option, registres.
 */
#include "card_module.h"
#include "emulator.h"
#include "cpu/cpu6502.h"
#include "io/ula_ng.h"
#include "utils/logging.h"
#include <stdio.h>
#include <string.h>

/* ── Configuration et option ───────────────────────────────────────────── */

typedef struct {
    const char* poke;   /* --ula-ng-poke "AAA=VV,..." (registres $0340-$035F) */
} ula_ng_cfg_t;

static void opt_poke(void* p, const char* arg) { ((ula_ng_cfg_t*)p)->poke = arg; }

static const card_opt_t k_opts[] = {
    { "ula-ng-poke", required_argument, opt_poke },
};

static const char k_help[] =
    "      --ula-ng-poke SEQ      Program ULA-NG registers ($0340-$035F) at startup,\n"
    "                             SEQ = comma-separated AAA=VV hex pairs (see docs/ula-ng).\n"
    "                             Ex: 340=4E,340=47,341=01,348=07,349=00,34A=F0 (palette)\n";

static const card_help_t k_helps[] = { { k_help, "type-keys" } };

static const card_desc_t k_desc = {
    "ula_ng", "ULA-NG",
    "ULA de nouvelle génération (palette, modes étendus), toujours présente ; "
    "un programme la déverrouille par $0340.",
    NULL, NULL, -1, -1, 0x0340, 32, true, { { 0 } }, 0
};
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Bus $0340-$035F : dernier périphérique, avant le repli VIA ──────────
 *  - Lecture : ne répond que déverrouillée (`claims`) ; verrouillée, la fenêtre
 *    retombe sur le miroir VIA (indiscernable).
 *  - Écriture : `claims_write` = fenêtre seule → l'ULA-NG voit les écritures
 *    même verrouillée pour guetter la séquence 'N','G'. `ula_ng_write` renvoie
 *    si elle a consommé ; sinon le dispatch retombe sur le VIA (bit-à-bit). */
static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return ula_ng_active(&emu->ula_ng) && ula_ng_addr_in_window(addr);
}
static bool dev_claims_write(emulator_t* emu, uint16_t addr) {
    (void)emu;
    return ula_ng_addr_in_window(addr);
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    return ula_ng_read(&emu->ula_ng, addr);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (!ula_ng_write(&emu->ula_ng, addr, value))
        return false;   /* non consommée (verrouillée, octet neutre) → repli VIA */
    /* Écriture consommée : synchroniser la ligne d'IRQ raster (un write de
     * NG_STATUS acquitte → désassertion). */
    if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
    else                          cpu_irq_clear(&emu->cpu, IRQF_ULANG);
    return true;
}
/* Section « UNG » : déléguée au module (POD, même build, garde de taille) ;
 * émise seulement si déverrouillée (.ost inchangé sinon). */
static bool dev_save(emulator_t* emu, FILE* fp) {
    return ula_ng_save(&emu->ula_ng, fp);
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    ula_ng_load(&emu->ula_ng, fp, size);
}

/* Pas de tick : l'ULA-NG avance avec la vidéo (emu_clock.c). */
static const io_device_t k_bus = {
    .name = "ula-ng", .claims = dev_claims, .read = dev_read,
    .write = dev_write, .claims_write = dev_claims_write,
    .save_tag = "UNG\0", .save = dev_save, .load = dev_load,
};

/* ── Mise en route ─────────────────────────────────────────────────────── */

/* --ula-ng-poke "AAA=VV,..." : programme directement les registres ULA-NG
 * ($0340-$035F) au démarrage (déverrouillage, palette, copper, raster…),
 * sans passer par des POKE BASIC lents. Idéal pour démos/tests/captures. */
static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)core;
    const ula_ng_cfg_t* cfg = p;
    if (!cfg->poke) return 0;
    const char* s = cfg->poke;
    int n = 0;
    while (*s) {
        unsigned addr = 0, val = 0;
        if (sscanf(s, "%x=%x", &addr, &val) == 2 &&
            ula_ng_addr_in_window((uint16_t)addr)) {
            ula_ng_write(&emu->ula_ng, (uint16_t)addr, (uint8_t)val);
            n++;
        }
        const char* comma = strchr(s, ',');
        if (!comma) break;
        s = comma + 1;
    }
    log_info("ULA-NG: %d register write(s) applied from --ula-ng-poke", n);
    return 0;
}

const card_module_t card_ula_ng = {
    .descs = k_descs, .ndescs = 1, .desc_before = NULL,
    .opts = k_opts, .nopts = 1, .opts_before = "control",
    .helps = k_helps, .nhelps = 1,
    .cfg_size = sizeof(ula_ng_cfg_t),
    .stage = CARD_STAGE_MACHINE, .setup = setup,
    .bus = &k_bus, .bus_before = NULL,
};
