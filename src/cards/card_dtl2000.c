/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_dtl2000.c
 * @brief Carte Digitelec DTL 2000 (PIA 6821 + ACIA 6850, modem V23) en module :
 *        menu, options, mise en route, bus et fermeture (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 */
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "cli/cli_parse.h"     /* parse_hex16 */
#include "cpu/cpu6502.h"
#include "io/dtl2000.h"
#include "io/serial_backend.h"
#include "utils/logging.h"
#include <stdio.h>
#include <stddef.h>   /* offsetof */

/* État de la carte, privé au module (une seule machine par processus :
 * emulator_init n'est appelé qu'une fois, par main). */
static dtl2000_t s_dev;
static serial_backend_t* s_backend;   /* transport ouvert par setup */

/* ── Configuration et options ──────────────────────────────────────────── */

typedef struct {
    const char* transport;   /* --dtl2000 TRANSPORT */
    const char* addr;        /* --dtl2000-addr ADDR (hex) */
} dtl2000_cfg_t;

static void opt_transport(void* p, const char* arg) { ((dtl2000_cfg_t*)p)->transport = arg; }
static void opt_addr(void* p, const char* arg) { ((dtl2000_cfg_t*)p)->addr = arg; }

static const card_opt_t k_opts[] = {
    { "dtl2000",      required_argument, opt_transport },
    { "dtl2000-addr", required_argument, opt_addr },
};

static const char k_help[] =
    "      --dtl2000 TRANSPORT   Digitelec DTL 2000 (PIA 6821 + ACIA 6850) at $03F8\n"
    "                            Transports (raw V23 line): loopback, tcp:H:P, pty, com:B,D,P,S,DEV, file:IN[:OUT]\n"
    "      --dtl2000-addr ADDR   DTL 2000 base address in hex (default: 03F8)\n";

/* Explications courtes, lues dans le menu (cf. cards.c). */
#define TRANSPORTS_SERIE \
    "loopback (écho local), tcp:hôte:port, modem:hôte:port (appels entrants), " \
    "pty (pseudo-terminal), com:bauds,bits,parité,stop,périphérique (port série " \
    "réel), file:entrée[:sortie]"

static const card_help_t k_helps[] = { { k_help, "mageco" } };

static const card_desc_t k_desc = {
    "dtl2000", "DTL 2000",
    "Modem Digitelec DTL 2000 (PIA 6821 + ACIA 6850) : ligne V23 brute vers un "
    "serveur Minitel.",
    NULL, "--dtl2000", 0, 1, 0, 6, false,
    { { "transport", "Ligne V23", CARD_P_TEXT, "--dtl2000", "loopback",
        "Où va la ligne : " TRANSPORTS_SERIE "." },
      { "adresse", "Adresse d'E/S", CARD_P_HEX, "--dtl2000-addr", "03F8",
        "Adresse de base de la carte (03F8 par défaut)." } },
    2
};
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Interruptions (ACIA 6850 de la carte) ─────────────────────────────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_DTL2000); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_DTL2000); }

static void wire_irq(emulator_t* emu) {
    s_dev.irq_set = irq_set;
    s_dev.irq_clr = irq_clr;
    s_dev.irq_userdata = emu;
}

/* ── Bus : $03F8-$03FD (plage exclusive) ───────────────────────────────── */

static bool dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->card_on[CARD_IDX_dtl2000] && dtl2000_addr_in_range(&s_dev, addr);
}
static uint8_t dev_read(emulator_t* emu, uint16_t addr) {
    return dtl2000_read(&s_dev, addr);
}
static bool dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    dtl2000_write(&s_dev, addr, value);
    return true;
}
/* Section « DTL » : émise seulement si la carte est présente (.ost inchangé sinon). */
static bool dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->card_on[CARD_IDX_dtl2000]) return false;
    return dtl2000_save(&s_dev, fp);
}
static void dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    dtl2000_load(&s_dev, fp, size);
}
void card_dtl2000_tick(emulator_t* emu, int cycles) { dtl2000_tick(&s_dev, cycles); }

static const io_device_t k_bus = {
    .name = "dtl2000", .claims = dev_claims, .read = dev_read, .write = dev_write,
    .save_tag = "DTL\0", .save = dev_save, .load = dev_load,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_dtl2000]), .tick = card_dtl2000_tick,
};

/* ── Cycle de vie ──────────────────────────────────────────────────────── */

/* Au démarrage, présente ou non : adresse par défaut, IRQ câblées. */
static void init(emulator_t* emu) {
    dtl2000_init(&s_dev, DTL2000_DEFAULT_BASE);
    wire_irq(emu);
}

/* Modem fidèle (PIA 6821 + ACIA 6850). Le transport réutilise les backends
 * série génériques. */
static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    const dtl2000_cfg_t* cfg = p;
    if (!cfg->transport) return 0;
    uint16_t base = DTL2000_DEFAULT_BASE;
    if (cfg->addr) {
        base = parse_hex16(cfg->addr);
    }
    if (emu->has_microdisc) {
        log_warning("DTL 2000 at $%04X shares page 3 with the disc electronics "
                    "(Jasmin) — not faithful to coexist on real hardware", base);
    }
    /* The DTL card accepts the same *transparent* transports as --serial
     * (loopback/tcp/pty/com) — raw byte pipes for the V23 line. The DTL 2000
     * is dialled by its PIA 6821 line bit and carries raw data, so the
     * protocol-injecting backends (Hayes modem, digitelec, picowifi) are
     * intentionally excluded: a Hayes AT layer behind the DTL would be
     * unfaithful (the host software never issues AT commands). */
    serial_backend_t* db = serial_transport_create(cfg->transport);
    if (!db) {
        log_error("Unknown DTL 2000 transport: %s", cfg->transport);
        log_error("  loopback, tcp:host:port, pty, com:baud,bits,P,stop,device, file:in[:out]");
        log_error("  (the DTL is dialled via its PIA, not Hayes AT — no 'modem')");
        return 1;
    }
    if (db->open(db)) {
        dtl2000_init(&s_dev, base);
        /* dtl2000_init() zeroes the struct — re-wire the CPU IRQ hooks */
        wire_irq(emu);
        dtl2000_set_backend(&s_dev, db);
        s_backend = db;
        emu->card_on[CARD_IDX_dtl2000] = true;
        if (emu->serial_trace_file) {
            dtl2000_set_trace(&s_dev, emu->serial_trace_file);
        }
        log_info("Digitelec DTL 2000 enabled at $%04X (transport: %s)",
                 base, cfg->transport);
    } else {
        log_error("Failed to open DTL 2000 transport: %s", cfg->transport);
        serial_backend_destroy(db);
    }
    return 0;
}

static void teardown(emulator_t* emu) {
    if (s_backend) {
        serial_backend_destroy(s_backend);
        s_backend = NULL;
        emu->card_on[CARD_IDX_dtl2000] = false;
    }
}

const card_module_t card_dtl2000 = {
    .descs = k_descs, .ndescs = 1, .desc_before = "mageco",
    .opts = k_opts, .nopts = 2, .opts_before = "mageco",
    .helps = k_helps, .nhelps = 1,
    .cfg_size = sizeof(dtl2000_cfg_t),
    .init = init, .stage = CARD_STAGE_SERIAL, .setup = setup, .teardown = teardown,
    .bus = &k_bus, .bus_before = "ula-ng",
};
