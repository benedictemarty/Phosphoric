/**
 * @file acia6850.h
 * @brief Motorola MC6850 ACIA (Asynchronous Communications Interface Adapter)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 *
 * Standalone, reusable MC6850 model: the pure UART register/IRQ logic, with the
 * baud clock and the actual byte transport left to the host (which feeds RX
 * bytes in and reads TX bytes out at its own cadence). DCD and CTS are input
 * pins driven by the host's modem/line logic.
 *
 * Distinct from the MOS 6551 ACIA (src/io/acia6551.c): the 6850 has no internal
 * baud-rate generator (the clock is external, divided /1, /16 or /64) and a
 * different, smaller register set.
 *
 * Two register addresses (RS):
 *   RS=0  Control (write)  / Status (read)
 *   RS=1  Tx Data (write)  / Rx Data (read)
 *
 * Control register:
 *   bits1-0  counter divide select (11 = master reset)
 *   bits4-2  word select (data bits / parity / stop bits)
 *   bits6-5  transmit control (RTS level + Tx interrupt enable)
 *   bit7     receive interrupt enable (RIE)
 *
 * Status register:
 *   bit0 RDRF, bit1 TDRE, bit2 DCD (1 = carrier lost), bit3 CTS (1 = not clear),
 *   bit4 FE, bit5 OVRN, bit6 PE, bit7 IRQ.
 *
 * Note: this model raises IRQ from (RIE & RDRF) and (Tx-IRQ-enable & TDRE); the
 * DCD-loss interrupt latch of the real chip is not modelled (the only on-Oric
 * user, the Digitelec DTL 2000, polls DCD rather than taking its interrupt).
 */

#ifndef ACIA6850_H
#define ACIA6850_H

#include <stdint.h>
#include <stdbool.h>

/* Control register (write) */
#define ACIA6850_CR_CDS_MASK    0x03  /* counter divide select */
#define ACIA6850_CR_MASTER_RST  0x03  /* CDS = 11 → master reset */
#define ACIA6850_CR_WS_MASK     0x1C  /* word select (bits 2-4) */
#define ACIA6850_CR_WS_SHIFT    2
#define ACIA6850_CR_TC_MASK     0x60  /* transmit control (bits 5-6) */
#define ACIA6850_CR_TC_SHIFT    5
#define ACIA6850_CR_RIE         0x80  /* receive interrupt enable */

/* Transmit-control field values (bits 6-5) */
#define ACIA6850_TC_RTS_LOW_TIE_OFF   0x00  /* /RTS low, Tx IRQ off */
#define ACIA6850_TC_RTS_LOW_TIE_ON    0x01  /* /RTS low, Tx IRQ on */
#define ACIA6850_TC_RTS_HIGH_TIE_OFF  0x02  /* /RTS high (no carrier), Tx IRQ off */
#define ACIA6850_TC_RTS_LOW_BREAK     0x03  /* /RTS low, transmit break */

/* Status register (read) */
#define ACIA6850_SR_RDRF  0x01  /* Receive Data Register Full */
#define ACIA6850_SR_TDRE  0x02  /* Transmit Data Register Empty */
#define ACIA6850_SR_DCD   0x04  /* Data Carrier Detect (1 = carrier lost) */
#define ACIA6850_SR_CTS   0x08  /* Clear To Send (1 = NOT clear) */
#define ACIA6850_SR_FE    0x10  /* Framing Error */
#define ACIA6850_SR_OVRN  0x20  /* Receiver Overrun */
#define ACIA6850_SR_PE    0x40  /* Parity Error */
#define ACIA6850_SR_IRQ   0x80  /* Interrupt Request */

typedef struct acia6850_s {
    uint8_t control;     /**< last control write */
    uint8_t status;      /**< status register */
    uint8_t rdr;         /**< received byte */
    uint8_t tdr;         /**< byte to transmit (masked to the frame width) */
    uint8_t framebits;   /**< total bits per frame (start+data+parity+stop) */
    uint8_t bitmask;     /**< data mask (0x7F for 7-bit, 0xFF for 8-bit) */
    bool    rts_low;     /**< /RTS asserted low (carrier emitted) */

    /* Interrupt output (called on every recompute; map to the CPU /IRQ line). */
    void  (*irq_out)(void* ud, bool active);
    void*   userdata;
} acia6850_t;

/* ── Lifecycle ── */
void    acia6850_init(acia6850_t* a);   /**< zero state, preserve callbacks */
void    acia6850_reset(acia6850_t* a);  /**< power-on register state */

/* ── CPU bus: control/status + data ── */
/** Write the control register. Returns true if this was a master reset. */
bool    acia6850_control_write(acia6850_t* a, uint8_t value);
uint8_t acia6850_status(const acia6850_t* a);
uint8_t acia6850_read_data(acia6850_t* a);            /**< clears RDRF */
void    acia6850_write_data(acia6850_t* a, uint8_t v);/**< loads TDR, clears TDRE */

/* ── Transport side: host moves bytes in/out at the baud cadence ── */
void    acia6850_rx_byte(acia6850_t* a, uint8_t b);   /**< delivers a received byte */
void    acia6850_tx_complete(acia6850_t* a);          /**< the TDR byte has shifted out */

/* ── Modem input pins ── */
void    acia6850_set_dcd(acia6850_t* a, bool carrier_present);
void    acia6850_set_cts(acia6850_t* a, bool clear_to_send);

/* ── Queries ── */
static inline bool acia6850_rdrf(const acia6850_t* a) { return (a->status & ACIA6850_SR_RDRF) != 0; }
static inline bool acia6850_tdre(const acia6850_t* a) { return (a->status & ACIA6850_SR_TDRE) != 0; }
static inline bool acia6850_rts_low(const acia6850_t* a) { return a->rts_low; }

#endif /* ACIA6850_H */
