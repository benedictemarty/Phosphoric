/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_mea8000.c
 * @brief MEA8000 card (TMPI speech synth, Philips MEA 8000) as a module:
 *        menu, options, setup, bus and sound (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "io/mea8000.h"
#include "audio/audio.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>   /* offsetof */

/* ── Configuration and options ─────────────────────────────────────────── */

typedef struct {
    bool     enabled;     /* --mea8000 (TMPI, no ROM) */
    uint16_t base_addr;   /* --mea8000-addr (hex) */
} mea8000_cfg_t;

static void cfg_defaults(void* p) {
    mea8000_cfg_t* c = p;
    c->enabled = false;
    c->base_addr = MEA8000_BASE_DEFAULT;
}
static void opt_enable(void* p, const char* arg) { (void)arg; ((mea8000_cfg_t*)p)->enabled = true; }
static void opt_addr(void* p, const char* arg) {
    ((mea8000_cfg_t*)p)->base_addr = (uint16_t)strtol(arg, NULL, 16);
}

static const card_opt_t k_opts[] = {
    { "mea8000",      no_argument,       opt_enable },
    { "mea8000-addr", required_argument, opt_addr },
};

static const char k_help[] =
    "      --mea8000              Enable the TMPI speech synth (Philips MEA 8000 formant\n"
    "                             chip at $03FE/$03FF). No ROM (host streams frames);\n"
    "                             speech mixed into the PSG. Exclusive with --sp0256-rom.\n"
    "      --mea8000-addr ADDR    MEA8000 base I/O address in hex (default 03FE)\n";

static const card_desc_t k_desc = {
    "mea8000", "MEA8000",
    "Synthétiseur vocal TMPI (Philips MEA 8000, synthèse par formants, sans "
    "ROM).",
    NULL, "--mea8000", -1, 0, 0, 2, false,
    { { "adresse", "Adresse d'E/S", CARD_P_HEX, "--mea8000-addr", "03FE",
        "Adresse de base (03FE par défaut ; Mageco MIDI utilise aussi 03FE)." } },
    1
};

/* ── Bus: data at base_addr, command at base_addr+1 ────────────────────── */

static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_mea8000] &&
           (addr == emu->mea8000.base_addr ||
            addr == (uint16_t)(emu->mea8000.base_addr + 1));
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    return mea8000_read(&emu->mea8000, addr);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    mea8000_write(&emu->mea8000, addr, value);
    return true;
}
static bool dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_mea8000]) return false;
    return mea8000_save(&emu->mea8000, fp);
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    mea8000_load(&emu->mea8000, fp, size);
}
void card_mea8000_tick(emulator_t* emu, int cycles) { mea8000_tick(&emu->mea8000, cycles); }

static const io_device_t k_bus = {
    .name = "mea8000", .claims = dev_claims, .read = dev_read, .write = dev_write,
    .save_tag = "MEA\0", .save = dev_save, .load = dev_load,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_mea8000]), .tick = card_mea8000_tick,
};

/* ── Sound and setup ───────────────────────────────────────────────────── */

static bool audio_gen(void* ctx, int16_t* out, int n) { mea8000_generate(ctx, out, n); return true; }

/* Philips/Signetics formant chip at $03FE/$03FF (TMPI card, confirmed in-game
 * with SYNTHOR; configurable address). No ROM: the host streams the frames.
 * Mutually exclusive with the SP0256 card (two speech synths). */
static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)core;
    const mea8000_cfg_t* cfg = p;
    if (!cfg->enabled) return 0;
    if (emu->card_on[CARD_IDX_sp0256]) {
        log_error("--mea8000 and --sp0256-rom are mutually exclusive "
                  "(both are speech cards)");
        return 1;
    }
    /* Default $03FE/$03FF overlaps the Mageco MIDI interface ($03FE-$03FF).
     * Relocate one of them (--mea8000-addr / --mageco-addr) to coexist. */
    if (emu->card_on[CARD_IDX_mageco] && cfg->base_addr >= 0x03FE) {
        log_error("--mea8000 (default $03FE/$03FF) overlaps the Mageco MIDI "
                  "interface — relocate with --mea8000-addr or --mageco-addr");
        return 1;
    }
    mea8000_init(&emu->mea8000, cfg->base_addr);
    emu->mea8000.emu = emu;
    emu->card_on[CARD_IDX_mea8000] = true;
    audio_add_source(audio_gen, &emu->mea8000);
    log_info("MEA8000 TMPI speech synthesizer enabled at $%04X/$%04X (formant, no ROM)",
             cfg->base_addr, (uint16_t)(cfg->base_addr + 1));
    return 0;
}

static const card_desc_t* const k_descs[] = { &k_desc };

const card_module_t card_mea8000 = {
    .descs = k_descs, .ndescs = 1, .desc_before = "hostfs",
    .opts = k_opts, .nopts = 2, .opts_before = "breakpoint",
    .help = k_help, .help_before = "disk-writeback",
    .cfg_size = sizeof(mea8000_cfg_t), .cfg_defaults = cfg_defaults,
    .stage = CARD_STAGE_SPEECH, .setup = setup,
    .bus = &k_bus, .bus_before = "dtl2000",
};
