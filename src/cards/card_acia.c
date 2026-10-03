/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_acia.c
 * @brief Carte série ACIA 6551 en module : menu, options, mise en route
 *        (transports, modem Hayes, picowifi…), bus et fermeture (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * L'état de l'ACIA (emu->acia) reste dans la machine : la section « SER » des
 * sauvegardes d'état (savestate.c), le débogueur, les commandes de contrôle et
 * LOCI (ACIA en $0380) le lisent directement.
 */
#define _DEFAULT_SOURCE
#include "card_module.h"
#include "cards_list.h"
#include "emulator.h"
#include "cli/cli_opts.h"
#include "cli/cli_parse.h"     /* parse_hex16 */
#include "cpu/cpu6502.h"
#include "io/acia6551.h"
#include "io/io_bus.h"     /* loci_emu_reflect_nirq */
#include "io/loci.h"
#include "io/loci_emu.h"
#include "io/serial_backend.h"
#include "memory/memory.h"
#include "utils/logging.h"
#include "utils/netutil.h"     /* parse_host_port */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   /* offsetof */

/* ── Configuration et options ──────────────────────────────────────────── */

typedef struct {
    const char* arg;               /* --serial TYPE */
    const char* addr;              /* --acia-addr ADDR (hex) */
    bool        v23;               /* --serial-v23 */
    int         buffer_size;       /* --serial-buffer N */
    int         baud;              /* --serial-baud N */
    bool        irq_on_rdrf;       /* --serial-irq-on-rdrf */
    const char* trace_file;        /* --serial-trace FILE (aussi DTL 2000, Mageco) */
    bool        tcp_backpressure;  /* --serial-tcp-backpressure[=N] */
    int         tcp_rcvbuf;        /* N explicite (0 : auto) */
    long        loci_irq_latency_us; /* --loci-irq-latency US */
} acia_cfg_t;

#define C ((acia_cfg_t*)p)
static void opt_serial(void* p, const char* arg) { C->arg = arg; }
static void opt_v23(void* p, const char* arg) { (void)arg; C->v23 = true; }
static void opt_buffer(void* p, const char* arg) { C->buffer_size = atoi(arg); }
static void opt_baud(void* p, const char* arg) {
    C->baud = atoi(arg);
    if (C->baud < 0) C->baud = 0;
}
static void opt_irq_rdrf(void* p, const char* arg) { (void)arg; C->irq_on_rdrf = true; }
static void opt_trace(void* p, const char* arg) { C->trace_file = arg; }
static void opt_backpressure(void* p, const char* arg) {
    C->tcp_backpressure = true;
    if (arg) {
        C->tcp_rcvbuf = atoi(arg);
        if (C->tcp_rcvbuf < 0) C->tcp_rcvbuf = 0;
    }
}
static void opt_irq_latency(void* p, const char* arg) {
    C->loci_irq_latency_us = atol(arg);
    if (C->loci_irq_latency_us < 0) C->loci_irq_latency_us = 0;
}
static void opt_addr(void* p, const char* arg) { C->addr = arg; }
#undef C

/* Même ordre que l'ancienne table getopt (préfixes ambigus identiques). */
static const card_opt_t k_opts[] = {
    { "serial",                  required_argument, opt_serial },
    { "serial-v23",              no_argument,       opt_v23 },
    { "serial-buffer",           required_argument, opt_buffer },
    { "serial-baud",             required_argument, opt_baud },
    { "serial-irq-on-rdrf",      no_argument,       opt_irq_rdrf },
    { "serial-trace",            required_argument, opt_trace },
    { "serial-tcp-backpressure", optional_argument, opt_backpressure },
    { "loci-irq-latency",        required_argument, opt_irq_latency },
    { "acia-addr",               required_argument, opt_addr },
};

/* Deux blocs : les options --serial* (avant celles de la co-simulation LOCI),
 * puis --acia-addr (après les options LOCI). */
static const char k_help_serial[] =
    "      --serial TYPE          Serial: loopback, tcp:H:P, pty, modem:H:P, com:B,D,P,S,DEV, file:IN[:OUT], picowifi[:SSID[:PASS]]\n"
    "                            (digitelec:H:P is DEPRECATED — use --dtl2000 for the faithful DTL 2000 card)\n"
    "      --serial-v23          V23 mode: 1200/75 baud (Minitel/Prestel/Digitelec)\n"
    "                            (auto-enabled with digitelec backend)\n"
    "      --serial-buffer N     RX FIFO buffer N bytes (prevents overrun, default: off)\n"
    "      --serial-baud N       External-clock baud (ACIA 6551): realistic timing\n"
    "                            instead of instant transfer when baud index = 0\n"
    "      --serial-irq-on-rdrf  WDC 65C51 IRQ mode (re-trigger while RDRF set)\n"
    "      --serial-trace FILE   Serial debug trace (TX/RX/signals with timestamps)\n"
    "      --serial-tcp-backpressure[=N]  Bounded RX for tcp:: stop draining the socket\n"
    "                            when the RX FIFO is full + cap kernel SO_RCVBUF to N\n"
    "                            bytes (default N=FIFO size or 512) → real TCP flow control\n"
    "      --loci-irq-latency US LOCI I2C IRQ transport cost: defer each ACIA /IRQ\n"
    "                            by US microseconds (e.g. 10000 → ~100 B/s IRQ-driven\n"
    "                            RX cap; polling stays fast). LOCI-context only.\n";
static const char k_help_addr[] =
    "      --acia-addr ADDR      ACIA base address in hex (default: 031C)\n";
static const card_help_t k_helps[] = {
    { k_help_serial, "loci-emu" },
    { k_help_addr,   "dtl2000" },
};

/* Transports proposés dans le menu (cf. cards.c, card_dtl2000.c). */
#define TRANSPORTS_SERIE \
    "loopback (écho local), tcp:hôte:port, modem:hôte:port (appels entrants), " \
    "pty (pseudo-terminal), com:bauds,bits,parité,stop,périphérique (port série " \
    "réel), file:entrée[:sortie]"

static const card_desc_t k_desc = {
        "acia", "ACIA 6551",
        "Port série (MOS 6551) : modem, terminal, Minitel, BBS. Avec LOCI, l'ACIA de "
        "la carte est en $0380.",
        NULL, "--serial", 0, 1, 0, 4, false,
        { { "transport", "Ligne série", CARD_P_TEXT, "--serial", "loopback",
            "Où va la ligne : " TRANSPORTS_SERIE ", picowifi[:ssid[:mot_de_passe]] "
            "(modem Wi-Fi émulé)." },
          { "adresse", "Adresse d'E/S", CARD_P_HEX, "--acia-addr", "031C",
            "031C : carte série Oric standard ; 0380 : ACIA de la carte LOCI." },
          { "bauds", "Vitesse (bauds)", CARD_P_TEXT, "--serial-baud", "",
            "Cadence réaliste quand l'ACIA prend son horloge à l'extérieur ; vide : "
            "transfert instantané." },
          { "tampon", "Tampon de réception", CARD_P_TEXT, "--serial-buffer", "",
            "N octets mis en attente à l'arrivée (évite de perdre des octets) ; vide : "
            "aucun." },
          { "v23", "Mode V23 (1200/75)", CARD_P_BOOL, "--serial-v23", "non",
            "oui : vitesses asymétriques du Minitel et de Prestel." } },
        5
    };
static const card_desc_t* const k_descs[] = { &k_desc };

/* ── Interruptions ─────────────────────────────────────────────────────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_SERIAL); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_SERIAL); }

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* ACIA 6551 ($031C-$031F par défaut, base configurable). */
static bool acia_dev_claims(emulator_t* emu, uint16_t addr) {
    /* Co-sim : le firmware sert sa fenêtre ACIA dès le boot, dongle ou non
     * (sans modem : $0381 = $70). Sans ce claim, --loci-emu sans --loci-cdc
     * laissait le miroir du VIA répondre en $0380 — infidèle au matériel. */
    if (loci_emu_active() && loci_emu_acia_served(addr)) return true;
    return emu->card_on[CARD_IDX_acia] && addr >= emu->acia_base_addr && addr <= (emu->acia_base_addr + 3);
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
    return emu->card_on[CARD_IDX_loci] && emu->acia_base_addr == 0x0380 &&
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
    bool lost = emu->card_on[CARD_IDX_loci] && emu->acia_base_addr == 0x0380 &&
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

void card_acia_tick(emulator_t* emu, int cycles) {
    acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
    acia_tick(&emu->acia, cycles);
}

/* Section « SER » : écrite par savestate.c (historique), pas par le module. */
static const io_device_t k_bus = {
    .name = "acia", .claims = acia_dev_claims, .read = acia_dev_read,
    .write = acia_dev_write, .peek = acia_dev_peek,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_acia]), .tick = card_acia_tick,
};

/* ── Cycle de vie ──────────────────────────────────────────────────────── */

/* Au démarrage, présente ou non : 6551 au repos, IRQ câblées. */
static void init(emulator_t* emu) {
    acia_init(&emu->acia);
    emu->acia.irq_set = irq_set;
    emu->acia.irq_clr = irq_clr;
    emu->acia.irq_userdata = emu;
}

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    const acia_cfg_t* c = p;
    /* Lus aussi par la mise en route LOCI (picowifi) et par les cartes DTL 2000
     * et Mageco (--serial-trace). */
    emu->serial_spec = c->arg;
    emu->serial_trace_file = c->trace_file;
    /* Serial interface (ACIA 6551) */
    if (c->addr) {
        emu->acia_base_addr = parse_hex16(c->addr);
        log_info("ACIA base address: $%04X", emu->acia_base_addr);
    } else if (core->loci_enabled && c->arg) {
        /* LOCI firmware exposes its ACIA at $0380-$0383 (acia.c). Under
         * --loci, default there so LOCI client software finds it. */
        emu->acia_base_addr = 0x0380;
        log_info("ACIA base address: $0380 (LOCI default — override with --acia-addr)");
    } else if (core->loci_emu_cdc_dev) {
        /* Co-sim (--loci-cdc) : l'ACIA $0380 est servie par le VRAI firmware
         * (oric/acia.c ↔ dongle CDC) — pas de backend série comportemental. */
        emu->acia_base_addr = 0x0380;
        log_info("ACIA base address: $0380 (co-sim firmware via --loci-cdc %s)", core->loci_emu_cdc_dev);
    } else {
        emu->acia_base_addr = ACIA_DEFAULT_BASE;
    }
    /* Co-sim (--loci-cdc) : active l'ACIA sans backend (le firmware réel la sert via
     * loci_emu_acia_*). La carte doit être présente (card_on) pour que le device ACIA claim $0380. */
    if (core->loci_emu_cdc_dev && core->loci_emu_path) emu->card_on[CARD_IDX_acia] = true;
    /* Garde-fou : sous --loci, la MIA occupe $03A0-$03BF et est routée AVANT
     * l'ACIA dans les callbacks I/O. Si l'ACIA y est forcée (--acia-addr dans
     * cette plage), la MIA la masque ET pilote le PSG/clavier → le scan clavier
     * lit du vide et get_key boucle → terminal « figé » (annuaire BBS gelé).
     * Le vrai LOCI expose son modem USB-CDC à $0380, pas dans la MIA. */
    if (core->loci_enabled && c->arg &&
        emu->acia_base_addr <= LOCI_MIA_END &&
        (uint16_t)(emu->acia_base_addr + 3) >= LOCI_MIA_BASE) {
        log_warning("--acia-addr $%04X force l'ACIA dans la MIA LOCI ($%04X-$%04X) : "
                    "la MIA la masque ET casse le scan clavier (PSG) -> terminal fige.",
                    emu->acia_base_addr, LOCI_MIA_BASE, LOCI_MIA_END);
        log_warning("  Le modem LOCI (picowifi) est expose a $0380 sur le vrai LOCI : "
                    "laissez --loci SANS --acia-addr (ACIA -> $0380) et adressez $0380.");
    }
    if (c->arg) {
        /* First try the shared transparent transports (loopback/tcp/pty/com),
         * then the ACIA-6551-specific protocol backends (Hayes modem, digitelec,
         * picowifi) that inject their own command/UART layer. */
        serial_backend_t* sb = serial_transport_create(c->arg);
        if (!sb && (strcmp(c->arg, "modem") == 0 ||
                    strncmp(c->arg, "modem:", 6) == 0)) {
            /* Hayes AT modem. Modes:
             *   --serial modem              Pure command mode (use ATD to dial)
             *   --serial modem:host:port    Preset host (ATD without args connects here)
             *   --serial modem:listen:port  Server mode (ATA to accept) */
            const char* hp = (c->arg[5] == ':') ? c->arg + 6 : "";
            bool listen_mode = false;
            char host[256] = {0};
            uint16_t port = 23;
            if (strncmp(hp, "listen:", 7) == 0) {
                listen_mode = true;
                port = (uint16_t)atoi(hp + 7);
            } else {
                parse_host_port(hp, host, sizeof(host), &port, 23);
            }
            sb = serial_backend_modem_create(host, port, listen_mode);
        } else if (!sb && strncmp(c->arg, "digitelec:", 10) == 0) {
            /* digitelec:host:port — DEPRECATED behavioural model. It treats the
             * DTL 2000 as an external V23 modem hanging off the emulated ACIA
             * 6551 ($031C), which is *not* how the real card works: the actual
             * DTL 2000 is a memory-mapped PIA 6821 + ACIA 6850 at $03F8 (now
             * faithfully modelled by --dtl2000, validated against OTRM). Kept
             * functional for one cycle; steer users to the faithful option. */
            log_warning("--serial digitelec: is DEPRECATED — it models the DTL 2000 as a");
            log_warning("  6551 external modem ($031C), not the real PIA+ACIA-6850 card.");
            log_warning("  Use --dtl2000 tcp:%s for the faithful DTL 2000 card,",
                        c->arg + 10);
            log_warning("  or --serial modem:/tcp: for a generic ACIA 6551 modem.");
            char host[256];
            uint16_t port;
            parse_host_port(c->arg + 10, host, sizeof(host), &port, 23);
            sb = serial_backend_digitelec_create(host, port, &emu->acia);
        } else if (!sb && (strcmp(c->arg, "picowifi") == 0 ||
                           strncmp(c->arg, "picowifi:", 9) == 0)) {
            /* PicoWiFiModemUSB (sodiumlb) — WiFi modem exposed via LOCI.
             *   --serial picowifi                Credentials set via AT$SSID=
             *   --serial picowifi:SSID           Pre-set SSID, no password
             *   --serial picowifi:SSID:PASS      Pre-set SSID + password */
            char ssid[64] = {0};
            char pass[64] = {0};
            if (c->arg[8] == ':') {
                const char* sp = c->arg + 9;
                const char* colon = strchr(sp, ':');
                if (colon) {
                    size_t sl = (size_t)(colon - sp);
                    if (sl >= sizeof(ssid)) sl = sizeof(ssid) - 1;
                    memcpy(ssid, sp, sl);
                    ssid[sl] = '\0';
                    strncpy(pass, colon + 1, sizeof(pass) - 1);
                } else {
                    strncpy(ssid, sp, sizeof(ssid) - 1);
                }
            }
            sb = serial_backend_picowifi_create(ssid[0] ? ssid : NULL,
                                                pass[0] ? pass : NULL);
        } else if (!sb) {
            log_error("Unknown serial backend: %s", c->arg);
            log_error("  loopback, tcp:host:port, pty, modem:host:port,");
            log_error("  modem:listen:port, com:baud,bits,P,stop,device,");
            log_error("  file:in[:out], digitelec:host:port, picowifi[:SSID[:PASS]]");
            return 1;
        }

        if (sb) {
            /* Bounded RX (--serial-tcp-backpressure): cap the kernel socket
             * buffer BEFORE open() so it takes effect on the live fd. Default
             * cap tracks the RX FIFO depth (or 512) when N is not given. */
            if (c->tcp_backpressure && sb->type == SERIAL_BACKEND_TCP) {
                int cap = c->tcp_rcvbuf;
                if (cap <= 0) cap = (c->buffer_size > 0) ? c->buffer_size : 512;
                serial_backend_tcp_set_rcvbuf(sb, cap);
            } else if (c->tcp_backpressure) {
                log_warning("--serial-tcp-backpressure has no effect on non-TCP backend '%s' "
                            "(only tcp: has a kernel socket buffer to bound)", c->arg);
            }
            if (sb->open(sb)) {
                acia_set_backend(&emu->acia, sb);
                emu->serial_backend = sb;
                emu->card_on[CARD_IDX_acia] = true;
                if (c->v23 || sb->type == SERIAL_BACKEND_DIGITELEC) {
                    acia_set_v23_mode(&emu->acia, true);
                }
                if (c->buffer_size > 0) {
                    acia_set_rx_fifo(&emu->acia, c->buffer_size);
                }
                if (c->baud > 0) {
                    acia_set_ext_clock_baud(&emu->acia, (uint32_t)c->baud);
                }
                if (c->irq_on_rdrf) {
                    acia_set_irq_on_rdrf(&emu->acia, true);
                }
                if (c->loci_irq_latency_us > 0) {
                    /* LOCI I2C IRQ transport cost. At 1 MHz, 1 µs = 1 cycle;
                     * compute from the master clock so it stays correct if the
                     * clock ever changes. LOCI-context only: warn on a bare 6551
                     * so nobody penalizes a plain ACIA card by accident. */
                    uint32_t cyc = (uint32_t)((uint64_t)c->loci_irq_latency_us *
                                              ORIC_CLOCK_HZ / 1000000u);
                    if (!core->loci_enabled) {
                        log_warning("--loci-irq-latency models a LOCI I2C artifact but "
                                    "--loci is not set; applying to the ACIA anyway "
                                    "(a real bare 6551 has no such transport cost)");
                    }
                    acia_set_irq_latency(&emu->acia, cyc);
                }
                if (c->tcp_backpressure && sb->type == SERIAL_BACKEND_TCP) {
                    acia_set_rx_backpressure(&emu->acia, true);
                }
                if (c->trace_file) {
                    acia_set_trace(&emu->acia, c->trace_file);
                }
                log_info("Serial interface enabled: %s", c->arg);
            } else {
                log_error("Failed to open serial backend: %s", c->arg);
                serial_backend_destroy(sb);
            }
        }
    }

    return 0;
}

static void teardown(emulator_t* emu) {
    if (emu->serial_backend) {
        serial_backend_destroy(emu->serial_backend);
        emu->serial_backend = NULL;
        emu->card_on[CARD_IDX_acia] = false;
    }
    /* Close ACIA trace and free RX FIFO */
    acia_set_trace(&emu->acia, NULL);
    acia_set_rx_fifo(&emu->acia, 0);
}

const card_module_t card_acia = {
    .descs = k_descs, .ndescs = 1, .desc_before = "dtl2000",
    .opts = k_opts, .nopts = 9, .opts_before = "dtl2000",
    .helps = k_helps, .nhelps = 2,
    .cfg_size = sizeof(acia_cfg_t),
    .init = init, .stage = CARD_STAGE_SERIAL, .setup = setup, .teardown = teardown,
    .bus = &k_bus, .bus_before = "mageco",
};
