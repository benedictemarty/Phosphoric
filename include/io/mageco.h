/**
 * @file mageco.h
 * @brief Mageco MIDI interface — MC6850 ACIA at $03FE-$03FF
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 *
 * Cycle-functional model of the Mageco MIDI interface for the Oric (see
 * Defence-Force forum thread t=2525, "Mageco MIDI interface", revived by Dbug
 * as the "Oric-Con" MIDI shield). The card is a single Motorola MC6850 ACIA
 * memory-mapped at $03FE-$03FF, driving the two MIDI DIN-5 sockets (IN through
 * a TIL 111 optocoupler, OUT directly):
 *
 *   $03FE  ACIA Control (write) / Status (read)
 *   $03FF  ACIA Tx Data (write) / Rx Data (read)
 *
 * MIDI is a 31250-baud, 8-N-1 asynchronous current loop. The ACIA's clock pins
 * are wired to a dedicated crystal so the host program only sees the standard
 * 6850 register set; the 31250-baud cadence lives here (the chip itself has no
 * internal baud generator — see acia6850.h).
 *
 * Unlike the Digitelec DTL 2000 (dtl2000.h), there is no modem handshake: MIDI
 * is a one-way optoisolated current loop with no DCD/CTS, so those input pins
 * are held in the "carrier present / clear to send" state and the transmitter
 * always puts the written byte on the wire (RTS is not in the MIDI Tx path).
 *
 * Transport is any *transparent* serial backend (file capture/replay, loopback,
 * tcp, pty): the raw MIDI byte stream passes through unchanged. On a PC this is
 * the exact equivalent of plugging a real Oric+Mageco card into a USB-MIDI
 * interface — the byte stream is identical, only the wire is virtual.
 *
 * ⚠️ Page-3 decode conflict: $03FE-$03FF aliases the VIA 6522 mirror that
 * repeats across $0300-$03FF, and the forum explicitly warns these addresses
 * "can rise incompatibilities with other extensions". The router intercepts
 * $03FE-$03FF ahead of the VIA fallback only when the card is enabled.
 */

#ifndef MAGECO_H
#define MAGECO_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "io/acia6850.h"

/* Forward declarations */
typedef struct serial_backend_s serial_backend_t;
typedef struct emulator_s emulator_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  I/O Address Map
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAGECO_DEFAULT_BASE  0x03FE  /* Oric base (forum t=2525) */
#define MAGECO_REG_SPAN      2       /* 2 ACIA registers */

/* Register offsets (address - base) */
#define MAGECO_REG_ACIA_CS   0  /* ACIA Control (W) / Status (R) */
#define MAGECO_REG_ACIA_D    1  /* ACIA Tx (W) / Rx (R) */

/* Emulated card clock (the Oric system clock drives the bus) */
#define MAGECO_CLOCK_HZ  1000000UL

/* MIDI line rate: 31250 baud, 8 data, no parity, 1 stop (10 bits / frame). */
#define MAGECO_MIDI_BAUD     31250

/* ═══════════════════════════════════════════════════════════════════════
 *  Device structure
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct mageco_s {
    uint16_t base_addr;          /**< I/O base ($03FE by default) */

    /* ACIA 6850 (registers, status, IRQ, frame format). The DCD/CTS input pins
     * are pinned "present/clear" (no MIDI handshake); the baud clock is the
     * fixed MIDI 31250 cadence applied below. */
    acia6850_t acia;

    /* Timing (cycle counters) — both directions run at the MIDI baud. */
    int32_t rx_cycles, rx_reload;
    int32_t tx_cycles, tx_reload;
    bool    tx_busy;             /**< a byte is shifting out */

    /* Transport backend (file capture/replay, loopback, tcp, pty). */
    serial_backend_t* backend;

    /* Statistics */
    uint32_t tx_count, rx_count;

    /* Optional TX/RX trace (NULL = off). */
    FILE* trace;

    /* Optional CPU IRQ routing (left NULL = polling mode). */
    void (*irq_set)(emulator_t* emu);
    void (*irq_clr)(emulator_t* emu);
    emulator_t* irq_userdata;
} mageco_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/** @brief Initialise the device to power-on state at the given base address. */
void mageco_init(mageco_t* dev, uint16_t base_addr);

/** @brief Reset (equivalent to the ACIA reset line). */
void mageco_reset(mageco_t* dev);

/** @brief Read a register ($03FE-$03FF). */
uint8_t mageco_read(mageco_t* dev, uint16_t addr);

/** @brief Write a register ($03FE-$03FF). */
void mageco_write(mageco_t* dev, uint16_t addr, uint8_t value);

/** @brief Advance TX/RX timing by N CPU cycles (aggregated per instruction). */
void mageco_tick(mageco_t* dev, int cycles);

/** @brief Attach a serial backend for transport (file/loopback/tcp/pty). */
void mageco_set_backend(mageco_t* dev, serial_backend_t* backend);

/** @brief Enable a human-readable TX/RX byte trace to @p filename (NULL = off). */
void mageco_set_trace(mageco_t* dev, const char* filename);

/** @brief True if @p addr falls inside the device's register window. */
bool mageco_addr_in_range(const mageco_t* dev, uint16_t addr);

#endif /* MAGECO_H */
