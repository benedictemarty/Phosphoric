/**
 * @file via6522.c
 * @brief MOS 6522 VIA - complete implementation with timers and interrupts
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.3.0-alpha
 */

#include "io/via6522.h"
#include <string.h>

static void via_check_irq(via6522_t* via) {
    bool irq = (via->ifr & via->ier & VIA_IER_MASK) != 0;
    if (irq) via->ifr |= VIA_INT_ANY;
    else via->ifr &= ~VIA_INT_ANY;

    /* Only notify CPU on /IRQ line transitions (like real hardware wire-OR).
     * Avoids spurious cpu_irq_clear when unrelated IFR bits change. */
    if (irq != via->irq_line) {
        via->irq_line = irq;
        if (via->irq_callback) {
            via->irq_callback(irq, via->irq_userdata);
        }
    }
}

/* ── Shift register ───────────────────────────────────────────────────
 * ACR bits 4-2 select the mode:
 *   000 disabled            100 shift OUT free-running at T2 rate
 *   001 shift IN under T2   101 shift OUT under T2
 *   010 shift IN under φ2   110 shift OUT under φ2
 *   011 shift IN under CB1  111 shift OUT under CB1 (external clock)
 * After 8 shifts the SR interrupt flag is set and shifting stops, except the
 * free-running output mode (100) which runs continuously and sets no flag. */
static void via_do_shift(via6522_t* via) {
    uint8_t mode = via->acr & 0x1C;
    bool shift_out = (mode & 0x10) != 0;
    if (shift_out) {
        bool msb = (via->sr & 0x80) != 0;
        via->cb2_pin = msb;
        via->sr = (uint8_t)((via->sr << 1) | (msb ? 1 : 0)); /* rotate (free-run repeats) */
    } else {
        via->sr = (uint8_t)((via->sr << 1) | (via->cb2_in ? 1 : 0));
    }
    via->sr_count++;
    if (via->sr_count >= 8) {
        via->sr_count = 0;
        if (mode != 0x10) {            /* free-running output: no flag, keep going */
            via->sr_active = false;
            via->ifr |= VIA_INT_SR;
            via_check_irq(via);
        }
    }
}

/* Port access side effects on CA2 (read/write ORA) and CB2 (write ORB):
 * handshake output (100) drives the pin low until the CA1/CB1 active edge;
 * pulse output (101) drives it low for one φ2 cycle. */
static void via_ca2_port_access(via6522_t* via) {
    uint8_t mode = via->pcr & 0x0E;
    if (mode == 0x08) {                   /* 100: handshake output */
        via->ca2_pin = false;
    } else if (mode == 0x0A) {            /* 101: pulse output */
        via->ca2_pin = false;
        via->ca2_pulse = 1;
    }
}

static void via_cb2_port_access(via6522_t* via) {
    uint8_t mode = via->pcr & 0xE0;
    if (mode == 0x80) {                   /* 100: handshake output */
        via->cb2_pin = false;
    } else if (mode == 0xA0) {            /* 101: pulse output */
        via->cb2_pin = false;
        via->cb2_pulse = 1;
    }
}

/* Live Port A input pins: wired-AND of IRA (PSG bus) and external drivers */
static uint8_t via_pa_pins(via6522_t* via) {
    uint8_t pins = via->porta_read ? via->porta_read(via->userdata) : 0xFF;
    return (uint8_t)(via->ira & pins);
}

void via_init(via6522_t* via) {
    memset(via, 0, sizeof(via6522_t));
    via->cb1_pin = true;   /* CB1 idle high (not driven on Oric) */
    via->ca1_pin = true;   /* CA1 idle high (printer ACK line, pulled up) */
    via->irq_line = false;  /* /IRQ not asserted */
}

void via_reset(via6522_t* via) {
    via->ora = via->orb = 0;
    via->ira = via->irb = 0xFF;
    via->ddra = via->ddrb = 0;
    via->t1_counter = 0xFFFF;
    via->t1_latch = 0xFFFF;
    via->t2_counter = 0xFFFF;
    via->t2_latch = 0xFF;
    via->t1_running = false;
    via->t2_running = false;
    via->sr = 0;
    via->sr_count = 0;
    via->acr = 0;
    via->pcr = 0;
    via->ifr = 0;
    via->ier = 0;
    via->cb1_pin = true;   /* CB1 idle high (not driven on Oric) */
    via->ca1_pin = true;   /* CA1 idle high */
    via->irq_line = false;  /* /IRQ not asserted */
    via->cb2_pin = false;
    via->cb2_in = false;
    via->ca2_pin = false;
    via->ca2_in = false;
    via->ca2_pulse = 0;
    via->cb2_pulse = 0;
    via->pa_latched = false;
    via->pb_latched = false;
    via->sr_active = false;
    via->sr_clk_acc = 0;
    via->pb6_pin = true;   /* PB6 idle high */
}

uint8_t via_read(via6522_t* via, uint8_t reg) {
    reg &= 0x0F;
    switch (reg) {
    case VIA_ORB: {
        /* Read Port B: combine input and output based on DDR.
         * With latching enabled (ACR bit 1) the input byte is the one
         * captured on the last CB1 active edge, not the live pins. */
        uint8_t input = 0xFF;
        if ((via->acr & 0x02) && via->pb_latched) {
            input = via->pb_latch;
        } else if (via->portb_read) {
            input = via->portb_read(via->userdata);
        }
        via->ifr &= ~VIA_INT_CB1;
        /* CB2 flag cleared unless CB2 is in an independent-interrupt mode */
        {
            uint8_t cb2_mode = via->pcr & 0xE0;
            if (cb2_mode != 0x20 && cb2_mode != 0x60)
                via->ifr &= ~VIA_INT_CB2;
        }
        via_check_irq(via);
        /* NOTE: on the 6522, reading ORB does NOT trigger the CB2
         * handshake/pulse — only writing ORB does (write handshake). */
        return (via->orb & via->ddrb) | (input & ~via->ddrb);
    }
    case VIA_ORA: {
        /* PSG drives IRA only when in READ mode (psg_decode updates ira).
         * Using ira instead of polling a callback matches hardware: PSG
         * puts data on the bus only during its READ bus cycle, not on
         * every Port A read. IRA is initialised to 0xFF (no keys pressed).
         *
         * porta_read (optional) models EXTERNAL devices on the printer
         * port (e.g. IJK joystick interface) pulling lines low: pulled-up
         * bus, every driver can only pull down → wired-AND of IRA (PSG)
         * and the external pin state. */
        uint8_t input = ((via->acr & 0x01) && via->pa_latched)
                      ? via->pa_latch : via_pa_pins(via);
        via->ifr &= ~VIA_INT_CA1;
        {
            uint8_t ca2_mode = via->pcr & 0x0E;
            if (ca2_mode != 0x02 && ca2_mode != 0x06)
                via->ifr &= ~VIA_INT_CA2;
        }
        via_check_irq(via);
        via_ca2_port_access(via);   /* CA2 handshake/pulse on ORA read */
        return (via->ora & via->ddra) | (input & ~via->ddra);
    }
    case VIA_DDRB: return via->ddrb;
    case VIA_DDRA: return via->ddra;
    case VIA_T1CL:
        via->ifr &= ~VIA_INT_T1;
        via_check_irq(via);
        return (uint8_t)(via->t1_counter & 0xFF);
    case VIA_T1CH:
        return (uint8_t)(via->t1_counter >> 8);
    case VIA_T1LL:
        return (uint8_t)(via->t1_latch & 0xFF);
    case VIA_T1LH:
        return (uint8_t)(via->t1_latch >> 8);
    case VIA_T2CL:
        via->ifr &= ~VIA_INT_T2;
        via_check_irq(via);
        return (uint8_t)(via->t2_counter & 0xFF);
    case VIA_T2CH:
        return (uint8_t)(via->t2_counter >> 8);
    case VIA_SR:
        via->ifr &= ~VIA_INT_SR;
        via_check_irq(via);
        /* Reading SR starts a new shift-in sequence (input modes only). */
        {
            uint8_t mode = via->acr & 0x1C;
            if (mode != 0 && !(mode & 0x10)) {
                via->sr_active = true;
                via->sr_count = 0;
                via->sr_clk_acc = 0;
            }
        }
        return via->sr;
    case VIA_ACR: return via->acr;
    case VIA_PCR: return via->pcr;
    case VIA_IFR: return via->ifr;
    case VIA_IER: return via->ier | VIA_INT_ANY;
    case VIA_ORA_NH: {
        /* No-handshake variant: same read as VIA_ORA (latching included),
         * without touching the CA1/CA2 flags or the CA2 handshake/pulse. */
        uint8_t input = ((via->acr & 0x01) && via->pa_latched)
                      ? via->pa_latch : via_pa_pins(via);
        return (via->ora & via->ddra) | (input & ~via->ddra);
    }
    }
    return 0xFF;
}

void via_write(via6522_t* via, uint8_t reg, uint8_t value) {
    reg &= 0x0F;
    switch (reg) {
    case VIA_ORB:
        via->orb = value;
        via->ifr &= ~VIA_INT_CB1;
        {
            uint8_t cb2w = via->pcr & 0xE0;
            if (cb2w != 0x20 && cb2w != 0x60)
                via->ifr &= ~VIA_INT_CB2;
        }
        via_check_irq(via);
        via_cb2_port_access(via);   /* CB2 handshake/pulse (write only) */
        if (via->portb_write) via->portb_write(value, via->userdata);
        break;
    case VIA_ORA:
        via->ora = value;
        via->ifr &= ~VIA_INT_CA1;
        {
            uint8_t ca2_mode = via->pcr & 0x0E;
            if (ca2_mode != 0x02 && ca2_mode != 0x06)
                via->ifr &= ~VIA_INT_CA2;
        }
        via_check_irq(via);
        via_ca2_port_access(via);   /* CA2 handshake/pulse on ORA write */
        if (via->porta_write) via->porta_write(value, via->userdata);
        break;
    case VIA_DDRB: via->ddrb = value; break;
    case VIA_DDRA: via->ddra = value; break;
    case VIA_T1CL:
    case VIA_T1LL:
        via->t1_latch = (via->t1_latch & 0xFF00) | value;
        break;
    case VIA_T1CH:
        via->t1_latch = (via->t1_latch & 0x00FF) | ((uint16_t)value << 8);
        via->t1_counter = via->t1_latch;
        via->t1_running = true;
        via->ifr &= ~VIA_INT_T1;
        via_check_irq(via);
        break;
    case VIA_T1LH:
        via->t1_latch = (via->t1_latch & 0x00FF) | ((uint16_t)value << 8);
        via->ifr &= ~VIA_INT_T1;
        via_check_irq(via);
        break;
    case VIA_T2CL:
        via->t2_latch = value;
        break;
    case VIA_T2CH:
        via->t2_counter = ((uint16_t)value << 8) | via->t2_latch;
        via->t2_running = true;
        via->ifr &= ~VIA_INT_T2;
        via_check_irq(via);
        break;
    case VIA_SR:
        via->sr = value;
        via->ifr &= ~VIA_INT_SR;
        via_check_irq(via);
        /* Writing SR starts a new shift-out sequence (output modes only). */
        {
            uint8_t mode = via->acr & 0x1C;
            if (mode & 0x10) {
                via->sr_active = true;
                via->sr_count = 0;
                via->sr_clk_acc = 0;
            }
        }
        break;
    case VIA_ACR:
        via->acr = value;
        /* Free-running shift-out (100) runs continuously; selecting the
         * disabled mode (000) halts any sequence. */
        if ((value & 0x1C) == 0x10) {
            via->sr_active = true;
            via->sr_count = 0;
            via->sr_clk_acc = 0;
        } else if ((value & 0x1C) == 0x00) {
            via->sr_active = false;
        }
        break;
    case VIA_PCR: via->pcr = value; break;
    case VIA_IFR:
        via->ifr &= ~(value & VIA_IER_MASK);
        via_check_irq(via);
        break;
    case VIA_IER:
        if (value & VIA_INT_ANY) via->ier |= (value & VIA_IER_MASK);
        else via->ier &= ~(value & VIA_IER_MASK);
        via_check_irq(via);
        break;
    case VIA_ORA_NH:
        via->ora = value;
        if (via->porta_write) via->porta_write(value, via->userdata);
        break;
    }
}

void via_update(via6522_t* via, int cycles) {
    /* CA2/CB2 pulse output (PCR mode 101): restore high after one cycle */
    if (via->ca2_pulse > 0) {
        via->ca2_pulse -= cycles;
        if (via->ca2_pulse <= 0) {
            via->ca2_pulse = 0;
            via->ca2_pin = true;
        }
    }
    if (via->cb2_pulse > 0) {
        via->cb2_pulse -= cycles;
        if (via->cb2_pulse <= 0) {
            via->cb2_pulse = 0;
            via->cb2_pin = true;
        }
    }

    /* Timer 1 */
    if (via->t1_running) {
        int old = via->t1_counter;
        via->t1_counter -= (uint16_t)cycles;
        if (via->t1_counter > (uint16_t)old || via->t1_counter == 0xFFFF || via->t1_counter == 0) {
            /* Timer 1 underflow */
            via->ifr |= VIA_INT_T1;
            via_check_irq(via);

            if (via->acr & 0x40) {
                /* Free-running: reload from latch */
                via->t1_counter = via->t1_latch;
            } else {
                /* One-shot: stop */
                via->t1_running = false;
            }
        }
    }

    /* Timer 2 (one-shot only in timer mode; pulse-counting mode is driven by
     * via_pb6_pulse() instead of φ2). */
    if (via->t2_running && !(via->acr & 0x20)) {
        int old = via->t2_counter;
        via->t2_counter -= (uint16_t)cycles;
        if (via->t2_counter > (uint16_t)old || via->t2_counter == 0xFFFF || via->t2_counter == 0) {
            via->ifr |= VIA_INT_T2;
            via_check_irq(via);
            via->t2_running = false;
        }
    }

    /* Shift register: internal (φ2 / T2) clock modes. The external-clock modes
     * (011 / 111) are driven by via_shift_clock() instead. */
    {
        uint8_t srmode = via->acr & 0x1C;
        bool internal = (srmode != 0) && (srmode != 0x0C) && (srmode != 0x1C);
        if (internal && (via->sr_active || srmode == 0x10)) {
            /* φ2 modes (010/110): one shift every 2 cycles (CB1 clock = φ2/2).
             * T2 modes: one shift every (T2 low latch + 2) cycles. */
            uint32_t div = (srmode == 0x08 || srmode == 0x18)
                         ? 2u : ((uint32_t)via->t2_latch + 2u);
            if (div == 0) div = 1;
            via->sr_clk_acc += (uint32_t)cycles;
            while (via->sr_clk_acc >= div && (via->sr_active || srmode == 0x10)) {
                via->sr_clk_acc -= div;
                via_do_shift(via);
            }
        }
    }
}

void via_set_port_callbacks(via6522_t* via,
                            uint8_t (*porta_read)(void*),
                            void (*porta_write)(uint8_t, void*),
                            uint8_t (*portb_read)(void*),
                            void (*portb_write)(uint8_t, void*),
                            void* userdata) {
    via->porta_read = porta_read;
    via->porta_write = porta_write;
    via->portb_read = portb_read;
    via->portb_write = portb_write;
    via->userdata = userdata;
}

void via_set_irq_callback(via6522_t* via,
                         void (*callback)(bool, void*),
                         void* userdata) {
    via->irq_callback = callback;
    via->irq_userdata = userdata;
}

void via_trigger_ca1(via6522_t* via) {
    via->ifr |= VIA_INT_CA1;
    via_check_irq(via);
}

void via_set_ca1(via6522_t* via, bool state) {
    bool old = via->ca1_pin;
    via->ca1_pin = state;
    if (old == state) return;

    /* PCR bit 0: 0 = interrupt on falling edge, 1 = rising edge */
    bool rising_edge = (via->pcr & 0x01) != 0;
    bool active = (rising_edge && !old && state) ||
                  (!rising_edge && old && !state);
    if (!active) return;

    via->ifr |= VIA_INT_CA1;
    /* Input latching (ACR bit 0): capture Port A pins on the active edge */
    if (via->acr & 0x01) {
        via->pa_latch = via_pa_pins(via);
        via->pa_latched = true;
    }
    /* Handshake output (PCR CA2 = 100): "data ready" edge restores CA2 */
    if ((via->pcr & 0x0E) == 0x08)
        via->ca2_pin = true;
    via_check_irq(via);
}

void via_set_ca2_input(via6522_t* via, bool level) {
    bool old = via->ca2_in;
    via->ca2_in = level;
    if ((via->pcr & 0x08) != 0) return;   /* output modes: pin is driven */
    if (old == level) return;

    /* PCR bit 2: 0 = falling edge, 1 = rising edge (input modes 000-011) */
    bool rising_edge = (via->pcr & 0x04) != 0;
    if ((rising_edge && !old && level) || (!rising_edge && old && !level)) {
        via->ifr |= VIA_INT_CA2;
        via_check_irq(via);
    }
}

void via_trigger_ca2(via6522_t* via) {
    via->ifr |= VIA_INT_CA2;
    via_check_irq(via);
}

void via_set_cb1(via6522_t* via, bool state) {
    bool old = via->cb1_pin;
    via->cb1_pin = state;

    /* No transition = no interrupt */
    if (old == state) return;

    /* PCR bit 4: 0 = interrupt on falling edge, 1 = interrupt on rising edge */
    bool rising_edge = (via->pcr & 0x10) != 0;
    bool active = (rising_edge && !old && state) ||
                  (!rising_edge && old && !state);
    if (!active) return;

    via->ifr |= VIA_INT_CB1;
    /* Input latching (ACR bit 1): capture Port B pins on the active edge */
    if (via->acr & 0x02) {
        via->pb_latch = via->portb_read ? via->portb_read(via->userdata) : 0xFF;
        via->pb_latched = true;
    }
    /* Handshake output (PCR CB2 = 100): "data taken" edge restores CB2 */
    if ((via->pcr & 0xE0) == 0x80)
        via->cb2_pin = true;
    via_check_irq(via);
}

void via_trigger_cb1(via6522_t* via) {
    /* Legacy pulse: high→low→high (always triggers regardless of PCR) */
    via_set_cb1(via, false);
    via_set_cb1(via, true);
}

void via_trigger_cb2(via6522_t* via) {
    via->ifr |= VIA_INT_CB2;
    via_check_irq(via);
}

void via_shift_clock(via6522_t* via) {
    uint8_t mode = via->acr & 0x1C;
    if (mode != 0x0C && mode != 0x1C) return; /* external-clock modes only */
    if (!via->sr_active) return;
    via_do_shift(via);
}

void via_set_cb2_input(via6522_t* via, bool level) {
    bool old = via->cb2_in;
    via->cb2_in = level;
    if ((via->pcr & 0x80) != 0) return;   /* output modes: pin is driven */
    if (old == level) return;

    /* PCR bit 6: 0 = falling edge, 1 = rising edge (input modes 000-011) */
    bool rising_edge = (via->pcr & 0x40) != 0;
    if ((rising_edge && !old && level) || (!rising_edge && old && !level)) {
        via->ifr |= VIA_INT_CB2;
        via_check_irq(via);
    }
}

void via_pb6_pulse(via6522_t* via) {
    /* Each call models one PB6 negative edge. */
    if (!via->t2_running || !(via->acr & 0x20)) return;
    if (via->t2_counter == 0) {
        via->t2_counter = 0xFFFF;         /* underflow */
        via->ifr |= VIA_INT_T2;
        via_check_irq(via);
        via->t2_running = false;
    } else {
        via->t2_counter--;
    }
}

bool via_get_ca2(via6522_t* via) {
    uint8_t ca2 = via->pcr & 0x0E;
    if ((ca2 & 0x08) == 0) return via->ca2_in;  /* 000-011: input pin */
    if (ca2 == 0x0C) return false;        /* 110: manual output low */
    if (ca2 == 0x0E) return true;         /* 111: manual output high */
    return via->ca2_pin;                  /* 100/101: handshake/pulse level */
}

bool via_get_cb2(via6522_t* via) {
    if (via->acr & 0x10) return via->cb2_pin; /* shift-out drives CB2 */
    uint8_t cb2 = via->pcr & 0xE0;
    if ((cb2 & 0x80) == 0) return via->cb2_in;  /* 000-011: input pin */
    if (cb2 == 0xC0) return false;        /* 110: manual output low */
    if (cb2 == 0xE0) return true;         /* 111: manual output high */
    return via->cb2_pin;                  /* 100/101: handshake/pulse level */
}
