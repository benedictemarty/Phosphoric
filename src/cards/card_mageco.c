/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_mageco.c
 * @brief Mageco MIDI interface and its ORICON variant (MC6850 ACIA) as a module:
 *        two menu entries, one chip; options, setup, bus and
 *        teardown (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "cli/cli_opts.h"      /* --serial-trace */
#include "cli/cli_parse.h"     /* parse_hex16 */
#include "cpu/cpu6502.h"
#include "io/mageco.h"
#include "io/serial_backend.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stddef.h>   /* offsetof */

/* État de la carte, privé au module (une seule machine par processus :
 * emulator_init n'est appelé qu'une fois, par main). */
static mageco_t s_dev;
static serial_backend_t* s_backend;   /* transport ouvert par setup */

/* ── Configuration et options ──────────────────────────────────────────── */

typedef struct {
    const char* transport;   /* --mageco / --oricon TRANSPORT */
    const char* addr;        /* --mageco-addr ADDR (hex) */
    bool        oricon;      /* --oricon: ORICON variant */
} mageco_cfg_t;

static void opt_mageco(void* p, const char* arg) { ((mageco_cfg_t*)p)->transport = arg; }
static void opt_addr(void* p, const char* arg) { ((mageco_cfg_t*)p)->addr = arg; }
static void opt_oricon(void* p, const char* arg) {
    ((mageco_cfg_t*)p)->transport = arg;
    ((mageco_cfg_t*)p)->oricon = true;
}

static const card_opt_t k_opts[] = {
    { "mageco",      required_argument, opt_mageco },
    { "mageco-addr", required_argument, opt_addr },
    { "oricon",      required_argument, opt_oricon },
};

static const char k_help[] =
    "      --mageco TRANSPORT    Mageco MIDI interface (ACIA 6850) at $03FE, 31250 baud\n"
    "                            Transports (raw MIDI bytes): file:IN[:OUT], midi[:TARGET], smf:FILE[:loop], loopback, tcp:H:P, pty\n"
    "                            file::out.mid captures Oric MIDI OUT ; midi = live ALSA port (MIDI=1) ; smf:song.mid replays a .mid into the Oric\n"
    "      --mageco-addr ADDR    Mageco base address in hex (default: 03FE)\n"
    "      --oricon TRANSPORT    ORICON MIDI variant (MC6850 at $031C-$031D + clock gen $031E-$031F, LOCI-compat)\n"
    "                            Same transports as --mageco ; overlaps --serial/Microdisc at $031C\n";

static const card_desc_t k_desc_mageco = {
    "mageco", "Mageco MIDI",
    "Interface MIDI Mageco (ACIA 6850 à 31250 bauds) : piloter un synthétiseur "
    "ou jouer un fichier .mid dans l'Oric.",
    "midi", "--mageco", 0, 1, 0, 2, false,
    { { "transport", "Liaison MIDI", CARD_P_TEXT, "--mageco", "loopback",
        "loopback, midi[:cible] (MIDI temps réel, build MIDI=1), "
        "smf:fichier.mid[:loop] (rejoue un fichier MIDI), tcp:hôte:port, "
        "file:entrée[:sortie]." },
      { "adresse", "Adresse d'E/S", CARD_P_HEX, "--mageco-addr", "03FE",
        "Adresse de base (03FE par défaut ; la MEA8000 utilise aussi 03FE)." } },
    2
};
static const card_desc_t k_desc_oricon = {
    "oricon", "ORICON",
    "Variante MIDI ORICON (MC6850 en $031C-$031D, générateur d'horloge "
    "$031E-$031F, compatible LOCI).",
    "midi", "--oricon", 0, -1, 0x031C, 4, false,
    { { "transport", "Liaison MIDI", CARD_P_TEXT, "--oricon", "loopback",
        "loopback, midi[:cible], smf:fichier.mid[:loop], tcp:hôte:port, "
        "file:entrée[:sortie]." } },
    1
};
static const card_desc_t* const k_descs[] = { &k_desc_mageco, &k_desc_oricon };

/* ── Interrupts (the card's ACIA 6850) ─────────────────────────────────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_MAGECO); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_MAGECO); }

static void wire_irq(emulator_t* emu) {
    s_dev.irq_set = irq_set;
    s_dev.irq_clr = irq_clr;
    s_dev.irq_userdata = emu;
}

/* ── Bus: $03FE-$03FF or $031C-$031E ───────────────────────────────────── */

static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_mageco] && mageco_addr_in_range(&s_dev, addr);
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    return mageco_read(&s_dev, addr);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    mageco_write(&s_dev, addr, value);
    return true;
}
/* Savestate ("MAG" section): emitted only if the Mageco is present →
 * .ost unchanged otherwise. Host transport not restored (see mageco_save). */
static bool dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_mageco]) return false;
    return mageco_save(&s_dev, fp);
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    mageco_load(&s_dev, fp, size);
}
void card_mageco_tick(emulator_t* emu, int cycles) { mageco_tick(&s_dev, cycles); }

static const io_device_t k_bus = {
    .name = "mageco", .claims = dev_claims, .read = dev_read, .write = dev_write,
    .save_tag = "MAG\0", .save = dev_save, .load = dev_load,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_mageco]), .tick = card_mageco_tick,
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* At startup, present or not: default address, IRQs wired. */
static void init(emulator_t* emu) {
    mageco_init(&s_dev, MAGECO_DEFAULT_BASE);
    wire_irq(emu);
}

/* Mageco / ORICON MIDI interface — MC6850 ACIA (forum t=2525).
 *   --mageco : original Mageco card, 6850 at $03FE-$03FF (thread p.1).
 *   --oricon : modern ORICON reboot (iss), 6850 at $031C-$031D + clock
 *              generator at $031E-$031F, LOCI-compatible decoding (p.3).
 * Both reuse the transparent serial backends: file: captures/replays the
 * raw MIDI stream, smf: plays a .mid into the Oric, midi: bridges a live
 * host MIDI port. The byte stream is identical to the real card. */
static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    const mageco_cfg_t* cfg = p;
    if (!cfg->transport) return 0;
    uint16_t base = cfg->oricon ? MAGECO_ORICON_BASE : MAGECO_DEFAULT_BASE;
    if (cfg->addr) {
        base = parse_hex16(cfg->addr);
    }
    const char* mode = cfg->oricon ? "ORICON" : "Mageco";
    if (emu->has_microdisc) {
        log_warning("%s MIDI at $%04X shares page 3 with the disc electronics "
                    "— possible clash with other extensions (forum t=2525)",
                    mode, base);
    }
    if (cfg->oricon && emu->has_serial && base == emu->acia_base_addr) {
        log_warning("ORICON at $%04X overlaps the ACIA 6551 serial (--serial) "
                    "— disable one of them", base);
    }
    serial_backend_t* mb = serial_transport_create(cfg->transport);
    if (!mb) {
        log_error("Unknown %s transport: %s", mode, cfg->transport);
        log_error("  file:in[:out], smf:FILE[:loop], midi[:TARGET], loopback, tcp:host:port, pty");
        return 1;
    }
    if (mb->open(mb)) {
        if (cfg->oricon) mageco_init_oricon(&s_dev, base);
        else             mageco_init(&s_dev, base);
        /* mageco_init*() zeroes the struct — re-wire the CPU IRQ hooks */
        wire_irq(emu);
        mageco_set_backend(&s_dev, mb);
        s_backend = mb;
        emu->card_on[CARD_IDX_mageco] = true;
        if (core->serial_trace_file) {
            mageco_set_trace(&s_dev, core->serial_trace_file);
        }
        log_info("%s MIDI enabled at $%04X (31250 baud, transport: %s)",
                 mode, base, cfg->transport);
    } else {
        log_error("Failed to open %s transport: %s", mode, cfg->transport);
        serial_backend_destroy(mb);
    }
    return 0;
}

static void teardown(emulator_t* emu) {
    if (s_backend) {
        mageco_set_trace(&s_dev, NULL);
        serial_backend_destroy(s_backend);
        s_backend = NULL;
        emu->card_on[CARD_IDX_mageco] = false;
    }
}

const card_module_t card_mageco = {
    .descs = k_descs, .ndescs = 2, .desc_before = "sp0256",
    .opts = k_opts, .nopts = 3, .opts_before = "dump-ram-at",
    .help = k_help, .help_before = "save-state",
    .cfg_size = sizeof(mageco_cfg_t),
    .init = init, .stage = CARD_STAGE_SERIAL, .setup = setup, .teardown = teardown,
    .bus = &k_bus, .bus_before = "microdisc",
};
