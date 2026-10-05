/* SPDX-License-Identifier: EUPL-1.2 */
#define _POSIX_C_SOURCE 200809L
/*
 * loci_hw.c — Phosphoric's "REAL HARDWARE" LOCI backend (--loci-hw DEV).
 *
 * Same interface as loci_emu.h, but instead of running the firmware in the
 * RP2040 emulator (loci_emu.c) or doing nothing (loci_emu_stub.c), every
 * access of the emulated 6502 to the LOCI page ($03xx) or to the served ROM ($C000-$FFFF under
 * nROMDIS) becomes a REAL bus cycle served by a LOCI-USB: a LOCI without an
 * Oric interface (Feather RP2040, future LOCI-USB board) whose LOCI firmware
 * (LOCI_USB variant) replays these accesses, speaking proto/loci_usb_proto.h over USB
 * CDC. (The first approach, a Pico bridge on CN1 of an unmodified cartridge, was
 * abandoned on 2026-09-13.)
 *
 * Built into Phosphoric as soon as the loci-usb repository is present (LOCI_HW, functions
 * renamed by loci_be_rename.h) and selected at launch by --loci-hw or the « usb »
 * mode of the LOCI card (dispatch in loci_backend.c). Source of truth: this
 * tree (snapshots in ~/loci/loci-usb/phosphoric/).
 *
 * Model:
 *  - the real firmware runs continuously: "booted" as soon as the bridge answers (PING);
 *    at startup, POWERON (caps & LUP_CAP_POWERON) puts it back into its power-on state;
 *  - $03xx: one USB round trip per access (stop-and-wait, ~0.1-1 ms);
 *  - served ROM: 16 KB host CACHE filled by RDN (one bank in one request),
 *    with the nROMDIS/nMAP flags per address. Invalidated when the GENERATION of the ROM
 *    view (gen8, returned by every response: base/MAP/trap/loading changed on the
 *    firmware side) moves, on every nRESET edge and on every nROMDIS change.
 *    LOCI_HW_ROM_NOCACHE=1: no cache (one cycle per fetch, exact, slow);
 *  - nIRQ / nRESET: edges counted by the bridge, drained once per frame
 *    (loci_emu_irq_take / loci_emu_reset_take); the MENU button is PHYSICAL: the host
 *    sees the resulting nRESET and resets its 6502;
 *  - USB keyboard/mouse: those plugged into the cartridge (no injection possible);
 *  - Φ2 race (caps & LUP_CAP_TIMING): stop-and-wait freezes the 6502 during each
 *    access, which hides the Φ2 constraint of a real LOCI. RDT/WRT return the
 *    act_loop cycles measured with the core-1 SysTick: SERVE (up to the trigger of
 *    the read-serve DMA) and ACT (up to the end of the side effects). Timeline of a
 *    $03xx read in ns, origin at the falling edge of Φ2 that opens the cycle:
 *      - the LOCI's PIO runs at Φ2cfg × 30 (Φ2cfg = firmware setting, 4000 kHz by
 *        default → 1 tick = 8.33 ns; this is NOT the Oric's clock);
 *      - mia_action pushes the word into the FIFO at (22 + tior) ticks + 2 sys cycles of
 *        synchroniser (counts read from mia.pio, not measured);
 *      - act_loop picks it up (LOCI_HW_POLL_NS, not measured, 0) and triggers the DMA SERVE
 *        sys cycles later;
 *      - mia_io_read waits for Φ2 high then drives the bus (3 + tiod) ticks later:
 *        data = max(Φ2 rise + synchroniser, ready) + (3 + tiod) ticks.
 *    The Oric's Φ2: period 1/LOCI_HW_PHI2_KHZ (1000), high during the last third of the cycle
 *    (LOCI_HW_PHI2_HIGH_NS, period/3: the ULA gives 2/3 low, 1/3 high). A read is LATE
 *    if the data arrives after the end of the cycle minus the 6502 setup time
 *    (LOCI_HW_TDSR_NS, 100). A $03xx access arriving less than ACT 6502 cycles after the
 *    previous one reads a STALE iopage. Detection only by default (counters + log);
 *    LOCI_HW_FAITHFUL=1 returns open-bus on a late read.
 */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "loci_usb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "io/bus_timing.h"

static lup_client_t g_c;
static int  g_active;             /* bridge open and PING OK */
static int  g_romdis;             /* last known nROMDIS state (1 = active) */
static int  g_reset_pending;      /* nRESET edges seen since the last loci_emu_reset_take */
static int  g_link_err_logged;
static long g_settle_us;          /* LOCI_HW_SETTLE_US: pause after each $03xx access (emulated bench) */
static long g_idle_poll_cycles;   /* LOCI_HW_IDLE_POLL: cycles without LOCI access before a LINES (0 = never) */
static long g_idle_cycles;        /* 6502 cycles elapsed since the last LOCI access */
static unsigned long g_idle_polls, g_idle_polls_hit;

/* Φ2 race (caps & LUP_CAP_TIMING) */
static int           g_timing;              /* RDT/WRT in use */
static lup_timing_t  g_tm;                  /* current clocks and delays of the cartridge */
static long          g_phi2_khz = 1000;     /* LOCI_HW_PHI2_KHZ: Φ2 of the emulated Oric (1 MHz) */
static double        g_high_ns;             /* LOCI_HW_PHI2_HIGH_NS: Φ2 high (period/3) */
static double        g_tdsr_ns = 100;       /* LOCI_HW_TDSR_NS: 6502 data setup time */
static double        g_poll_ns;             /* LOCI_HW_POLL_NS: FIFO → act_loop, not measured */
static int           g_faithful;            /* LOCI_HW_FAITHFUL: late → open-bus */
static int           g_read_lost;           /* last read lost (faithful mode) */
static long          g_since_access = 1L << 30;   /* 6502 cycles since the last $03xx access */
static long          g_prev_act_cyc;        /* act duration of the last access, in 6502 cycles */
static unsigned long g_timed, g_late, g_stale;
static unsigned      g_serve_max, g_act_max;
static double        g_worst_margin = 1e9;  /* deadline - data, in ns (negative = late) */

/* Cache of the served ROM ($C000-$FFFF) */
static uint8_t g_rom[16384], g_rom_flags[16384];
static int     g_rom_valid, g_rom_nocache;
static unsigned long g_rom_refills;
static unsigned long g_bal_writes, g_bal_cmds;   /* captured BAL writes, commands launched */
#define BAL_GROUP_CONSOLE 2               /* BAL group executed on the 6502 side */
static long g_bal_timeout_ms = 10000;   /* LOCI_HW_BAL_TIMEOUT_MS: maximum wait for a BAL command */

const char *loci_emu_backend_name(void) { return "hw"; }

static void link_error_once(const char *ctx)
{
    if (g_link_err_logged) return;
    g_link_err_logged = 1;
    log_error("LOCI-hw: %s — %s (les accès suivants rendent $FF)", ctx, lup_client_error(&g_c));
}

static void rom_invalidate(const char *why)
{
    if (g_rom_valid) log_debug("LOCI-hw: cache ROM invalidé (%s)", why);
    g_rom_valid = 0;
}

static uint8_t g_gen;          /* generation of the cached ROM view */
/* LOCI API call (write of MIA_OP $03AF): on a real Oric, the 6502 is already in
 * the iopage wait loop ($03B0, the `JSR MIA_SPIN` following the STA) when the
 * firmware acts, even if it replaces the served ROM (mia_api_boot loads BASIC
 * into the menu bank and moves gen8). Here the bridge reports the new generation
 * in the reply to the write: invalidation is deferred until the next $03xx
 * access, otherwise the JSR bytes would be read again from the already replaced
 * ROM (LOCI menu → ESC frozen on « Booting », PC=$0244). nROMDIS and nRESET-edge
 * invalidations stay immediate. Without a cache (LOCI_HW_ROM_NOCACHE), the ROM is
 * read directly: the race remains. */
#define LOCI_HW_MIA_OP 0x03AF
static int g_op_defer;         /* MIA_OP written, no $03xx access yet */
static int g_gen_pending;      /* generation change seen during the deferral */
static void note_gen(void)
{
    if (g_c.gen != g_gen) {
        g_gen = g_c.gen;
        if (g_op_defer) g_gen_pending = 1;
        else rom_invalidate("génération");
    }
}
/* First $03xx access after the call: the 6502 is in the iopage, the ROM may change. */
static void op_defer_release(void)
{
    if (!g_op_defer) return;
    g_op_defer = 0;
    if (g_gen_pending) { g_gen_pending = 0; rom_invalidate("génération (après appel API)"); }
}
/* nRESET edge: the cache is invalidated anyway, the deferral no longer matters. */
static void op_defer_cancel(void) { g_op_defer = 0; g_gen_pending = 0; }
static void note_flags(uint8_t flags)
{
    int romdis = (flags & LUP_F_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    note_gen();
}

/* Access trace: LOCI_HW_TRACE=<file> (or "-" = stderr) — one line per $03xx
 * access (R/W, address, value, flags); identical repeated reads are counted. */
static FILE *g_trace; static int g_trace_init;
static void trace_access(char dir, uint16_t addr, uint8_t v, uint8_t f)
{
    static char last_dir; static uint16_t last_addr; static uint8_t last_v; static unsigned repeat;
    if (!g_trace_init) { g_trace_init = 1; const char *p = getenv("LOCI_HW_TRACE");
        if (p && *p) { g_trace = (p[0] == '-' && !p[1]) ? stderr : fopen(p, "w"); if (g_trace) setvbuf(g_trace, NULL, _IOLBF, 0); } }
    if (!g_trace) return;
    if (dir == last_dir && addr == last_addr && v == last_v) { repeat++; return; }
    if (repeat) fprintf(g_trace, "   (x%u)\n", repeat + 1);
    repeat = 0; last_dir = dir; last_addr = addr; last_v = v;
    fprintf(g_trace, "%c $%04X %02X f=%02X\n", dir, addr, v, f);
}

/* EMULATED bench (loci_usb_emul): the firmware there is much slower than on silicon
 * while Phosphoric's 6502 keeps running; an end-of-sector IRQ may then
 * arrive "late" compared with the real hardware. LOCI_HW_SETTLE_US=n gives the
 * firmware n µs of real time after each access (0 = none, default; useless on silicon). */
static void settle(void)
{
    if (g_settle_us > 0) { struct timespec ts = { 0, g_settle_us * 1000L }; nanosleep(&ts, NULL); }
}

/* ── Φ2 race ── */
static double period_ns(void) { return 1e6 / (double)g_phi2_khz; }
static double tick_ns(void)   { return 1e6 / ((double)g_tm.phi2_khz * 30.0); }   /* PIO = Φ2cfg × 30 */

static void timing_load(void)
{
    if (!g_timing) return;
    if (lup_timing(&g_c, &g_tm) != 0 || !g_tm.sys_khz || !g_tm.phi2_khz) {
        log_warning("LOCI-hw: TIMING a échoué (%s) — course Φ2 non mesurée", lup_client_error(&g_c));
        g_timing = 0;
        return;
    }
    log_info("LOCI-hw: course Φ2 mesurée — sys %lu kHz, tick PIO %.2f ns (Φ2cfg %lu kHz), tior %u, tiod %u ; "
             "Oric : période %.0f ns, Φ2 haut %.0f ns, tDSR %.0f ns ; %s",
             (unsigned long)g_tm.sys_khz, tick_ns(), (unsigned long)g_tm.phi2_khz, g_tm.tior, g_tm.tiod,
             period_ns(), g_high_ns, g_tdsr_ns, g_faithful ? "fidèle (retard → open-bus)" : "détection seule");
}
/* Core-1 cycles → 6502 cycles (rounded up). */
static long cyc_to_6502(unsigned cyc) { return (long)(((uint64_t)cyc * (uint64_t)g_phi2_khz + g_tm.sys_khz - 1) / g_tm.sys_khz); }

/* Instant (ns after the falling edge of Φ2) at which the data of a read served in
 * `serve` sys cycles is on the bus, and the 6502 deadline: timeline shared
 * with the emulated model (bus_timing.h), parameterised by the real clocks. */
static bus_loci_timing_t hw_timing(void)
{
    bus_loci_timing_t t = bus_loci_timing_default();
    t.sys_khz   = g_tm.sys_khz;
    t.pio_khz   = g_tm.phi2_khz * 30u;
    t.period_ps = (int64_t)(period_ns() * 1000.0);
    t.high_ps   = (int64_t)(g_high_ns * 1000.0);
    t.tdsr_ps   = (int64_t)(g_tdsr_ns * 1000.0);
    t.poll_ps   = (int64_t)(g_poll_ns * 1000.0);
    return t;
}
static double data_valid_ns(unsigned serve)
{
    bus_loci_timing_t t = hw_timing();
    return bus_loci_read_valid_ps(&t, g_tm.tior, g_tm.tiod, serve) / 1000.0;
}
static double deadline_ns(void) { bus_loci_timing_t t = hw_timing(); return bus_loci_deadline_ps(&t) / 1000.0; }

/* $03xx access arriving before the end of the previous one's side effects: stale iopage. */
static void check_stale(char dir, uint16_t addr)
{
    if (g_prev_act_cyc > 0 && g_since_access < g_prev_act_cyc) {
        if (++g_stale <= 10)
            log_warning("LOCI-hw: Φ2 %c $%04X %ld cycles après l'accès précédent, dont l'action en dure %ld — iopage périmé",
                        dir, addr, g_since_access, g_prev_act_cyc);
    }
}
static void note_act(unsigned act)
{
    if (act > g_act_max) g_act_max = act;
    g_prev_act_cyc = act ? cyc_to_6502(act) : 0;
    g_since_access = 0;
}

/* Generic bus cycle ($03xx page). */
static uint8_t bus_rd(uint16_t addr)
{
    uint8_t d = 0xFF, f;
    if (!g_active) return 0xFF;
    op_defer_release();
    if (g_timing) {
        uint16_t serve = 0, act = 0;
        if (lup_rdt(&g_c, addr, &d, &f, &serve, &act) != 0) { link_error_once("lecture"); return 0xFF; }
        check_stale('R', addr);
        if (serve) {                         /* 0: access outside act_loop, nothing to judge */
            g_timed++;
            if (serve > g_serve_max) g_serve_max = serve;
            double valid = data_valid_ns(serve), margin = deadline_ns() - valid;
            if (margin < g_worst_margin) g_worst_margin = margin;
            if (margin < 0) {
                if (++g_late <= 10)
                    log_warning("LOCI-hw: Φ2 lecture $%04X EN RETARD — serve %u cycles, donnée à %.0f ns > échéance %.0f ns%s",
                                addr, serve, valid, deadline_ns(), g_faithful ? " → open-bus" : "");
                if (g_faithful) g_read_lost = 1;
            }
        }
        note_act(act);
    } else {
        if (lup_rd(&g_c, addr, &d, &f) != 0) { link_error_once("lecture"); return 0xFF; }
    }
    note_flags(f);
    trace_access('R', addr, d, f);
    settle();
    g_idle_cycles = 0;
    return d;
}

static void bus_wr(uint16_t addr, uint8_t v)
{
    uint8_t f;
    if (!g_active) return;
    op_defer_release();
    if (addr == LOCI_HW_MIA_OP) g_op_defer = 1;   /* its reply may already carry the new gen8 */
    if (g_timing) {
        uint16_t act = 0;
        if (lup_wrt(&g_c, addr, v, &f, &act) != 0) { link_error_once("écriture"); return; }
        check_stale('W', addr);
        note_act(act);
    } else {
        if (lup_wr(&g_c, addr, v, &f) != 0) { link_error_once("écriture"); return; }
    }
    note_flags(f);
    trace_access('W', addr, v, f);
    settle();
    g_idle_cycles = 0;
}

bool loci_emu_read_lost(void) { int l = g_read_lost; g_read_lost = 0; return l != 0; }

/* ── lifecycle ── */
int loci_emu_start(const char *dev)
{
    g_rom_nocache = getenv("LOCI_HW_ROM_NOCACHE") != NULL;
    g_settle_us = getenv("LOCI_HW_SETTLE_US") ? atol(getenv("LOCI_HW_SETTLE_US")) : 0;
    g_idle_poll_cycles = getenv("LOCI_HW_IDLE_POLL") ? atol(getenv("LOCI_HW_IDLE_POLL")) : 1000;
    if (lup_open(&g_c, dev) != 0) {
        log_error("LOCI-hw: impossible d'ouvrir le pont « %s » : %s", dev, lup_client_error(&g_c));
        return -1;
    }
    /* The cartridge stays powered by USB between two sessions: without a reset to
     * the « power-on » state, the next session inherits the previous state
     * (2 nRESET edges, unstable gen8, 6502 crashed in the stack). POWERON = the equivalent of
     * switching on the Oric, which also resets the LOCI. LOCI_HW_NO_POWERON=1: keep the state. */
    if ((g_c.caps & LUP_CAP_POWERON) && !getenv("LOCI_HW_NO_POWERON")) {
        if (lup_poweron(&g_c) != 0 || lup_reconnect(&g_c, dev, 10000) != 0) {
            log_error("LOCI-hw: POWERON a échoué : %s", lup_client_error(&g_c));
            lup_close(&g_c);
            return -1;
        }
        log_info("LOCI-hw: cartouche remise à l'état mise sous tension (POWERON)");
    }
    uint8_t lines = 0, irqs, rsts;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) {
        log_error("LOCI-hw: LINES a échoué : %s", lup_client_error(&g_c));
        lup_close(&g_c);
        return -1;
    }
    g_romdis = (lines & LUP_L_NROMDIS) != 0;
    g_gen = g_c.gen;
    g_active = 1;
    g_phi2_khz  = getenv("LOCI_HW_PHI2_KHZ") ? atol(getenv("LOCI_HW_PHI2_KHZ")) : 1000;
    if (g_phi2_khz <= 0) g_phi2_khz = 1000;
    g_high_ns   = getenv("LOCI_HW_PHI2_HIGH_NS") ? atof(getenv("LOCI_HW_PHI2_HIGH_NS")) : period_ns() / 3;
    g_tdsr_ns   = getenv("LOCI_HW_TDSR_NS") ? atof(getenv("LOCI_HW_TDSR_NS")) : 100;
    g_poll_ns   = getenv("LOCI_HW_POLL_NS") ? atof(getenv("LOCI_HW_POLL_NS")) : 0;
    g_faithful  = getenv("LOCI_HW_FAITHFUL") != NULL;
    g_timing    = (g_c.caps & LUP_CAP_TIMING) != 0;
    g_timed = g_late = g_stale = 0; g_serve_max = g_act_max = 0; g_worst_margin = 1e9;
    g_read_lost = 0; g_prev_act_cyc = 0; g_since_access = 1L << 30;
    g_bal_writes = g_bal_cmds = 0; g_rom_valid = 0; g_rom_refills = 0; op_defer_cancel();
    g_bal_timeout_ms = getenv("LOCI_HW_BAL_TIMEOUT_MS") ? atol(getenv("LOCI_HW_BAL_TIMEOUT_MS")) : 10000;
    log_info("LOCI-hw: pont « %s » prêt (proto %u, firmware pont %u, caps %02X%s) — nROMDIS=%d nRESET=%d ; "
             "cache ROM %s", dev, g_c.proto, g_c.fw, g_c.caps,
             (g_c.caps & LUP_CAP_VIRTUAL) ? ", VIRTUEL" : "", g_romdis, (lines & LUP_L_NRESET) != 0,
             g_rom_nocache ? "désactivé" : "actif");
    if (g_settle_us > 0) log_info("LOCI-hw: stabilisation %ld µs après chaque accès (LOCI_HW_SETTLE_US)", g_settle_us);
    log_info("LOCI-hw: poll en attente %s (LOCI_HW_IDLE_POLL=%ld cycles)", g_idle_poll_cycles > 0 ? "actif" : "désactivé", g_idle_poll_cycles);
    if (g_timing) timing_load();
    else log_info("LOCI-hw: course Φ2 non mesurée (firmware sans TIMING)");
    return 0;
}

void loci_emu_set_usb_image(const char *path)  { if (path) log_warning("LOCI-hw: --loci-emu-usb-image ignoré (clé USB réelle sur la cartouche)"); }
void loci_emu_set_flash_image(const char *path) { if (path) log_warning("LOCI-hw: --loci-emu-flash ignoré (flash réelle de la cartouche)"); }
void loci_emu_set_cdc_device(const char *path)  { if (path) log_warning("LOCI-hw: --loci-cdc ignoré (modem USB réel sur la cartouche)"); }

void loci_emu_stop(void)
{
    if (!g_active) return;
    log_info("LOCI-hw: fin de session — %lu requêtes, %lu rechargements du cache ROM, %lu polls en attente (%lu avec événement)",
             g_c.n_req, g_rom_refills, g_idle_polls, g_idle_polls_hit);
    if (g_bal_writes)
        log_info("LOCI-hw: BAL — %lu écritures captées, %lu commandes", g_bal_writes, g_bal_cmds);
    if (g_timing)
        log_info("LOCI-hw: course Φ2 — %lu lectures mesurées, %lu EN RETARD, %lu sur iopage périmé ; "
                 "serve max %u cycles, act max %u cycles, marge minimale %.0f ns",
                 g_timed, g_late, g_stale, g_serve_max, g_act_max, g_timed ? g_worst_margin : 0.0);
    lup_close(&g_c);
    g_active = 0;
}

bool loci_emu_active(void)     { return g_active != 0; }
bool loci_emu_wait_boot(void)  { return g_active != 0; }

/* ── MENU button: software (protocol v2, LOCI_USB firmware) or physical ──
 * In both cases the firmware drives nRESET: the host sees it at the next
 * loci_emu_reset_take() and then resets its 6502 — so false is returned here. */
static bool press_button(uint8_t action)
{
    if (!g_active) return false;
    if (g_c.caps & LUP_CAP_FIRMWARE) {
        if (lup_btn(&g_c, action) != 0) { link_error_once("BTN"); return false; }
        /* The firmware handles the button and loads its ROM in REAL TIME (seconds in
         * emulation, tens of ms on silicon) while the emulated Oric keeps running:
         * we wait here for nRESET to be released (at most 10 s) so that the 6502 reset
         * lands on a complete ROM — like a real Oric held in reset by LOCI. */
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        for (int i = 0; i < 500; i++) {
            uint8_t lines = 0, irqs = 0, rsts = 0;
            if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return false; }
            int romdis = (lines & LUP_L_NROMDIS) != 0;
            if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate("nROMDIS (bouton)"); }
            note_gen();
            if (rsts) { g_reset_pending += rsts; op_defer_cancel(); rom_invalidate("front nRESET (bouton)");
                        log_info("LOCI-hw: bouton MENU %s → nRESET relâché après %d ms", action == 2 ? "long" : "court", i * 20); return false; }
            nanosleep(&ts, NULL);
        }
        log_warning("LOCI-hw: bouton MENU %s envoyé, mais pas de nRESET en 10 s", action == 2 ? "long" : "court");
    } else {
        log_info("LOCI-hw: le bouton MENU est PHYSIQUE (sur la cartouche) — appuyez dessus ; "
                 "le 6502 sera réinitialisé quand LOCI pilotera nRESET");
    }
    return false;
}
bool loci_emu_menu_button(void)     { return press_button(1); }
bool loci_emu_button_was_warm(void) { return false; }
bool loci_emu_diag_button(void)     { return press_button(2); }

/* ── ROM overlay ── */
static int rom_refill(void)
{
    if (lup_rdn(&g_c, 0xC000, 16384, g_rom, g_rom_flags) != 0) { link_error_once("RDN ROM"); return 0; }
    g_rom_valid = 1; g_rom_refills++;
    g_gen = g_c.gen;                     /* the image is consistent with this generation */
    int romdis = (g_rom_flags[0] & LUP_F_NROMDIS) != 0;
    if (romdis != g_romdis) g_romdis = romdis;
    return 1;
}

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    if (!g_active || address < 0xC000 || !g_romdis) return false;
    uint8_t d, f;
    if (g_rom_nocache) {
        if (lup_rd(&g_c, address, &d, &f) != 0) { link_error_once("lecture ROM"); return false; }
        note_flags(f);
    } else {
        if (!g_rom_valid && !rom_refill()) return false;
        d = g_rom[address - 0xC000]; f = g_rom_flags[address - 0xC000];
    }
    if (!(f & LUP_F_NROMDIS) || (f & LUP_F_NMAP)) return false;   /* under MAP: Oric RAM overlay */
    *out = d;
    return true;
}

bool loci_emu_romdis(void) { return g_active && g_romdis; }

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    uint8_t lines = 0;
    if (!g_active || lup_lines(&g_c, &lines, NULL, NULL) != 0) { lines = 0; if (g_active) link_error_once("LINES"); }
    if (nirq)    *nirq    = (lines & LUP_L_NIRQ) != 0;
    if (nreset)  *nreset  = (lines & LUP_L_NRESET) != 0;
    if (nromdis) *nromdis = (lines & LUP_L_NROMDIS) != 0;
}

/* ── $03xx page: API, Microdisc, tape, ACIA — all real cycles ── */
void    loci_emu_api_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_api_read(uint16_t address)                 { return bus_rd(address); }
void    loci_emu_dsk_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_dsk_read(uint16_t address)                 { return bus_rd(address); }
void    loci_emu_tap_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_tap_read(uint16_t address)                 { return bus_rd(address); }
/* The tape motor (VIA ORB $0300): on a real bus, LOCI snoops the write
 * to $0300 — we replay it (nIO low: $03xx page), but ONLY on a change
 * of PB6: the ROM rewrites ORB at every column of the keyboard scan, one USB
 * round trip per write would collapse the emulation. */
void loci_emu_tap_motor(uint8_t via_orb)
{
    static int last = -1;
    int motor = (via_orb >> 6) & 1;
    if (motor == last) return;
    last = motor;
    bus_wr(0x0300, via_orb);
}
void    loci_emu_dsk_tick(void)                             { }

/* nIRQ edges counted by the bridge since the last drain (once per frame).
 * Uses the same round trip to pick up nROMDIS and the nRESET edges. */
static int lines_drain(void)
{
    uint8_t lines = 0, irqs = 0, rsts = 0;
    if (!g_active) return 0;
    if (lup_lines(&g_c, &lines, &irqs, &rsts) != 0) { link_error_once("LINES"); return 0; }
    int romdis = (lines & LUP_L_NROMDIS) != 0;
    if (romdis != g_romdis) { g_romdis = romdis; rom_invalidate(romdis ? "nROMDIS actif" : "nROMDIS relâché"); }
    note_gen();
    if (rsts) { g_reset_pending += rsts; op_defer_cancel(); rom_invalidate("front nRESET"); }
    return irqs;
}

int loci_emu_irq_take(void) { return lines_drain(); }

/* "Idle" poll: with no LOCI access for LOCI_HW_IDLE_POLL cycles, one LINES.
 * Free when the program talks to LOCI (the counter is reset on every
 * access), bounds the latency of asynchronous IRQs/resets to N cycles when it waits. */
int loci_emu_idle_poll(int cycles)
{
    if (g_since_access < (1L << 30)) g_since_access += cycles;
    if (!g_active || g_idle_poll_cycles <= 0) return 0;
    g_idle_cycles += cycles;
    if (g_idle_cycles < g_idle_poll_cycles) return 0;
    g_idle_cycles = 0;
    g_idle_polls++;
    int before = g_reset_pending;
    int irqs = lines_drain();
    if (irqs || g_reset_pending != before) g_idle_polls_hit++;
    return irqs ? irqs : (g_reset_pending != before ? -1 : 0);  /* -1 = reset only: the caller picks up loci_emu_reset_take */
}

/* nRESET edges driven by LOCI (physical MENU button, freeze) since the last
 * call: the host must then reset its 6502. */
int loci_emu_reset_take(void)
{
    int n = g_reset_pending;
    g_reset_pending = 0;
    if (n) {
        log_info("LOCI-hw: nRESET piloté par LOCI (%d front%s) → reset du 6502", n, n > 1 ? "s" : "");
        timing_load();                    /* Φ2 and delays may have changed in the menu */
    }
    return n;
}

/* ── ACIA: served by the cartridge itself ($0380-$0383, mode 1) ── */
bool    loci_emu_acia_active(void)              { return false; }   /* no CDC dongle on the host side */
bool    loci_emu_acia_served(uint16_t address)  { return g_active && address >= 0x0380 && address <= 0x0383; }
void    loci_emu_acia_write(uint16_t address, uint8_t value) { bus_wr(address, value); }
uint8_t loci_emu_acia_read(uint16_t address)    { return bus_rd(address); }
/* "Non-destructive" observation: on real hardware, reading $0380 consumes
 * the received byte → it is not read; the status/command/control registers are. */
uint8_t loci_emu_acia_peek(uint16_t address)    { return address == 0x0380 ? 0xFF : bus_rd(address); }
void    loci_emu_acia_tick(void)                { }

void loci_emu_tick(long steps) { (void)steps; }   /* the real firmware advances on its own */

/* HID: with the LOCI_USB firmware (caps FIRMWARE), the host keyboard/mouse
 * are injected through the protocol (the firmware's real kbd_report()/mou_report());
 * otherwise it is the devices plugged into the cartridge. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    if (!g_active || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    if (lup_mou(&g_c, buttons, dx, dy, wheel, pan) != 0) { link_error_once("MOU"); return false; }
    return true;
}
bool loci_emu_mou_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!g_active || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    if (lup_kbd(&g_c, modifier, keycodes) != 0) { link_error_once("KBD"); return false; }
    return true;
}
bool loci_emu_kbd_armed(void) { return g_active && (g_c.caps & LUP_CAP_FIRMWARE); }

/* 6502 write to page $FF under nROMDIS: loci-fw mailbox (BAL). The
 * firmware copies the byte into the served image and answers SERVED (« captured ») without
 * changing gen8: the cache is updated here. A non-zero byte at $FF00 launches a
 * command (except group 2, Console, executed by the 6502 itself); we wait for
 * its completion ($FF00 read back as 0 by uncached RD, 10 s at most,
 * LOCI_HW_BAL_TIMEOUT_MS) so that the kernel's wait
 * loop sees it immediately, instead of at the next LINES. The dispatcher
 * changes gen8 when returning its results: the cache is then invalidated. Older
 * firmware never answers SERVED here (write ignored) → false, as before. */
static long elapsed_ms(const struct timespec *t0)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}
bool loci_emu_rom_write(uint16_t address, uint8_t value)
{
    if (!g_active || (address & 0xFF00u) != 0xFF00u || !(g_c.caps & LUP_CAP_FIRMWARE)) return false;
    uint8_t f;
    if (lup_wr(&g_c, address, value, &f) != 0) { link_error_once("écriture BAL"); return false; }
    note_flags(f);
    if (!(f & LUP_F_SERVED)) return false;          /* not captured: overlay RAM or ROM */
    if (g_rom_valid) g_rom[address - 0xC000] = value;
    g_bal_writes++;
    /* Group 2 (Console) is never handled by the firmware: the 6502 kernel
     * executes it (loci-fw ADR-004, dispatch.c); waiting for it would freeze the 6502
     * until the timeout. */
    if (address == 0xFF00u && value && value != BAL_GROUP_CONSOLE) {
        /* Real-time timeout: an MSC command (USB stick read) or a console command can
         * last hundreds of reads on silicon. */
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            uint8_t d = 0xFF, rf;
            if (lup_rd(&g_c, 0xFF00, &d, &rf) != 0) { link_error_once("attente BAL"); break; }
            note_flags(rf);
            if (d == 0) break;
            if (elapsed_ms(&t0) >= g_bal_timeout_ms) {
                log_warning("LOCI-hw: commande BAL $%02X toujours en cours après %ld ms", value, g_bal_timeout_ms);
                break;
            }
        }
        g_bal_cmds++;
    }
    return true;
}

/* Whole I/O page through bus cycles: specific to the neo backend (loci-fw). */
bool    loci_emu_io_page(void) { return false; }
bool    loci_emu_io_read(uint16_t address, uint8_t *out) { (void)address; (void)out; return false; }
void    loci_emu_io_write(uint16_t address, uint8_t value) { (void)address; (void)value; }

/* Φ2 race counters (tests; outside the loci_emu.h interface). */
void loci_hw_timing_stats(unsigned long *timed, unsigned long *late, unsigned long *stale)
{
    if (timed) *timed = g_timed;
    if (late)  *late  = g_late;
    if (stale) *stale = g_stale;
}
