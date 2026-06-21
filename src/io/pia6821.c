/**
 * @file pia6821.c
 * @brief Motorola MC6821 PIA — faithful model (see pia6821.h for the register map)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 */

#include "io/pia6821.h"

#include <string.h>

/* ───────────────────────────────────────────────────────────────────────
 *  Helpers
 * ─────────────────────────────────────────────────────────────────────── */

/* Resolve the IRQ output of one side from its control register. IRQ1 (CA1/CB1)
 * is enabled by bit0 and flagged in bit7; IRQ2 (CA2/CB2, only when CA2/CB2 is an
 * input) is enabled by bit3 and flagged in bit6. */
static bool pia_irq_from_cr(uint8_t cr)
{
    bool irq1 = (cr & PIA_CR_IRQ1_FLAG) && (cr & PIA_CR_IRQ1_ENABLE);
    bool irq2 = !(cr & PIA_CR_C2_OUTPUT) &&
                (cr & PIA_CR_IRQ2_FLAG) && (cr & PIA_CR_C2_LOW);
    return irq1 || irq2;
}

static void pia_update_irqa(pia6821_t* pia)
{
    bool irq = pia_irq_from_cr(pia->cra);
    if (irq != pia->irqa) {
        pia->irqa = irq;
        if (pia->irqa_out) pia->irqa_out(pia->userdata, irq);
    }
}

static void pia_update_irqb(pia6821_t* pia)
{
    bool irq = pia_irq_from_cr(pia->crb);
    if (irq != pia->irqb) {
        pia->irqb = irq;
        if (pia->irqb_out) pia->irqb_out(pia->userdata, irq);
    }
}

/* Drive the CA2 output line (output modes only); notify on change. */
static void pia_drive_ca2(pia6821_t* pia, bool level)
{
    if (level != pia->ca2) {
        pia->ca2 = level;
        if (pia->ca2_out) pia->ca2_out(pia->userdata, level);
    }
}

static void pia_drive_cb2(pia6821_t* pia, bool level)
{
    if (level != pia->cb2) {
        pia->cb2 = level;
        if (pia->cb2_out) pia->cb2_out(pia->userdata, level);
    }
}

/* True active edge for an interrupt input given the CR edge-select bit
 * (1 = active on rising / low→high, 0 = active on falling / high→low). */
static bool pia_active_edge(bool old_level, bool new_level, bool rising_active)
{
    if (rising_active) return (!old_level && new_level);
    return (old_level && !new_level);
}

/* ───────────────────────────────────────────────────────────────────────
 *  Lifecycle
 * ─────────────────────────────────────────────────────────────────────── */

void pia6821_init(pia6821_t* pia)
{
    void* ud = pia->userdata;
    void (*pa)(void*, uint8_t) = pia->port_a_out;
    void (*pb)(void*, uint8_t) = pia->port_b_out;
    void (*c2a)(void*, bool)   = pia->ca2_out;
    void (*c2b)(void*, bool)   = pia->cb2_out;
    void (*ia)(void*, bool)    = pia->irqa_out;
    void (*ib)(void*, bool)    = pia->irqb_out;

    memset(pia, 0, sizeof(*pia));

    pia->userdata   = ud;
    pia->port_a_out = pa;
    pia->port_b_out = pb;
    pia->ca2_out    = c2a;
    pia->cb2_out    = c2b;
    pia->irqa_out   = ia;
    pia->irqb_out   = ib;
}

void pia6821_reset(pia6821_t* pia)
{
    pia->ddra = pia->ora = pia->ira = pia->cra = 0;
    pia->ddrb = pia->orb = pia->irb = pia->crb = 0;
    pia->ca1 = pia->ca2 = pia->cb1 = pia->cb2 = false;
    pia->irqa = pia->irqb = false;
}

/* ───────────────────────────────────────────────────────────────────────
 *  Output-pin resolution
 * ─────────────────────────────────────────────────────────────────────── */

uint8_t pia6821_port_a_output(const pia6821_t* pia)
{
    return (uint8_t)(pia->ora & pia->ddra);
}

uint8_t pia6821_port_b_output(const pia6821_t* pia)
{
    return (uint8_t)(pia->orb & pia->ddrb);
}

/* ───────────────────────────────────────────────────────────────────────
 *  Control-register writes (handles CA2/CB2 output configuration)
 * ─────────────────────────────────────────────────────────────────────── */

static void pia_write_cra(pia6821_t* pia, uint8_t value)
{
    bool was_output = (pia->cra & PIA_CR_C2_OUTPUT) != 0;
    /* bits 6/7 are read-only interrupt flags; keep them. */
    pia->cra = (uint8_t)((value & 0x3F) | (pia->cra & 0xC0));

    if (pia->cra & PIA_CR_C2_OUTPUT) {
        if (pia->cra & PIA_CR_C2_HIGH) {
            /* Set/reset mode: CA2 follows bit3. */
            pia_drive_ca2(pia, (pia->cra & PIA_CR_C2_LOW) != 0);
        } else if (!was_output) {
            /* Entering handshake/pulse output mode: idle high. */
            pia_drive_ca2(pia, true);
        }
    }
    pia_update_irqa(pia);
}

static void pia_write_crb(pia6821_t* pia, uint8_t value)
{
    bool was_output = (pia->crb & PIA_CR_C2_OUTPUT) != 0;
    pia->crb = (uint8_t)((value & 0x3F) | (pia->crb & 0xC0));

    if (pia->crb & PIA_CR_C2_OUTPUT) {
        if (pia->crb & PIA_CR_C2_HIGH) {
            pia_drive_cb2(pia, (pia->crb & PIA_CR_C2_LOW) != 0);
        } else if (!was_output) {
            pia_drive_cb2(pia, true);
        }
    }
    pia_update_irqb(pia);
}

/* ───────────────────────────────────────────────────────────────────────
 *  CPU bus access
 * ─────────────────────────────────────────────────────────────────────── */

uint8_t pia6821_read(pia6821_t* pia, uint8_t rs)
{
    switch (rs & 3) {
        case PIA_RS_PRA:
            if (pia->cra & PIA_CR_DDR_SELECT) {
                uint8_t v = (uint8_t)((pia->ora & pia->ddra) |
                                      (pia->ira & ~pia->ddra));
                /* Reading PRA clears both IRQ flags for side A. */
                pia->cra &= (uint8_t)~(PIA_CR_IRQ1_FLAG | PIA_CR_IRQ2_FLAG);
                /* CA2 output handshake/pulse strobe on PRA read. */
                if ((pia->cra & PIA_CR_C2_OUTPUT) && !(pia->cra & PIA_CR_C2_HIGH)) {
                    if (pia->cra & PIA_CR_C2_LOW) {        /* pulse */
                        pia_drive_ca2(pia, false);
                        pia_drive_ca2(pia, true);
                    } else {                               /* handshake */
                        pia_drive_ca2(pia, false);
                    }
                }
                pia_update_irqa(pia);
                return v;
            }
            return pia->ddra;

        case PIA_RS_CRA:
            return pia->cra;

        case PIA_RS_PRB:
            if (pia->crb & PIA_CR_DDR_SELECT) {
                uint8_t v = (uint8_t)((pia->orb & pia->ddrb) |
                                      (pia->irb & ~pia->ddrb));
                pia->crb &= (uint8_t)~(PIA_CR_IRQ1_FLAG | PIA_CR_IRQ2_FLAG);
                pia_update_irqb(pia);
                return v;
            }
            return pia->ddrb;

        case PIA_RS_CRB:
            return pia->crb;
    }
    return 0xFF;
}

void pia6821_write(pia6821_t* pia, uint8_t rs, uint8_t value)
{
    switch (rs & 3) {
        case PIA_RS_PRA:
            if (pia->cra & PIA_CR_DDR_SELECT) {
                pia->ora = value;
            } else {
                pia->ddra = value;
            }
            if (pia->port_a_out) pia->port_a_out(pia->userdata,
                                                 pia6821_port_a_output(pia));
            break;

        case PIA_RS_CRA:
            pia_write_cra(pia, value);
            break;

        case PIA_RS_PRB:
            if (pia->crb & PIA_CR_DDR_SELECT) {
                pia->orb = value;
                /* CB2 output handshake/pulse strobe on PRB write. */
                if ((pia->crb & PIA_CR_C2_OUTPUT) && !(pia->crb & PIA_CR_C2_HIGH)) {
                    if (pia->crb & PIA_CR_C2_LOW) {        /* pulse */
                        pia_drive_cb2(pia, false);
                        pia_drive_cb2(pia, true);
                    } else {                               /* handshake */
                        pia_drive_cb2(pia, false);
                    }
                }
            } else {
                pia->ddrb = value;
            }
            if (pia->port_b_out) pia->port_b_out(pia->userdata,
                                                 pia6821_port_b_output(pia));
            break;

        case PIA_RS_CRB:
            pia_write_crb(pia, value);
            break;
    }
}

/* ───────────────────────────────────────────────────────────────────────
 *  Peripheral side
 * ─────────────────────────────────────────────────────────────────────── */

void pia6821_set_port_a_input(pia6821_t* pia, uint8_t pins) { pia->ira = pins; }
void pia6821_set_port_b_input(pia6821_t* pia, uint8_t pins) { pia->irb = pins; }

void pia6821_set_ca1(pia6821_t* pia, bool level)
{
    bool active = pia_active_edge(pia->ca1, level,
                                  (pia->cra & PIA_CR_IRQ1_EDGE) != 0);
    pia->ca1 = level;
    if (active) {
        pia->cra |= PIA_CR_IRQ1_FLAG;
        /* CA2 handshake output: an active CA1 edge releases CA2 back high. */
        if ((pia->cra & PIA_CR_C2_OUTPUT) &&
            !(pia->cra & PIA_CR_C2_HIGH) && !(pia->cra & PIA_CR_C2_LOW)) {
            pia_drive_ca2(pia, true);
        }
        pia_update_irqa(pia);
    }
}

void pia6821_set_cb1(pia6821_t* pia, bool level)
{
    bool active = pia_active_edge(pia->cb1, level,
                                  (pia->crb & PIA_CR_IRQ1_EDGE) != 0);
    pia->cb1 = level;
    if (active) {
        pia->crb |= PIA_CR_IRQ1_FLAG;
        if ((pia->crb & PIA_CR_C2_OUTPUT) &&
            !(pia->crb & PIA_CR_C2_HIGH) && !(pia->crb & PIA_CR_C2_LOW)) {
            pia_drive_cb2(pia, true);
        }
        pia_update_irqb(pia);
    }
}

void pia6821_set_ca2(pia6821_t* pia, bool level)
{
    if (pia->cra & PIA_CR_C2_OUTPUT) return;  /* CA2 is an output: ignore */
    bool active = pia_active_edge(pia->ca2, level,
                                  (pia->cra & PIA_CR_C2_HIGH) != 0);
    pia->ca2 = level;
    if (active) {
        pia->cra |= PIA_CR_IRQ2_FLAG;
        pia_update_irqa(pia);
    }
}

void pia6821_set_cb2(pia6821_t* pia, bool level)
{
    if (pia->crb & PIA_CR_C2_OUTPUT) return;
    bool active = pia_active_edge(pia->cb2, level,
                                  (pia->crb & PIA_CR_C2_HIGH) != 0);
    pia->cb2 = level;
    if (active) {
        pia->crb |= PIA_CR_IRQ2_FLAG;
        pia_update_irqb(pia);
    }
}
