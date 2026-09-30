/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file io_device.h
 * @brief "I/O bus peripheral" contract — abstraction of the page 3 dispatch.
 *
 * Architectural goal: move out of main.c the cascade of `if (has_X &&
 * X_addr_in_range(addr)) return X_read(...)` (25 hard-wired peripherals)
 * in favour of a **peripheral table** that the dispatch walks through.
 * Adding/removing a peripheral = registering/removing an entry, without
 * touching the core (see docs/architecture/io-bus.md).
 *
 * The functions receive the `emulator_t*` context (and not a mere `self`)
 * because some `claims` are conditional and cross-dependent (ACIA<->Microdisc,
 * LOCI MIA reliability, etc.): they need to see the other subsystems.
 *
 * Concerns ONLY the peripherals that claim an address range
 * (Microdisc, ACIA, LOCI, DTL2000, Mageco, ULA-NG). The peripherals
 * "attached to a port" (joystick/PSG, printer/VIA, cassette/VIA) keep
 * their port callback model; the core (CPU/memory/VIA/ULA/PSG) is
 * not a peripheral.
 */
#ifndef IO_DEVICE_H
#define IO_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>   /* FILE* for the serialization hooks */
#include <stddef.h>  /* size_t, offsetof */

struct emulator_s;   /* full context (forward-decl: avoids the include cycle) */

typedef struct io_device_s {
    const char* name;
    /** True if the peripheral owns this address *right now* for **reading**
     *  (presence + range + possible cross conditions). Also serves as the
     *  default write claim when `claims_write` is NULL. */
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);
    /** Reads one byte at `addr` (called only if claims() returned true). */
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    /** Writes one byte at `addr`. Returns **true if the write is consumed**,
     *  **false to let it fall through to the VIA** (fallback). Ordinary
     *  peripherals (exclusive range) always return true; this false lets
     *  the ULA-NG watch for its unlock sequence in its window while
     *  letting unrecognised writes through, exactly like the VIA. */
    bool    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value);
    /** Separate write claim (optional, NULL → reuses `claims`). Useful
     *  when a peripheral must see writes it does not intercept on
     *  reads: the locked ULA-NG must receive the writes to its window
     *  (to spot 'N','G') while its reads fall through to the VIA. */
    bool    (*claims_write)(struct emulator_s* emu, uint16_t addr);

    /* ── State serialization (savestate .ost), optional ──────────────────────
     * Fills a gap: bus devices had no .ost section. Each device serializes
     * ITS OWN section, without savestate.c knowing about it (coupling
     * avoided: the table is handed to it via savestate_set_io_devices).
     * The .ost format uses self-describing sections (tag+size); unknown
     * sections are skipped → backward/forward compatible. */

    /** 4-byte section tag (e.g. "UNG\0"). NULL → device not serialized. */
    const char* save_tag;
    /** Writes the section payload to `fp` (the tag/size header is handled
     *  by the caller). Returns **false to emit NO section at all** (default
     *  state → `.ost` unchanged, zero regression for the usual use). */
    bool    (*save)(struct emulator_s* emu, FILE* fp);
    /** Reads back `size` payload bytes written by `save` (called if a
     *  section tag matches `save_tag`). */
    void    (*load)(struct emulator_s* emu, FILE* fp, uint32_t size);

    /* ── Observation read, without side effect (optional) ────────────────────
     * `read()` may mutate state (reading ACIA DATA clears RDRF; reading STATUS
     * clears the IRQ). An observer (debugger, monitor, dump, remote
     * display) must see the register WITHOUT consuming it, otherwise it
     * silently steals RX bytes. NULL → the caller falls back to `read`
     * (identical to the historical behaviour). Only devices with destructive
     * reads (ACIA) provide a `peek`. */
    uint8_t (*peek)(struct emulator_s* emu, uint16_t addr);

    /* ── Cycle de vie : avance temporelle (optionnelle) ─────────────────────
     * `tick` avance le périphérique de `cycles` cycles CPU (FDC, ACIA, synthèse
     * vocale…). io_bus_tick() appelle les ticks dans un ORDRE EXPLICITE
     * (io_bus_tick_order, distinct de l'ordre de dispatch) et seulement pour les
     * périphériques présents : `present_off` est la position du drapeau `has_X`
     * dans emulator_t (offsetof), testé sans appel de fonction — un périphérique
     * absent ne coûte qu'une lecture par cycle. NULL → pas de tick. */
    size_t  present_off;
    void    (*tick)(struct emulator_s* emu, int cycles);
} io_device_t;

#endif /* IO_DEVICE_H */
