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

#include <stdio.h>

/* ── I/O bus: registered peripherals (docs/architecture/io-bus.md) ──────
 * Each peripheral provides claims/read/write; the dispatch walks the table.
 * Table order = priority: LOCI first (overlaps the Microdisc via
 * TAP $0315-$0317), then ACIA (owns $031C-$031F if present) before Microdisc.
 * The ULA-NG comes last: its write must always receive the byte in its
 * window (even when locked, to watch for the 'N','G' sequence) and `ula_ng_write`
 * *returns* whether it consumed it — otherwise fall back to the VIA; hence the
 * separate `claims_write` and the boolean return of `write`. "Strangler" pattern. */

/* LOCI (sodiumlb): three disjoint sub-windows, dispatched internally.
 *  - MIA $03A0-$03BF (independent of the other peripherals);
 *  - TAP $0315-$0317: replaces the tape interface, overlaps the Microdisc
 *    $0310-$031F → priority (LOCI is at the head of the table);
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

/* MEA8000 TMPI "Synthétiseur Vocal" (Philips formant): data at base_addr,
 * command at base_addr+1 (default $03F0/$03F1). Mutually exclusive with the SP0256 ($03F1). */
static bool mea8000_dev_claims(emulator_t* emu, uint16_t addr) {
    return emu->has_mea8000 &&
           (addr == emu->mea8000.base_addr ||
            addr == (uint16_t)(emu->mea8000.base_addr + 1));
}
static uint8_t mea8000_dev_read(emulator_t* emu, uint16_t addr) {
    return mea8000_read(&emu->mea8000, addr);
}
static bool mea8000_dev_write(emulator_t* emu, uint16_t addr, uint8_t value) {
    mea8000_write(&emu->mea8000, addr, value);
    return true;
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

static const io_device_t io_bus[] = {
    /* (save_tag/save/load NULL: these devices have no .ost section yet —
     * to be migrated on the same model as the ULA-NG; LOCI has the OS-handles caveat.) */
    { "loci",      loci_dev_claims,      loci_dev_read,      loci_dev_write,      NULL, NULL, NULL, NULL },
    { "acia",      acia_dev_claims,      acia_dev_read,      acia_dev_write,      NULL, NULL, NULL, NULL,
      acia_dev_peek },
    { "mageco",    mageco_dev_claims,    mageco_dev_read,    mageco_dev_write,    NULL,
      "MAG\0",     mageco_dev_save,      mageco_dev_load },
    { "microdisc", microdisc_dev_claims, microdisc_dev_read, microdisc_dev_write, NULL, NULL, NULL, NULL },
    { "jasmin",    jasmin_dev_claims,    jasmin_dev_read,    jasmin_dev_write,    NULL, NULL, NULL, NULL },
    { "sp0256",    sp0256_dev_claims,    sp0256_dev_read,    sp0256_dev_write,    NULL, NULL, NULL, NULL },
    { "mea8000",   mea8000_dev_claims,   mea8000_dev_read,   mea8000_dev_write,   NULL, NULL, NULL, NULL },
    { "dtl2000",   dtl2000_dev_claims,   dtl2000_dev_read,   dtl2000_dev_write,   NULL,
      "DTL\0",     dtl2000_dev_save,     dtl2000_dev_load },
    /* ULA-NG last (fallback before VIA). Separate claims_write: sees the
     * writes to its window even when locked ('N','G' watch). Serialised via the
     * "UNG" section (emitted only if unlocked → .ost unchanged otherwise). */
    { "ula-ng",    ula_ng_dev_claims,    ula_ng_dev_read,    ula_ng_dev_write,    ula_ng_dev_claims_write,
      "UNG\0",     ula_ng_dev_save,      ula_ng_dev_load },
};
static const int io_bus_count = (int)(sizeof(io_bus) / sizeof(io_bus[0]));

const io_device_t* io_bus_find(emulator_t* emu, uint16_t addr) {
    for (int i = 0; i < io_bus_count; i++)
        if (io_bus[i].claims(emu, addr))
            return &io_bus[i];
    return NULL;
}

const io_device_t* io_bus_find_write(emulator_t* emu, uint16_t addr) {
    for (int i = 0; i < io_bus_count; i++) {
        bool (*cw)(emulator_t*, uint16_t) = io_bus[i].claims_write ? io_bus[i].claims_write
                                                                   : io_bus[i].claims;
        if (cw(emu, addr))
            return &io_bus[i];
    }
    return NULL;
}

const io_device_t* io_bus_devices(int* count) {
    if (count) *count = io_bus_count;
    return io_bus;
}

/* Tick of the timed bus peripherals. HISTORICAL ORDER PRESERVED exactly as
 * in the old cpu_cycle_tick (microdisc → loci → acia → dtl → mageco)
 * → identical behaviour by construction (and not merely by the independence of the
 * ticks). A generic loop over `io_bus[]` would reorder them; so it is not done
 * here (see docs/architecture/io-bus.md §6: non-uniform lifecycle hooks). */
void io_bus_tick(emulator_t* emu, int cycles) {
    if (emu->has_microdisc) fdc_ticktock(&emu->microdisc.fdc, cycles);
    if (emu->has_jasmin)    fdc_ticktock(&emu->jasmin.fdc, cycles);
    if (emu->has_loci) {
        fdc_ticktock(&emu->loci.dsk_fdc, cycles);
        loci_adj_tick(&emu->loci, cycles);
    }
    if (emu->has_serial) {
        acia_set_trace_cycle(&emu->acia, emu->cpu.cycles);
        acia_tick(&emu->acia, cycles);
    }
    if (emu->has_dtl2000) dtl2000_tick(&emu->dtl2000, cycles);
    if (emu->has_mageco)  mageco_tick(&emu->mageco, cycles);
    if (emu->has_sp0256)  sp0256_tick(&emu->sp0256, cycles);
    if (emu->has_mea8000) mea8000_tick(&emu->mea8000, cycles);
}
