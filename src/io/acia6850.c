/**
 * @file acia6850.c
 * @brief Motorola MC6850 ACIA — faithful UART register/IRQ model
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 *
 * See acia6850.h. The baud clock and the byte transport live in the host; this
 * module owns the control/status registers, the word-select frame decode, the
 * RTS/TIE/RIE control bits and the IRQ resolution.
 */

#include "io/acia6850.h"

#include <string.h>

/* Decode the word-select field (control bits 2-4) into a frame length and the
 * data mask. ws: 0=7E2 1=7O2 2=7E1 3=7O1 4=8N2 5=8N1 6=8E1 7=8O1 */
static uint8_t acia6850_frame_from_ws(uint8_t ws, uint8_t* bitmask_out)
{
    uint8_t data   = (ws < 4) ? 7 : 8;
    uint8_t parity = (ws == 4 || ws == 5) ? 0 : 1;
    uint8_t stop   = (ws == 0 || ws == 1 || ws == 4) ? 2 : 1;
    if (bitmask_out) *bitmask_out = (data == 7) ? 0x7F : 0xFF;
    return (uint8_t)(1 + data + parity + stop);  /* + start bit */
}

/* Recompute the IRQ status bit and notify. IRQ comes from a full receive
 * register (RIE) or an empty transmit register with the Tx interrupt enabled
 * (transmit-control field = 01). */
static void acia6850_update_irq(acia6850_t* a)
{
    bool irq = false;
    if ((a->control & ACIA6850_CR_RIE) && (a->status & ACIA6850_SR_RDRF)) {
        irq = true;
    }
    uint8_t tc = (uint8_t)((a->control & ACIA6850_CR_TC_MASK) >> ACIA6850_CR_TC_SHIFT);
    if (tc == ACIA6850_TC_RTS_LOW_TIE_ON && (a->status & ACIA6850_SR_TDRE)) {
        irq = true;
    }

    if (irq) a->status |= ACIA6850_SR_IRQ;
    else     a->status &= (uint8_t)~ACIA6850_SR_IRQ;

    if (a->irq_out) a->irq_out(a->userdata, irq);
}

void acia6850_init(acia6850_t* a)
{
    void* ud = a->userdata;
    void (*irq)(void*, bool) = a->irq_out;
    memset(a, 0, sizeof(*a));
    a->userdata = ud;
    a->irq_out  = irq;
    acia6850_reset(a);
}

void acia6850_reset(acia6850_t* a)
{
    a->control = 0;
    /* Power-on: transmitter empty, line open → carrier absent / not CTS. */
    a->status  = ACIA6850_SR_TDRE | ACIA6850_SR_DCD | ACIA6850_SR_CTS;
    a->rdr = a->tdr = 0;
    a->bitmask   = 0x7F;   /* 7-bit until configured */
    a->framebits = 10;
    a->rts_low   = false;
}

bool acia6850_control_write(acia6850_t* a, uint8_t value)
{
    if ((value & ACIA6850_CR_CDS_MASK) == ACIA6850_CR_MASTER_RST) {
        /* Master reset: clear receiver, transmitter empty; the frame format and
         * the DCD/CTS input pins are left as they are. */
        a->control = value;
        a->status  = (uint8_t)((a->status & (ACIA6850_SR_DCD | ACIA6850_SR_CTS)) |
                               ACIA6850_SR_TDRE);
        a->rdr = 0;
        acia6850_update_irq(a);
        return true;
    }

    a->control = value;

    uint8_t ws = (uint8_t)((value & ACIA6850_CR_WS_MASK) >> ACIA6850_CR_WS_SHIFT);
    a->framebits = acia6850_frame_from_ws(ws, &a->bitmask);

    uint8_t tc = (uint8_t)((value & ACIA6850_CR_TC_MASK) >> ACIA6850_CR_TC_SHIFT);
    /* Carrier is emitted whenever /RTS is driven low — every code except the
     * "RTS high" one (10). */
    a->rts_low = (tc != ACIA6850_TC_RTS_HIGH_TIE_OFF);

    acia6850_update_irq(a);
    return false;
}

uint8_t acia6850_status(const acia6850_t* a)
{
    return a->status;
}

uint8_t acia6850_read_data(acia6850_t* a)
{
    uint8_t v = a->rdr;
    a->status &= (uint8_t)~ACIA6850_SR_RDRF;
    acia6850_update_irq(a);
    return v;
}

void acia6850_write_data(acia6850_t* a, uint8_t value)
{
    a->tdr = (uint8_t)(value & a->bitmask);
    a->status &= (uint8_t)~ACIA6850_SR_TDRE;  /* transmitter busy */
    acia6850_update_irq(a);
}

void acia6850_rx_byte(acia6850_t* a, uint8_t b)
{
    a->rdr = (uint8_t)(b & a->bitmask);
    a->status |= ACIA6850_SR_RDRF;
    acia6850_update_irq(a);
}

void acia6850_tx_complete(acia6850_t* a)
{
    a->status |= ACIA6850_SR_TDRE;
    acia6850_update_irq(a);
}

void acia6850_set_dcd(acia6850_t* a, bool carrier_present)
{
    if (carrier_present) a->status &= (uint8_t)~ACIA6850_SR_DCD;
    else                 a->status |= ACIA6850_SR_DCD;
}

void acia6850_set_cts(acia6850_t* a, bool clear_to_send)
{
    if (clear_to_send) a->status &= (uint8_t)~ACIA6850_SR_CTS;
    else               a->status |= ACIA6850_SR_CTS;
}
