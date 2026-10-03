/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file card_acia.c
 * @brief ACIA 6551 serial card as a module: menu, options, setup
 *        (transports, Hayes modem, picowifi…), bus and teardown (card_module.h).
 * @author bmarty <bmarty@mailo.com>
 *
 * The ACIA state (emu->acia) stays in the machine: the « SER » section of
 * save states (savestate.c), the debugger, the control commands and
 * LOCI (ACIA at $0380) read it directly.
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

/* ── Configuration and options ─────────────────────────────────────────── */

typedef struct {
    const char* arg;               /* --serial TYPE */
    const char* addr;              /* --acia-addr ADDR (hex) */
    bool        v23;               /* --serial-v23 */
    int         buffer_size;       /* --serial-buffer N */
    int         baud;              /* --serial-baud N */
    bool        irq_on_rdrf;       /* --serial-irq-on-rdrf */
    const char* trace_file;        /* --serial-trace FILE (also DTL 2000, Mageco) */
    bool        tcp_backpressure;  /* --serial-tcp-backpressure[=N] */
    int         tcp_rcvbuf;        /* explicit N (0: auto) */
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

/* Same order as the old getopt table (identical ambiguous prefixes). */
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

/* Two blocks: the --serial* options (before the LOCI co-simulation ones),
 * then --acia-addr (after the LOCI options). */
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

/* Transports offered in the menu (cf. cards.c, card_dtl2000.c). */
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

/* ── Interrupts ────────────────────────────────────────────────────────── */

static void irq_set(emulator_t* emu) { cpu_irq_set(&emu->cpu, IRQF_SERIAL); }
static void irq_clr(emulator_t* emu) { cpu_irq_clear(&emu->cpu, IRQF_SERIAL); }

/* ── Bus ───────────────────────────────────────────────────────────────── */

/* ACIA 6551 ($031C-$031F by default, configurable base). */
static bool acia_dev_claims(emulator_t* emu, uint16_t addr) {
    /* Co-sim: the firmware serves its ACIA window from boot, dongle or not
     * (without modem: $0381 = $70). Without this claim, --loci-emu without --loci-cdc
     * let the VIA mirror answer at $0380 — unfaithful to the hardware. */
    if (loci_emu_active() && loci_emu_acia_served(addr)) return true;
    return emu->card_on[CARD_IDX_acia] && addr >= emu->acia_base_addr && addr <= (emu->acia_base_addr + 3);
}
/* picowifi-over-LOCI: the emulated ACIA 6551 lives at $0380, served through a PHI2
 * RACE. The MIA (RP2040: PIO core1 + slow software serve, ~I²C/DMA) must put
 * the byte on the data bus BEFORE the 6502's rising PHI2 edge. If the `tior`
 * timing margin is badly tuned (outside the window auto-tuned by ADJ_SCAN), the serve
 * loses the race. Since the VIA is decode-inhibited symmetrically over all of $03x0-$03xF
 * (IO_CONTROL = IO·(A4+A5+A6+A7), proven on hardware), NOTHING then drives
 * the bus → the 6502 latches the OPEN BUS (last driven byte), NOT the VIA.
 *
 * Asymmetry faithful to the HW (bug report + spec-acia-fiable):
 *  - WRITE always reliable: `write_enable_map = 0xFFFFFFFF` → a write to
 *    $0380-$0383 ALWAYS reaches the ACIA, race lost or not.
 *  - READ fragile AND, on the DATA register, DESTRUCTIVE on the LOCI side: the serve
 *    executes `acia_read()` "blindly" (it consumes the RX byte) while
 *    the 6502 only latches the floating bus → BYTE LOST, not re-readable. This is
 *    precisely the "unreachable modem".
 *  - STAT/CMD/CTRL are idempotent (re-readable) → a lost race returns the
 *    floating bus THIS time but the register stays readable on the next one (miss
 *    forgiven, like disk/MIA polling). Hence: disk OK / modem KO under the
 *    SAME margin, with no need for a probabilistic model. */
static inline bool acia_serve_lost(const emulator_t* emu) {
    return emu->has_loci && emu->acia_base_addr == 0x0380 &&
           !loci_mia_io_reliable(&emu->loci);
}
static uint8_t acia_dev_read(emulator_t* emu, uint16_t addr) {
    /* Co-sim backend (--loci-cdc): the $0380 ACIA is served by the REAL firmware
     * (oric/acia.c ↔ USB CDC modem) instead of the behavioural 6551. */
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr))) {
        uint8_t v = loci_emu_acia_read(addr);
        loci_emu_reflect_nirq(emu);
        return v;
    }
    /* CPU path: samples the race WITH jitter (advances the PRNG). The jitter
     * only has an effect in the PHASE model near the latch; otherwise it is the
     * deterministic nominal decision. */
    bool lost = emu->has_loci && emu->acia_base_addr == 0x0380 &&
                loci_mia_serve_lost_sampled(&emu->loci);
    if (lost) {
        /* Race lost: on DATA, LOCI consumed the byte blindly (lost);
         * the 6502 latches the open bus. On STAT/CMD/CTRL, nothing is consumed. */
        if ((addr & ACIA_ADDR_MASK) == ACIA_REG_DATA)
            (void)acia_read(&emu->acia, addr);   /* consume and discard: byte lost */
        return memory_open_bus(&emu->memory);
    }
    return acia_read(&emu->acia, addr);
}
static bool acia_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    /* Co-sim backend (--loci-cdc): $0380-$0383 write handled by the real firmware. */
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr))) {
        loci_emu_acia_write(addr, value);
        loci_emu_reflect_nirq(emu);
        return true;
    }
    /* Write always reliable (write_enable_map = 0xFFFFFFFF on the real LOCI):
     * it goes through even when the race is lost. */
    acia_write(&emu->acia, addr, value);
    return true;
}
/* Non-destructive observation read (debugger/monitor/dump/remote):
 * does NOT clear RDRF, does NOT pop the FIFO, does NOT clear the IRQ. Models the open bus
 * WITHOUT consuming (an observer does not take part in the 6502's PHI2 race). */
static uint8_t acia_dev_peek(emulator_t* emu, uint16_t addr) {
    if (loci_emu_acia_active() || (loci_emu_active() && loci_emu_acia_served(addr)))   /* co-sim: io-page peek */
        return loci_emu_acia_peek(addr);
    if (acia_serve_lost(emu))
        return memory_open_bus(&emu->memory);
    return acia_peek(&emu->acia, addr);
}

void card_acia_tick(emulator_t* emu, int cycles) {
    acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
    acia_tick(&emu->acia, cycles);
}

/* « SER » section: written by savestate.c (historical), not by the module. */
static const io_device_t k_bus = {
    .name = "acia", .claims = acia_dev_claims, .read = acia_dev_read,
    .write = acia_dev_write, .peek = acia_dev_peek,
    .present_off = offsetof(emulator_t, card_on[CARD_IDX_acia]), .tick = card_acia_tick,
};

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* At startup, present or not: 6551 idle, IRQs wired. */
static void init(emulator_t* emu) {
    acia_init(&emu->acia);
    emu->acia.irq_set = irq_set;
    emu->acia.irq_clr = irq_clr;
    emu->acia.irq_userdata = emu;
}

static int setup(emulator_t* emu, const void* p, const struct cli_opts_s* core) {
    const acia_cfg_t* c = p;
    /* Also read by the LOCI setup (picowifi) and by the DTL 2000
     * and Mageco cards (--serial-trace). */
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
        /* Co-sim (--loci-cdc): the ACIA at $0380 is served by the REAL firmware
         * (oric/acia.c ↔ CDC dongle) — no behavioural serial backend. */
        emu->acia_base_addr = 0x0380;
        log_info("ACIA base address: $0380 (co-sim firmware via --loci-cdc %s)", core->loci_emu_cdc_dev);
    } else {
        emu->acia_base_addr = ACIA_DEFAULT_BASE;
    }
    /* Co-sim (--loci-cdc): enables the ACIA without a backend (the real firmware serves it via
     * loci_emu_acia_*). The card must be present (card_on) for the ACIA device to claim $0380. */
    if (core->loci_emu_cdc_dev && core->loci_emu_path) emu->card_on[CARD_IDX_acia] = true;
    /* Safeguard: under --loci, the MIA occupies $03A0-$03BF and is routed BEFORE
     * the ACIA in the I/O callbacks. If the ACIA is forced there (--acia-addr in
     * this range), the MIA masks it AND drives the PSG/keyboard → the keyboard scan
     * reads nothing and get_key loops → « frozen » terminal (BBS directory frozen).
     * The real LOCI exposes its USB-CDC modem at $0380, not in the MIA. */
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
