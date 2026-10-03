/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_demo.c
 * @brief Carte d'exemple : le modèle à copier pour écrire une carte
 *        d'extension (docs/CARTES.md). Hors de la build par défaut.
 * @author bmarty <bmarty@mailo.com>
 *
 * Une carte fictive à un registre, en $03D0 par défaut :
 *   - écrire y range l'octet ; le lire rend son complément (octet ^ $FF) ;
 *   - elle compte les cycles CPU depuis son activation (tick) ;
 *   - son état passe dans les sauvegardes .ost (section « DMO »).
 * Pour l'essayer : copier ce fichier dans src/cards/, ajouter la ligne
 * `X(demo, 1)` à CARD_MODULE_LIST (include/cards_list.h), recompiler, puis
 * lancer avec --demo. `make test-card-template` fait exactement cela dans une
 * copie de l'arbre et vérifie le résultat.
 */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>   /* offsetof */

/* ── État, privé au module (une seule machine par processus) ──────────── */

typedef struct {
    uint16_t base;       /* adresse du registre */
    uint8_t  value;      /* dernier octet écrit */
    uint64_t cycles;     /* cycles CPU depuis l'activation */
} demo_state_t;

static demo_state_t s_dev;

/* ── Configuration et options ──────────────────────────────────────────── */

typedef struct {
    bool     enabled;    /* --demo */
    uint16_t base;       /* --demo-addr ADDR (hex) */
} demo_cfg_t;

static void cfg_defaults(void* p) {
    demo_cfg_t* c = p;
    c->enabled = false;
    c->base = 0x03D0;
}
static void opt_enable(void* p, const char* arg) { (void)arg; ((demo_cfg_t*)p)->enabled = true; }
static void opt_addr(void* p, const char* arg) {
    ((demo_cfg_t*)p)->base = (uint16_t)strtol(arg, NULL, 16);
}

static const card_opt_t k_opts[] = {
    { "demo",      no_argument,       opt_enable },
    { "demo-addr", required_argument, opt_addr },
};

/* Aide : sans ancre, elle va en fin de liste des options. */
static const char k_help[] =
    "      --demo                 Enable the demo card (one register at $03D0)\n"
    "      --demo-addr ADDR       Demo card register address in hex (default 03D0)\n";
static const card_help_t k_helps[] = { { k_help, NULL } };

/* Fiche du menu F1 : id, nom, rôle, groupe, option d'activation, paramètres. */
static const card_desc_t k_desc = {
    "demo", "Carte d'exemple",
    "Modèle pour écrire une carte : un registre qui rend le complément de ce "
    "qu'on y écrit.",
    NULL, "--demo", -1, 0, 0, 1, false,
    { { "adresse", "Adresse d'E/S", CARD_P_HEX, "--demo-addr", "03D0",
        "Adresse du registre (03D0 par défaut)." } },
    1
};
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Bus ───────────────────────────────────────────────────────────────── */

static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_demo] && addr == s_dev.base;
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    (void)emu; (void)addr;
    return (uint8_t)(s_dev.value ^ 0xFF);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    (void)emu; (void)addr;
    s_dev.value = value;
    return true;          /* écriture consommée (sinon : repli sur le VIA) */
}
/* Section « DMO » : émise seulement si la carte est présente (.ost inchangé sinon). */
static bool dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_demo]) return false;
    return fwrite(&s_dev, sizeof(s_dev), 1, fp) == 1;
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    (void)emu;
    if (size == sizeof(s_dev) && fread(&s_dev, sizeof(s_dev), 1, fp) != 1)
        log_error("demo: section DMO illisible");
}
/* Appelée à chaque cycle si la carte est présente (X(demo, 1) dans la liste). */
void card_demo_tick(emulator_t* emu, int cycles) {
    (void)emu;
    s_dev.cycles += (uint64_t)cycles;
}

static const io_device_t k_bus = {
    .name = "demo", .claims = dev_claims, .read = dev_read, .write = dev_write,
    .save_tag = "DMO\0", .save = dev_save, .load = dev_load,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_demo]), .tick = card_demo_tick,
};

/* ── Mise en route ─────────────────────────────────────────────────────── */

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)core;
    const demo_cfg_t* cfg = p;
    if (!cfg->enabled) return 0;
    s_dev.base = cfg->base;
    s_dev.value = 0;
    s_dev.cycles = 0;
    emu->card_on[CARD_IDX_demo] = true;
    log_info("Demo card enabled at $%04X", cfg->base);
    return 0;
}

/* Ancres à NULL : une nouvelle carte se place en fin d'options, d'aide, de menu
 * et de bus (les cartes historiques gardent leur place grâce aux leurs). */
const card_module_t card_demo = {
    .descs = k_descs, .ndescs = 1,
    .opts = k_opts, .nopts = 2,
    .helps = k_helps, .nhelps = 1,
    .cfg_size = sizeof(demo_cfg_t), .cfg_defaults = cfg_defaults,
    .stage = CARD_STAGE_SPEECH, .setup = setup,
    .bus = &k_bus,
};
