/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_sp0256.c
 * @brief SP0256 card (Mageco speech synth, GI SP0256-AL2) as a module:
 *        menu, options, setup, bus and sound (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "io/sp0256.h"
#include "audio/audio.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>   /* offsetof */

/* ── Configuration and options ─────────────────────────────────────────── */

typedef struct {
    const char* rom_file;    /* --sp0256-rom FILE */
    uint16_t    base_addr;   /* --sp0256-addr (hex) */
} sp0256_cfg_t;

static void cfg_defaults(void* p) {
    sp0256_cfg_t* c = p;
    c->rom_file = NULL;
    c->base_addr = SP0256_BASE_DEFAULT;
}
static void opt_rom(void* p, const char* arg) { ((sp0256_cfg_t*)p)->rom_file = arg; }
static void opt_addr(void* p, const char* arg) {
    ((sp0256_cfg_t*)p)->base_addr = (uint16_t)strtol(arg, NULL, 16);
}

static const card_opt_t k_opts[] = {
    { "sp0256-rom",  required_argument, opt_rom },
    { "sp0256-addr", required_argument, opt_addr },
};

static const char k_help[] =
    "      --sp0256-rom FILE      Enable the Mageco speech synth (GI SP0256-AL2 at $03F1);\n"
    "                             loads the 2 KB allophone ROM (sp0256-al2.bin). Speech is\n"
    "                             mixed into the PSG audio (Frelon, Cobra Pinball, …).\n"
    "      --sp0256-addr ADDR     SP0256 I/O address in hex (default 03F1)\n";

static const card_desc_t k_desc = {
    "sp0256", "SP0256",
    "Synthétiseur vocal Mageco (GI SP0256-AL2, allophones), mixé au son de "
    "l'Oric.",
    NULL, "--sp0256-rom", 0, 1, 0, 1, false,
    { { "rom", "ROM d'allophones", CARD_P_FILE, "--sp0256-rom", "roms/al2.bin",
        "ROM du SP0256-AL2 (al2.bin) : les 64 sons de base de la parole." },
      { "adresse", "Adresse d'E/S", CARD_P_HEX, "--sp0256-addr", "03F1",
        "Adresse du port (03F1 par défaut)." } },
    2
};
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Bus: single port at base_addr (default $03F1) ─────────────────────── */

static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_sp0256] && addr == emu->sp0256.base_addr;
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    return sp0256_read(&emu->sp0256, addr);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    sp0256_write(&emu->sp0256, addr, value);
    return true;
}
static bool dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_sp0256]) return false;
    return sp0256_save(&emu->sp0256, fp);
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    sp0256_load(&emu->sp0256, fp, size);
}
void card_sp0256_tick(emulator_t* emu, int cycles) { sp0256_tick(&emu->sp0256, cycles); }

static const io_device_t k_bus = {
    .name = "sp0256", .claims = dev_claims, .read = dev_read, .write = dev_write,
    .save_tag = "SPO\0", .save = dev_save, .load = dev_load,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_sp0256]), .tick = card_sp0256_tick,
};

/* ── Sound and setup ───────────────────────────────────────────────────── */

/* Mixed only with a valid ROM (as before: nothing produced otherwise). */
static bool audio_gen(void* ctx, int16_t* out, int n) {
    sp0256_t* sp = ctx;
    if (!sp->rom_valid) return false;
    sp0256_generate(sp, out, n);
    return true;
}

/* GI SP0256-AL2 speech chip at $03F1 (or --sp0256-addr). Loads the 2 KB
 * allophone ROM; output is mixed into the PSG audio. Used by Frelon, Cobra Pinball, … */
static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    (void)core;
    const sp0256_cfg_t* cfg = p;
    if (!cfg->rom_file) return 0;
    FILE* sf = fopen(cfg->rom_file, "rb");
    if (!sf) {
        log_error("Failed to open SP0256 ROM: %s", cfg->rom_file);
        return 1;
    }
    uint8_t sbuf[SP0256_ROM_SIZE];
    size_t srd = fread(sbuf, 1, SP0256_ROM_SIZE, sf);
    fclose(sf);

    sp0256_init(&emu->sp0256, cfg->base_addr);
    if (srd != SP0256_ROM_SIZE || !sp0256_load_rom(&emu->sp0256, sbuf, (uint32_t)srd)) {
        log_error("SP0256 ROM must be exactly %d bytes (got %zu): %s",
                  SP0256_ROM_SIZE, srd, cfg->rom_file);
        return 1;
    }
    emu->sp0256.emu = emu;
    emu->card_on[CARD_IDX_sp0256] = true;
    audio_add_source(audio_gen, &emu->sp0256);   /* mixed into the Oric's sound */
    log_info("SP0256 Mageco speech synthesizer enabled at $%04X (SP0256-AL2)",
             cfg->base_addr);
    return 0;
}

const card_module_t card_sp0256 = {
    .descs = k_descs, .ndescs = 1, .desc_before = "mea8000",
    .opts = k_opts, .nopts = 2, .opts_before = "mea8000",
    .help = k_help, .help_before = "mea8000",
    .cfg_size = sizeof(sp0256_cfg_t), .cfg_defaults = cfg_defaults,
    .stage = CARD_STAGE_SPEECH, .setup = setup,
    .bus = &k_bus, .bus_before = "mea8000",
};
