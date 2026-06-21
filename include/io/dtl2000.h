/**
 * @file dtl2000.h
 * @brief Digitelec DTL 2000 — faithful PIA 6821 + ACIA 6850 modem card
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 *
 * Cycle-functional model of the *real* Digitelec DTL 2000 V23 modem card as
 * seen on the Oric: a memory-mapped 6-byte window at $03F8-$03FD exposing a
 * Motorola PIA 6821 (4 registers) + a Motorola ACIA 6850 (2 registers).
 *
 * This is distinct from the behavioural `digitelec` serial backend (which is
 * driven through the emulated MOS 6551 ACIA at $031C). Here the host program
 * pilots the modem exactly as the period software did: PIA drives the line /
 * mode, ACIA carries the serial data.
 *
 * Register map (offset = address - base):
 *   $03F8  PIA Port A   — DDRA (CRA bit2=0) / Output Register A (CRA bit2=1)
 *   $03F9  PIA CRA      — Control register A (only bit2 used by the DTL)
 *   $03FA  PIA Port B   — DDRB / ORB (UNUSED by the DTL V23 card)
 *   $03FB  PIA CRB      — Control register B (UNUSED)
 *   $03FC  ACIA 6850    — Control (write) / Status (read)
 *   $03FD  ACIA 6850    — Tx Data (write) / Rx Data (read)
 *
 * Register semantics extracted by OCR from the period programming manual
 * ("Programmation carte DTL V23", chips identical to the Apple II variant):
 *   - PIA ORA bit 2 = line connection: 0 = line closed (CONNECTED),
 *                                       1 = line open  (DISCONNECTED).
 *   - PIA ORA bit 4 = modulation mode: 1 = asymmetric V23 (75 TX / 1200 RX),
 *                                       0 = symmetric V23 (1200 / 1200).
 *   - ACIA control $03 = master reset; word-select bits 2-4, divide bits 0-1,
 *                        transmit control (RTS/TIE) bits 5-6, RIE bit 7.
 *   - ACIA status bit0 = RDRF, bit1 = TDRE, bit2 = DCD (1 = carrier lost),
 *                  bit3 = CTS (1 = not clear to send).
 *
 * ⚠️ Page-3 decode conflict: $03F8-$03FF aliases the VIA 6522 mirror
 * ($0300-$030F repeats across the page) and, on real hardware, the Jasmin
 * disc electronics. The router intercepts $03F8-$03FD ahead of the VIA
 * fallback when the DTL 2000 is enabled.
 */

#ifndef DTL2000_H
#define DTL2000_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "io/pia6821.h"

/* Forward declarations */
typedef struct serial_backend_s serial_backend_t;
typedef struct emulator_s emulator_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  I/O Address Map
 * ═══════════════════════════════════════════════════════════════════════ */

#define DTL2000_DEFAULT_BASE  0x03F8  /* Oric base (confirmed period source) */
#define DTL2000_REG_SPAN      6       /* 4 PIA + 2 ACIA registers */

/* Register offsets (address - base) */
#define DTL_REG_PIA_A    0  /* PIA Port A: DDRA / ORA */
#define DTL_REG_PIA_CRA  1  /* PIA Control A */
#define DTL_REG_PIA_B    2  /* PIA Port B: DDRB / ORB (unused) */
#define DTL_REG_PIA_CRB  3  /* PIA Control B (unused) */
#define DTL_REG_ACIA_CS  4  /* ACIA Control (W) / Status (R) */
#define DTL_REG_ACIA_D   5  /* ACIA Tx (W) / Rx (R) */

/* Emulated card clock (the Oric system clock drives the bus) */
#define DTL2000_CLOCK_HZ  1000000UL

/* ═══════════════════════════════════════════════════════════════════════
 *  PIA 6821
 * ═══════════════════════════════════════════════════════════════════════ */

#define DTL_PIA_CR_DDR_SEL  0x04  /* CR bit2: 0 → access DDR, 1 → access OR */

/* Port A output register bits (DTL 2000 wiring) */
#define DTL_PIA_A_LINE  0x04  /* bit2: 0 = line closed (connected) */
#define DTL_PIA_A_MODE  0x10  /* bit4: 1 = asymmetric V23, 0 = symmetric */

/* Reference OR values from the manual (decimal → hex) */
#define DTL_OR_ASYM_CONNECT     0xD0  /* 208: asym, line closed */
#define DTL_OR_ASYM_DISCONNECT  0xD4  /* 212: asym, line open */
#define DTL_OR_SYM_CONNECT      0xC0  /* 192: sym,  line closed */
#define DTL_OR_SYM_DISCONNECT   0xC4  /* 196: sym,  line open */
#define DTL_DDRA_INIT           0xF4  /* 244: in=b0,b1,b3 ; out=b2,b4-b7 */

/* ═══════════════════════════════════════════════════════════════════════
 *  ACIA 6850 — Control register (write $03FC)
 * ═══════════════════════════════════════════════════════════════════════ */

#define DTL_ACIA_CR_CDS_MASK   0x03  /* Counter divide select (11 = reset) */
#define DTL_ACIA_CR_MASTER_RST 0x03  /* CDS=11 → master reset */
#define DTL_ACIA_CR_WS_MASK    0x1C  /* Word select (bits 2-4) */
#define DTL_ACIA_CR_WS_SHIFT   2
#define DTL_ACIA_CR_TC_MASK    0x60  /* Transmit control RTS/TIE (bits 5-6) */
#define DTL_ACIA_CR_TC_SHIFT   5
#define DTL_ACIA_CR_RIE        0x80  /* Receive interrupt enable */

/* Transmit-control field values (bits 6-5) */
#define DTL_TC_RTS_LOW_TIE_OFF   0x00  /* /RTS low (emit carrier), TX IRQ off */
#define DTL_TC_RTS_LOW_TIE_ON    0x01  /* /RTS low, TX IRQ on */
#define DTL_TC_RTS_HIGH_TIE_OFF  0x02  /* /RTS high (no carrier), TX IRQ off */
#define DTL_TC_RTS_LOW_BREAK     0x03  /* /RTS low, transmit break */

/* Reference control values from the manual */
#define DTL_ACIA_RESET     0x03  /* master reset */
#define DTL_ACIA_ASYM_CFG  0x49  /* 7E1, ÷16, no emission (RTS high) */
#define DTL_ACIA_ASYM_EMIT 0x09  /* 7E1, ÷16, emit carrier (RTS low) */
#define DTL_ACIA_SYM_CFG   0x55  /* 8N1, ÷16, no emission */
#define DTL_ACIA_SYM_EMIT  0x15  /* 8N1, ÷16, emit carrier */

/* ═══════════════════════════════════════════════════════════════════════
 *  ACIA 6850 — Status register (read $03FC)
 * ═══════════════════════════════════════════════════════════════════════ */

#define DTL_ACIA_SR_RDRF  0x01  /* Receive Data Register Full */
#define DTL_ACIA_SR_TDRE  0x02  /* Transmit Data Register Empty */
#define DTL_ACIA_SR_DCD   0x04  /* Data Carrier Detect (1 = carrier lost) */
#define DTL_ACIA_SR_CTS   0x08  /* Clear To Send (1 = NOT clear) */
#define DTL_ACIA_SR_FE    0x10  /* Framing Error */
#define DTL_ACIA_SR_OVRN  0x20  /* Receiver Overrun */
#define DTL_ACIA_SR_PE    0x40  /* Parity Error */
#define DTL_ACIA_SR_IRQ   0x80  /* Interrupt Request */

/* V23 baud presets */
#define DTL_V23_RX_BAUD  1200
#define DTL_V23_TX_BAUD  75

/* ═══════════════════════════════════════════════════════════════════════
 *  Device structure
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct dtl2000_s {
    uint16_t base_addr;          /**< I/O base ($03F8 by default) */

    /* PIA 6821 (Port A drives the line/mode; Port B unused by the card). */
    pia6821_t pia;

    /* ACIA 6850 registers */
    uint8_t acia_control;        /**< last control write */
    uint8_t acia_status;         /**< status (read) */
    uint8_t acia_rdr;            /**< received byte */
    uint8_t acia_tdr;            /**< byte being transmitted */

    /* Decoded modem state */
    bool    line_connected;      /**< ORA bit2 == 0 (line closed) */
    bool    symmetric;           /**< ORA bit4 == 0 (symmetric V23) */
    bool    tx_carrier;          /**< RTS low → emitting carrier */
    uint8_t bitmask;             /**< data mask (0x7F=7-bit, 0xFF=8-bit) */
    uint8_t framebits;           /**< total bits per frame (timing) */
    uint32_t rx_baud, tx_baud;   /**< current baud rates */

    /* Timing (cycle counters) */
    int32_t rx_cycles, rx_reload;
    int32_t tx_cycles, tx_reload;
    bool    tx_busy;             /**< a byte is shifting out */

    /* Transport backend (loopback, TCP, PTY) */
    serial_backend_t* backend;

    /* Statistics */
    uint32_t tx_count, rx_count;

    /* Optional TX/RX trace (NULL = off). Lines: dir, hex, ascii, status. */
    FILE* trace;

    /* Optional CPU IRQ routing (left NULL = polling mode, as the DTL software) */
    void (*irq_set)(emulator_t* emu);
    void (*irq_clr)(emulator_t* emu);
    emulator_t* irq_userdata;
} dtl2000_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/** @brief Initialise the device to power-on state at the given base address. */
void dtl2000_init(dtl2000_t* dev, uint16_t base_addr);

/** @brief Reset (equivalent to the PIA/ACIA reset lines). */
void dtl2000_reset(dtl2000_t* dev);

/** @brief Read a register ($03F8-$03FD). */
uint8_t dtl2000_read(dtl2000_t* dev, uint16_t addr);

/** @brief Write a register ($03F8-$03FD). */
void dtl2000_write(dtl2000_t* dev, uint16_t addr, uint8_t value);

/** @brief Advance TX/RX timing by N CPU cycles (aggregated per instruction). */
void dtl2000_tick(dtl2000_t* dev, int cycles);

/** @brief Attach a serial backend for transport (loopback/TCP/PTY). */
void dtl2000_set_backend(dtl2000_t* dev, serial_backend_t* backend);

/** @brief Enable a human-readable TX/RX byte trace to @p filename (NULL = off). */
void dtl2000_set_trace(dtl2000_t* dev, const char* filename);

/** @brief True if @p addr falls inside the device's register window. */
bool dtl2000_addr_in_range(const dtl2000_t* dev, uint16_t addr);

#endif /* DTL2000_H */
