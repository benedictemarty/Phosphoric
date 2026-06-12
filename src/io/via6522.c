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

void via_init(via6522_t* via) {
    memset(via, 0, sizeof(via6522_t));
    via->cb1_pin = true;   /* CB1 idle high (not driven on Oric) */
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
    via->irq_line = false;  /* /IRQ not asserted */
}

uint8_t via_read(via6522_t* via, uint8_t reg) {
    reg &= 0x0F;
    switch (reg) {
    case VIA_ORB: {
        /* Read Port B: combine input and output based on DDR */
        uint8_t input = 0xFF;
        if (via->portb_read) input = via->portb_read(via->userdata);
        via->ifr &= ~VIA_INT_CB1;
        /* CB2 handshake clear if PCR configured */
        {
            uint8_t cb2_mode = via->pcr & 0xE0;
            if (cb2_mode != 0x20 && cb2_mode != 0x60)
                via->ifr &= ~VIA_INT_CB2;
        }
        via_check_irq(via);
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
        uint8_t pins = via->porta_read ? via->porta_read(via->userdata) : 0xFF;
        via->ifr &= ~VIA_INT_CA1;
        {
            uint8_t ca2_mode = via->pcr & 0x0E;
            if (ca2_mode != 0x02 && ca2_mode != 0x06)
                via->ifr &= ~VIA_INT_CA2;
        }
        via_check_irq(via);
        return (via->ora & via->ddra) | ((via->ira & pins) & ~via->ddra);
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
        return via->sr;
    case VIA_ACR: return via->acr;
    case VIA_PCR: return via->pcr;
    case VIA_IFR: return via->ifr;
    case VIA_IER: return via->ier | VIA_INT_ANY;
    case VIA_ORA_NH: {
        /* No-handshake variant: same wired-AND as VIA_ORA, without
         * touching the CA1/CA2 interrupt flags. */
        uint8_t pins = via->porta_read ? via->porta_read(via->userdata) : 0xFF;
        return (via->ora & via->ddra) | ((via->ira & pins) & ~via->ddra);
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
        break;
    case VIA_ACR: via->acr = value; break;
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

    /* Timer 2 (one-shot only in timer mode) */
    if (via->t2_running && !(via->acr & 0x20)) {
        int old = via->t2_counter;
        via->t2_counter -= (uint16_t)cycles;
        if (via->t2_counter > (uint16_t)old || via->t2_counter == 0xFFFF || via->t2_counter == 0) {
            via->ifr |= VIA_INT_T2;
            via_check_irq(via);
            via->t2_running = false;
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

    if (rising_edge && !old && state) {
        /* Rising edge detected and PCR selects rising */
        via->ifr |= VIA_INT_CB1;
        via_check_irq(via);
    } else if (!rising_edge && old && !state) {
        /* Falling edge detected and PCR selects falling */
        via->ifr |= VIA_INT_CB1;
        via_check_irq(via);
    }
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
