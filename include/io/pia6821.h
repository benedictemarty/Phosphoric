/**
 * @file pia6821.h
 * @brief Motorola MC6821 PIA (Peripheral Interface Adapter) — faithful model
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-21
 *
 * A standalone, reusable MC6821 emulation: two 8-bit ports (A/B), each with a
 * Data Direction Register (DDR), an Output Register (OR) and a Control Register
 * (CR), plus the four peripheral control lines CA1/CA2/CB1/CB2.
 *
 * Distinct from the MOS 6522 VIA (src/io/via6522.c): the 6821 PIA has no timers
 * and no shift register — it is a pure parallel-port interface with two
 * interrupt inputs (CA1/CB1), and CA2/CB2 lines that can act as a second
 * interrupt input or as a handshake/pulse/set-reset output.
 *
 * Register map (RS1,RS0 = the two address lines), with CRA/CRB bit2 selecting
 * DDR vs OR for the data location:
 *   RS=0  PRA  (CRA bit2=1)  / DDRA (CRA bit2=0)
 *   RS=1  CRA
 *   RS=2  PRB  (CRB bit2=1)  / DDRB (CRB bit2=0)
 *   RS=3  CRB
 *
 * Control register layout (CRA/CRB):
 *   bit0  CA1/CB1 interrupt enable
 *   bit1  CA1/CB1 active edge (0 = high→low, 1 = low→high)
 *   bit2  DDR/OR access select (0 = DDR, 1 = OR / peripheral)
 *   bit3  CA2/CB2 control  (input: IRQ2 enable; output: mode/level — see bit5/4)
 *   bit4  CA2/CB2 control  (input: IRQ2 edge;   output: mode)
 *   bit5  CA2/CB2 direction (0 = input, 1 = output)
 *   bit6  IRQ2 flag (CA2/CB2) — read-only, set on active edge, cleared by PR read
 *   bit7  IRQ1 flag (CA1/CB1) — read-only, set on active edge, cleared by PR read
 *
 * CA2/CB2 output modes (bit5 = 1):
 *   bit4=1            : set/reset — the line follows bit3
 *   bit4=0, bit3=1    : pulse     — one-cycle strobe after PRA read / PRB write
 *   bit4=0, bit3=0    : handshake — line goes low after PRA read / PRB write,
 *                                   back high on the next active CA1/CB1 edge
 */

#ifndef PIA6821_H
#define PIA6821_H

#include <stdint.h>
#include <stdbool.h>

/* Control-register bit masks (apply to CRA and CRB alike) */
#define PIA_CR_IRQ1_ENABLE   0x01  /* bit0 */
#define PIA_CR_IRQ1_EDGE     0x02  /* bit1: 1 = rising active */
#define PIA_CR_DDR_SELECT    0x04  /* bit2: 1 = OR, 0 = DDR */
#define PIA_CR_C2_LOW        0x08  /* bit3 */
#define PIA_CR_C2_HIGH       0x10  /* bit4 */
#define PIA_CR_C2_OUTPUT     0x20  /* bit5: 1 = Cx2 is an output */
#define PIA_CR_IRQ2_FLAG     0x40  /* bit6 (read-only) */
#define PIA_CR_IRQ1_FLAG     0x80  /* bit7 (read-only) */

/* Register-select values (RS1,RS0) */
#define PIA_RS_PRA   0
#define PIA_RS_CRA   1
#define PIA_RS_PRB   2
#define PIA_RS_CRB   3

typedef struct pia6821_s {
    /* Port A */
    uint8_t ddra;       /**< Data direction A (1 = output) */
    uint8_t ora;        /**< Output register A */
    uint8_t ira;        /**< Peripheral input lines A */
    uint8_t cra;        /**< Control register A */
    bool    ca1;        /**< Current CA1 input level */
    bool    ca2;        /**< CA2 level (input level, or driven output state) */

    /* Port B */
    uint8_t ddrb;       /**< Data direction B (1 = output) */
    uint8_t orb;        /**< Output register B */
    uint8_t irb;        /**< Peripheral input lines B */
    uint8_t crb;        /**< Control register B */
    bool    cb1;        /**< Current CB1 input level */
    bool    cb2;        /**< CB2 level (input level, or driven output state) */

    /* Resolved interrupt outputs (active high here; the bus is /IRQ) */
    bool    irqa;
    bool    irqb;

    /* Optional callbacks (NULL = ignored). userdata is passed through. */
    void  (*port_a_out)(void* ud, uint8_t data);   /**< ORA or DDRA changed: new A output pins */
    void  (*port_b_out)(void* ud, uint8_t data);   /**< ORB or DDRB changed: new B output pins */
    void  (*ca2_out)(void* ud, bool level);        /**< CA2 driven (output modes) */
    void  (*cb2_out)(void* ud, bool level);        /**< CB2 driven (output modes) */
    void  (*irqa_out)(void* ud, bool active);      /**< IRQA line changed */
    void  (*irqb_out)(void* ud, bool active);      /**< IRQB line changed */
    void*   userdata;
} pia6821_t;

/* ── Lifecycle ── */
void    pia6821_init(pia6821_t* pia);
void    pia6821_reset(pia6821_t* pia);

/* ── CPU bus access (rs = 0..3) ── */
uint8_t pia6821_read(pia6821_t* pia, uint8_t rs);
void    pia6821_write(pia6821_t* pia, uint8_t rs, uint8_t value);

/* ── Peripheral side: drive input pins / control lines ── */
void    pia6821_set_port_a_input(pia6821_t* pia, uint8_t pins);
void    pia6821_set_port_b_input(pia6821_t* pia, uint8_t pins);
void    pia6821_set_ca1(pia6821_t* pia, bool level);
void    pia6821_set_cb1(pia6821_t* pia, bool level);
void    pia6821_set_ca2(pia6821_t* pia, bool level);  /* ignored when CA2 is output */
void    pia6821_set_cb2(pia6821_t* pia, bool level);  /* ignored when CB2 is output */

/* ── Resolved port output pins (OR masked by DDR; input bits read as 0) ── */
uint8_t pia6821_port_a_output(const pia6821_t* pia);
uint8_t pia6821_port_b_output(const pia6821_t* pia);

/* ── Interrupt outputs (true = /IRQ asserted) ── */
static inline bool pia6821_irqa(const pia6821_t* pia) { return pia->irqa; }
static inline bool pia6821_irqb(const pia6821_t* pia) { return pia->irqb; }

#endif /* PIA6821_H */
