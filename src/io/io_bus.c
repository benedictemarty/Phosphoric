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
#include "card_module.h"   /* cartes en modules : leurs périphériques de bus */
#include "card_ticks.h"    /* leurs ticks par cycle, en appels directs */

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

/* Mageco / ORICON MIDI (ACIA 6850): $03FE-$03FF or $031C-$031E. */
static bool mageco_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_mageco && mageco_addr_in_range(&emu->mageco, addr);
}
static uint8_t mageco_dev_read(emulator_t* emu, uint16_t addr) {
    return mageco_read(&emu->mageco, addr);
}
static bool mageco_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    mageco_write(&emu->mageco, addr, value);
    return true;
}
/* Savestate ("MAG" section): emitted only if the Mageco is present →
 * .ost unchanged otherwise. Host transport not restored (see mageco_save). */
static bool mageco_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_mageco) return false;
    return mageco_save(&emu->mageco, fp);
}
static void mageco_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    mageco_load(&emu->mageco, fp, size);
}

/* Microdisc WD1793: $0310-$031F (the ACIA, registered earlier, already owns
 * $031C-$031F if present → no internal test here). */
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
/* SP0256 Mageco "Synthétiseur Vocal" (GI SP0256-AL2): single port at
 * emu->sp0256.base_addr (default $03F1). Audio output mixed into the PSG. */
static bool sp0256_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_sp0256 && addr == emu->sp0256.base_addr;
}
static uint8_t sp0256_dev_read(emulator_t* emu, uint16_t addr) {
    return sp0256_read(&emu->sp0256, addr);
}
static bool sp0256_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    sp0256_write(&emu->sp0256, addr, value);
    return true;
}

/* Savestate (sections "SPO" / "MEA"): emitted only when the card is present. */
static bool sp0256_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_sp0256) return false;
    return sp0256_save(&emu->sp0256, fp);
}
static void sp0256_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    sp0256_load(&emu->sp0256, fp, size);
}

/* Digitelec DTL 2000 (PIA 6821 + ACIA 6850): $03F8-$03FD (exclusive range). */
static bool dtl2000_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_dtl2000 && dtl2000_addr_in_range(&emu->dtl2000, addr);
}
static uint8_t dtl2000_dev_read(emulator_t* emu, uint16_t addr) {
    return dtl2000_read(&emu->dtl2000, addr);
}
static bool dtl2000_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    dtl2000_write(&emu->dtl2000, addr, value);
    return true;
}
/* Savestate ("DTL" section): emitted only if the DTL2000 is present →
 * .ost unchanged otherwise. Host transport not restored (see dtl2000_save). */
static bool dtl2000_dev_save(emulator_t* emu, FILE* fp) {
    if (!emu->has_dtl2000) return false;
    return dtl2000_save(&emu->dtl2000, fp);
}
static void dtl2000_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    dtl2000_load(&emu->dtl2000, fp, size);
}

/* ULA-NG $0340-$035F: last peripheral on the bus, before the VIA fallback.
 *  - Read: answers only when unlocked (`claims`); when locked, the window
 *    falls back to the VIA mirror (indistinguishable).
 *  - Write: `claims_write` = window only → the ULA-NG sees the writes
 *    even when locked, to watch for the 'N','G' sequence. `ula_ng_write` returns
 *    whether it consumed; otherwise the dispatch falls back to the VIA (bit-exact). */
static bool ula_ng_dev_claims(emulator_t* emu, uint16_t addr) {
    return ula_ng_active(&emu->ula_ng) && ula_ng_addr_in_window(addr);
}
static bool ula_ng_dev_claims_write(emulator_t* emu, uint16_t addr) {
    (void)emu;
    return ula_ng_addr_in_window(addr);
}
static uint8_t ula_ng_dev_read(emulator_t* emu, uint16_t addr) {
    return ula_ng_read(&emu->ula_ng, addr);
}
static bool ula_ng_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    if (!ula_ng_write(&emu->ula_ng, addr, value))
        return false;   /* not consumed (locked, neutral byte) → VIA fallback */
    /* Write consumed: synchronise the raster IRQ line (a write to
     * NG_STATUS acknowledges → deassertion). */
    if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
    else                          cpu_irq_clear(&emu->cpu, IRQF_ULANG);
    return true;
}
/* Savestate ("UNG" section): delegated to the module (POD, same build, size guard). */
static bool ula_ng_dev_save(emulator_t* emu, FILE* fp) {
    return ula_ng_save(&emu->ula_ng, fp);
}
static void ula_ng_dev_load(emulator_t* emu, FILE* fp, uint32_t size) {
    ula_ng_load(&emu->ula_ng, fp, size);
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
static void dtl2000_dev_tick(emulator_t* emu, int cycles) { dtl2000_tick(&emu->dtl2000, cycles); }
static void mageco_dev_tick(emulator_t* emu, int cycles)  { mageco_tick(&emu->mageco, cycles); }
static void sp0256_dev_tick(emulator_t* emu, int cycles)  { sp0256_tick(&emu->sp0256, cycles); }

/* Position of each device in io_bus[] (= dispatch priority). */
enum { DEV_LOCI, DEV_ACIA, DEV_MAGECO, DEV_MICRODISC, DEV_JASMIN, DEV_SP0256,
       DEV_DTL2000, DEV_ULA_NG, DEV_COUNT };

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
    [DEV_MAGECO] = { .name = "mageco", .claims = mageco_dev_claims, .read = mageco_dev_read,
                     .write = mageco_dev_write,
                     .save_tag = "MAG\0", .save = mageco_dev_save, .load = mageco_dev_load,
                     .present_off = PRESENT(has_mageco), .tick = mageco_dev_tick },
    /* Microdisc: sections FDC/MDC/DSK/BAD written by savestate.c (historical). */
    [DEV_MICRODISC] = { .name = "microdisc", .claims = microdisc_dev_claims,
                        .read = microdisc_dev_read, .write = microdisc_dev_write,
                        .present_off = PRESENT(has_microdisc), .tick = microdisc_dev_tick },
    [DEV_JASMIN] = { .name = "jasmin", .claims = jasmin_dev_claims, .read = jasmin_dev_read,
                     .write = jasmin_dev_write,
                     .save_tag = "JAS\0", .save = jasmin_dev_save, .load = jasmin_dev_load,
                     .present_off = PRESENT(has_jasmin), .tick = jasmin_dev_tick },
    [DEV_SP0256] = { .name = "sp0256", .claims = sp0256_dev_claims, .read = sp0256_dev_read,
                     .write = sp0256_dev_write,
                     .save_tag = "SPO\0", .save = sp0256_dev_save, .load = sp0256_dev_load,
                     .present_off = PRESENT(has_sp0256), .tick = sp0256_dev_tick },
    [DEV_DTL2000] = { .name = "dtl2000", .claims = dtl2000_dev_claims, .read = dtl2000_dev_read,
                      .write = dtl2000_dev_write,
                      .save_tag = "DTL\0", .save = dtl2000_dev_save, .load = dtl2000_dev_load,
                      .present_off = PRESENT(has_dtl2000), .tick = dtl2000_dev_tick },
    /* ULA-NG last (fallback before VIA). Separate claims_write: sees the
     * writes to its window even when locked (watching for 'N','G'). Serialised
     * via the "UNG" section (emitted only when unlocked → .ost unchanged otherwise).
     * No tick: the ULA-NG advances with the video. */
    [DEV_ULA_NG] = { .name = "ula-ng", .claims = ula_ng_dev_claims, .read = ula_ng_dev_read,
                     .write = ula_ng_dev_write, .claims_write = ula_ng_dev_claims_write,
                     .save_tag = "UNG\0", .save = ula_ng_dev_save, .load = ula_ng_dev_load },
};

/* ORDRE DES TICKS, distinct de l'ordre de dispatch et PRÉSERVÉ à l'identique de
 * l'ancien cpu_cycle_tick (microdisc → jasmin → loci → acia → dtl → mageco →
 * sp0256), puis les cartes en modules dans l'ordre de k_card_modules (mea8000) :
 * iso-comportement par construction. */
static const io_device_t* const io_bus_tick_order[] = {
    &io_bus[DEV_MICRODISC], &io_bus[DEV_JASMIN], &io_bus[DEV_LOCI], &io_bus[DEV_ACIA],
    &io_bus[DEV_DTL2000], &io_bus[DEV_MAGECO], &io_bus[DEV_SP0256],
};

/* Table de répartition effective : périphériques du cœur, avec les cartes en
 * modules insérées avant leur ancre (bus_before), copiés dans un tableau
 * contigu (même forme qu'avant pour savestate.c : l'ordre des sections .ost est
 * celui de cette table). Construite au premier usage. */
#define IO_BUS_MAX (DEV_COUNT + 32)
static io_device_t s_bus[IO_BUS_MAX];
static int s_bus_n = -1;

static void bus_add_modules_before(const char* name, bool* placed) {
    for (int m = 0; m < k_card_module_count; m++) {
        const card_module_t* mod = k_card_modules[m];
        if (placed[m] || !mod->bus) continue;
        if (name ? (mod->bus_before && strcmp(mod->bus_before, name) == 0) : true) {
            placed[m] = true;
            bus_add_modules_before(mod->bus->name, placed);
            if (s_bus_n < IO_BUS_MAX) s_bus[s_bus_n++] = *mod->bus;
        }
    }
}

static void io_bus_build(void) {
    bool placed[64] = { false };
    s_bus_n = 0;
    for (int i = 0; i < DEV_COUNT; i++) {
        bus_add_modules_before(io_bus[i].name, placed);
        s_bus[s_bus_n++] = io_bus[i];
    }
    bus_add_modules_before(NULL, placed);          /* sans ancre : en fin de table */
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
    /* Cartes en modules (après le cœur, dans l'ordre de la liste) : appels
     * directs générés (card_ticks.h). */
    card_modules_tick(emu, cycles);
}
