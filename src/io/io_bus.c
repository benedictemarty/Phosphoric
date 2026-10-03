/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file io_bus.c
 * @brief I/O bus adapter — `io_device_t` wrappers, table and dispatch.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extracted from main.c (Epic 7 / US2, Sprint 126), with identical behaviour. Confines
 * the coupling to `emulator_t` to a single adaptation layer; the peripheral
 * modules stay decoupled from `emulator.h`. See docs/architecture/io-bus.md.
 */
#include "io/io_bus.h"
#include "io/loci_emu.h"   /* co-sim backend: MIA $03xx API served by the real firmware (--loci-emu) */
#include "emulator.h"
#include "card_module.h"   /* cards as modules: their bus devices */
#include "card_ticks.h"    /* their per-cycle ticks, as direct calls */

#include <stdio.h>
#include <stddef.h>   /* offsetof */
#include <string.h>

/* ── I/O bus: registered devices (docs/architecture/io-bus.md) ─────────────
 * Each device provides claims/read/write; the dispatch walks the table.
 * Table order = priority: LOCI first (overlaps the Microdisc via
 * TAP $0315-$0317), then ACIA (owns $031C-$031F when present) before Microdisc.
 * The ULA-NG comes last: its write must always receive the byte in its window
 * (even when locked, to watch for the 'N','G' sequence) and `ula_ng_write`
 * *returns* whether it consumed it — otherwise VIA fallback; hence the separate
 * `claims_write` and the boolean return of `write`. "Strangler" pattern. */

/* LOCI (sodiumlb): three disjoint sub-windows, dispatched internally.
 *  - MIA $03A0-$03BF (independent of the other devices);
 *  - TAP $0315-$0317: replaces the cassette interface, overlaps the Microdisc
 *    $0310-$031F → priority (LOCI is first in the table);
 *  - DSK $0310-$0314 + $0318-$0319: only when no real Microdisc is present
 *    (otherwise the Microdisc owns the range). */
/* Co-sim: window + registers of the $AF RAM expansion ($03C0-$03E4), served by the
 * firmware (io-page) — unknown to the internal model. */
static bool loci_emu_ramx_claims(uint16_t addr) {
    return loci_emu_active() && addr >= 0x03C0 && addr <= 0x03E4;
}
static bool loci_dev_claims(emulator_t* emu, uint16_t addr) {
    if (!emu->has_loci) return false;
    if (loci_emu_io_page()) return addr >= 0x0310 && addr <= 0x03FF;   /* neo backend: /IO CONTROL */
    if (loci_addr_in_mia(addr) || loci_emu_ramx_claims(addr)) return true;
    if (loci_addr_in_tap(addr)) return true;
    if (!emu->has_microdisc && loci_addr_in_dsk(addr)) return true;
    return false;
}
/* Synchronous nIRQ reflection (co-sim backend --loci-emu). The RP2040 firmware
 * PULSES the nIRQ line (ext_put(EXT_IRQ,true) then false) in response to a
 * MIA transaction (the write runs core0+core1 for the duration of the bus dialogue):
 * a LEVEL poll would miss it (already dropped). The emulator latches every rising
 * edge; these pulses are drained here, right after the transaction, and delivered
 * to the 6502 as an EDGE / ONE-SHOT (cpu_irq_pulse) — one IRQ per pulse, with no
 * level held, hence no storm. main.c also drains once per frame (safety net for
 * pulses outside MIA writes, e.g. IRQ trap on the button). */
static void loci_emu_reflect_nirq(emulator_t* emu) {
    int pulses = loci_emu_irq_take();
    for (int i = 0; i < pulses; i++) cpu_irq_pulse(&emu->cpu);
}
static uint8_t loci_dev_read(emulator_t* emu, uint16_t addr) {
    /* Co-sim backend (--loci-emu): the MIA $03xx window is served by the REAL
     * RP2040 firmware (emulator) instead of the behavioural backend (loci_core).
     * During the background boot (first launch of an ELF), we WAIT: otherwise the
     * internal model answered in place of the firmware (open("N:…") → FR_NO_FILE). */
    loci_emu_wait_boot();
    if (loci_emu_io_page()) {
        uint8_t v;
        bool drv = loci_emu_io_read(addr, &v);
        loci_emu_reflect_nirq(emu);
        return drv ? v : memory_open_bus(&emu->memory);
    }
    if (loci_emu_ramx_claims(addr)) return loci_emu_api_read(addr);
    if (loci_addr_in_mia(addr)) return loci_emu_active() ? loci_emu_api_read(addr)
                                                         : loci_read(&emu->loci, addr);
    if (loci_addr_in_tap(addr)) return loci_emu_active() ? loci_emu_tap_read(addr)
                                                         : loci_tap_read(&emu->loci, addr);
    /* DSK (guaranteed by claims). In co-sim, the WD1793 is the firmware's (oric/dsk.c):
     * a .dsk mounted on A: in the REAL LOCI menu is finally read by the 6502. */
    if (loci_emu_active()) {
        uint8_t v = loci_emu_dsk_read(addr);
        loci_emu_reflect_nirq(emu);   /* end of sector: the IRQ is raised on the LAST DATA read */
        return v;
    }
    return loci_dsk_read(&emu->loci, addr);
}
static bool loci_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    loci_emu_wait_boot();
    if (loci_emu_io_page()) { loci_emu_io_write(addr, value); loci_emu_reflect_nirq(emu); return true; }
    if (loci_emu_ramx_claims(addr))  loci_emu_api_write(addr, value);
    else if (loci_addr_in_mia(addr)) { if (loci_emu_active()) { loci_emu_api_write(addr, value);
                                                           loci_emu_reflect_nirq(emu); }
                                  else                   loci_write(&emu->loci, addr, value); }
    else if (loci_addr_in_tap(addr)) { if (loci_emu_active()) loci_emu_tap_write(addr, value);
                                       else                   loci_tap_write(&emu->loci, addr, value); }
    else if (loci_emu_active())    { loci_emu_dsk_write(addr, value);           /* DSK co-sim */
                                     loci_emu_reflect_nirq(emu); }
    else                             loci_dsk_write(&emu->loci, addr, value);  /* DSK */
    return true;
}

/* ACIA 6551 ($031C-$031F by default, configurable base). */
static bool acia_dev_claims(emulator_t* emu, uint16_t addr) {
    /* Co-sim: the firmware serves its ACIA window from boot, dongle or not
     * (without modem: $0381 = $70). Without this claim, --loci-emu without --loci-cdc
     * let the VIA mirror answer at $0380 — unfaithful to the hardware. */
    if (loci_emu_active() && loci_emu_acia_served(addr)) return true;
    return emu->has_serial && addr >= emu->acia_base_addr && addr <= (emu->acia_base_addr + 3);
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

/* Jasmin WD177x: $03F4-$03FF (mutually exclusive with DTL2000/Mageco, which
 * overlap $03F8-$03FF — guard at activation in main.c). */
static bool jasmin_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_jasmin && addr >= JASMIN_BASE && addr <= JASMIN_END;
}
static uint8_t jasmin_dev_read(emulator_t* emu, uint16_t addr) {
    return jasmin_read(&emu->jasmin, addr);
}
static bool jasmin_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (fdc_trace_enabled()) {
        fprintf(stderr, "[FDC] PC=%04X cyc=%llu write $%04X = %02X\n",
                emu->cpu.PC, (unsigned long long)emu->cpu.cycles, addr, value);
    }
    jasmin_write(&emu->jasmin, addr, value);
    /* Sync Jasmin banking flags to the memory system. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
    return true;
}

/* Savestate (section "JAS"): emitted only when the Jasmin is present. The
 * disk images go through the DSK section (savestate.c), read BEFORE it. */
static bool jasmin_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_jasmin) return false;
    return jasmin_save(&emu->jasmin, fp);
}
static void jasmin_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    jasmin_load(&emu->jasmin, fp, size);
    /* Same latch-to-memory synchronisation as jasmin_dev_write. */
    emu->memory.jasmin_olay   = emu->jasmin.olay;
    emu->memory.jasmin_romdis = emu->jasmin.romdis;
}

/* ── Ticks: exactly the operations of the old io_bus_tick, per device ─────── */
static void microdisc_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->microdisc.fdc, cycles);
}
static void jasmin_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->jasmin.fdc, cycles);
}
static void loci_dev_tick(emulator_t* emu, int cycles) {
    fdc_ticktock(&emu->loci.dsk_fdc, cycles);
    loci_adj_tick(&emu->loci, cycles);
}
static void acia_dev_tick(emulator_t* emu, int cycles) {
    acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
    acia_tick(&emu->acia, cycles);
}

/* Position de chaque device dans io_bus[] (= priorité de dispatch). */
enum { DEV_LOCI, DEV_ACIA, DEV_MICRODISC, DEV_JASMIN, DEV_COUNT };

#define PRESENT(flag) offsetof(emulator_t, flag)

static const io_device_t io_bus[DEV_COUNT] = {
    /* (LOCI: no .ost section — caveat of the file backend's OS handles.) */
    [DEV_LOCI] = { .name = "loci", .claims = loci_dev_claims, .read = loci_dev_read,
                   .write = loci_dev_write,
                   .present_off = PRESENT(has_loci), .tick = loci_dev_tick },
    /* ACIA: its "SER" section is written by savestate.c (historical). */
    [DEV_ACIA] = { .name = "acia", .claims = acia_dev_claims, .read = acia_dev_read,
                   .write = acia_dev_write, .peek = acia_dev_peek,
                   .present_off = PRESENT(has_serial), .tick = acia_dev_tick },
    /* Microdisc : sections FDC/MDC/DSK/BAD écrites par savestate.c (historique). */
    [DEV_MICRODISC] = { .name = "microdisc", .claims = microdisc_dev_claims,
                        .read = microdisc_dev_read, .write = microdisc_dev_write,
                        .present_off = PRESENT(has_microdisc), .tick = microdisc_dev_tick },
    [DEV_JASMIN] = { .name = "jasmin", .claims = jasmin_dev_claims, .read = jasmin_dev_read,
                     .write = jasmin_dev_write,
                     .save_tag = "JAS\0", .save = jasmin_dev_save, .load = jasmin_dev_load,
                     .present_off = PRESENT(has_jasmin), .tick = jasmin_dev_tick },
};

/* ORDRE DES TICKS, distinct de l'ordre de dispatch et PRÉSERVÉ à l'identique de
 * l'ancien cpu_cycle_tick (microdisc → jasmin → loci → acia), puis les cartes en
 * modules dans l'ordre de cards_list.h (dtl2000 → mageco → sp0256 → mea8000) :
 * iso-comportement par construction. */
static const io_device_t* const io_bus_tick_order[] = {
    &io_bus[DEV_MICRODISC], &io_bus[DEV_JASMIN], &io_bus[DEV_LOCI], &io_bus[DEV_ACIA],
};

/* Effective dispatch table: core devices, with the cards as modules
 * inserted before their anchor (bus_before), copied into a contiguous
 * array (same shape as before for savestate.c: the order of the .ost sections
 * is that of this table). Built on first use. */
#define IO_BUS_MAX (DEV_COUNT + 32)
static io_device_t s_bus[IO_BUS_MAX];
static int s_bus_n = -1;

static const char* bus_anchor(int m) { return k_card_modules[m]->bus_before; }
static int bus_count(int m) { return k_card_modules[m]->bus ? 1 : 0; }
static const char* bus_key(int m, int j) { (void)j; return k_card_modules[m]->bus->name; }
static void bus_emit(int m, int j, void* ctx) {
    (void)j; (void)ctx;
    if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = *k_card_modules[m]->bus;
}
static void bus_emit_core(int i, void* ctx) {
    (void)ctx;
    if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = io_bus[i];
}

static void io_bus_build(void) {
    const char* names[DEV_COUNT];
    for (int i = 0; i < DEV_COUNT; i++) names[i] = io_bus[i].name;
    s_bus_n = 0;
    card_modules_place(names, DEV_COUNT, bus_anchor, bus_count, bus_key,
                       bus_emit, bus_emit_core, NULL);
}

const io_device_t* io_bus_find(emulator_t* emu, uint16_t addr) {
    if (__builtin_expect(s_bus_n < 0, 0)) io_bus_build();
    for (int i = 0; i < s_bus_n; i++)
        if (s_bus[i].claims(emu, addr))
            return &s_bus[i];
    return NULL;
}

const io_device_t* io_bus_find_write(emulator_t* emu, uint16_t addr) {
    if (__builtin_expect(s_bus_n < 0, 0)) io_bus_build();
    for (int i = 0; i < s_bus_n; i++) {
        bool (*cw)(emulator_t*, uint16_t) = s_bus[i].claims_write ? s_bus[i].claims_write
                                                                  : s_bus[i].claims;
        if (cw(emu, addr))
            return &s_bus[i];
    }
    return NULL;
}

const io_device_t* io_bus_devices(int* count) {
    if (s_bus_n < 0) io_bus_build();
    if (count) *count = s_bus_n;
    return s_bus;
}

/* Tick of the timed bus devices, in io_bus_tick_order. */
void io_bus_tick(emulator_t* emu, int cycles) {
    const char* base = (const char*)emu;
    /* Called on EVERY cycle: once unrolled, the loop over a constant table
     * folds into flag tests + direct calls (measured cost: +22 % per frame
     * without unrolling). `expect(…, 0)` keeps the common case — no device —
     * on the straight-line path, with calls out of line (+7 % without it). */
#pragma GCC unroll 16
    for (size_t i = 0; i < sizeof(io_bus_tick_order) / sizeof(io_bus_tick_order[0]); i++) {
        const io_device_t* d = io_bus_tick_order[i];
        if (__builtin_expect(*(const bool*)(base + d->present_off), 0))
            d->tick(emu, cycles);
    }
    /* Cards as modules (after the core, in list order): generated direct
     * calls (card_ticks.h). */
    card_modules_tick(emu, cycles);
}
