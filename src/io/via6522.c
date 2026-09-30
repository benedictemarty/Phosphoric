/* SPDX-License-Identifier: EUPL-1.2 */
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

/* Neo6502Vic20 : registre à décalage cadencé en interne (T2 ou φ2), sur le
 * modèle de VICE viacore.c (référence de comportement, aucun code copié) et
 * validé par testprogs/VIC20/via_sr du vrai VIC-20. Une séquence compte 16
 * demi-périodes de CB1 (sr_count 0..15) ; en sortie le décalage a lieu aux
 * états pairs, en entrée aux états impairs ; au 16e état, drapeau SR (sauf
 * sortie libre, mode 100, qui reboucle sans drapeau). */
static void via_sr_event(via6522_t* via) {
    uint8_t mode = via->acr & 0x1C;
    if (mode == 0 || !via->sr_active) return;
    bool out = (mode & 0x10) != 0;
    if ((via->sr_count & 1) == 0) {
        if (out) {
            bool msb = (via->sr & 0x80) != 0;
            via->cb2_pin = msb;
            via->sr = (uint8_t)((via->sr << 1) | (msb ? 1 : 0));
        }
    } else if (!out) {
        via->sr = (uint8_t)((via->sr << 1) | (via->cb2_in ? 1 : 0));
    }
    via->sr_count++;
    if (via->sr_count >= 16) {
        via->sr_count = 0;
        if (mode != 0x10) {
            via->sr_active = false;
            via->ifr |= VIA_INT_SR;
            via_check_irq(via);
        }
    }
}

/* Accès (lecture ou écriture) au registre : démarre une séquence si aucune
 * n'est en cours, quel que soit le sens ; en φ2, premier événement au cycle
 * suivant. */
static void via_sr_access(via6522_t* via) {
    uint8_t mode = via->acr & 0x1C;
    if (mode == 0) return;               /* désactivé : ne démarre rien */
    if (mode == 0x10) {                  /* sortie libre : parité conservée */
        via->sr_active = true;
        via->sr_count &= 0x0F;
        return;
    }
    if (!via->sr_active) {
        via->sr_active = true;
        via->sr_count = 0;
        if (mode == 0x08 || mode == 0x18) via->sr_delay = 1;
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
    via->t1_active = false;
    via->t2_active = false;
    via->sr = 0;
    via->sr_count = 0;
    via->acr = 0;
    via->t2_phi2 = true;   /* Neo6502Vic20 : ACR à 0, T2 compte φ2 */
    via->t2_hold = false;
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
    via->sr_count = 0;
    via->sr_delay = 0;
    via->sr_t2_pending = 0;
    via->pb6_pin = true;   /* PB6 idle high */
    via->pb7_pin = true;   /* Neo6502Vic20 : bascule PB7 à 1 au RESET (VICE t1_pb7 = 0x80 ;
                            * référence viavarious via10-13 du vrai VIC-20) */
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
        {
            uint8_t rb = (via->orb & via->ddrb) | (input & ~via->ddrb);
            /* Neo6502Vic20 : PB7 est la sortie de T1 dès que ACR bit7 = 1,
             * quel que soit DDRB bit7 (référence viavarious via10-13 du vrai
             * VIC-20 : PB7 piloté avec DDRB = 0). */
            if (via->acr & 0x80)
                rb = via->pb7_pin ? (rb | 0x80) : (rb & 0x7F);
            return rb;
        }
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
        via_sr_access(via);  /* Neo6502Vic20 : lecture ou écriture, tous sens */
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
        via->t1_active = true;
        /* Neo6502Vic20 : le compteur chargé ne décompte qu'au cycle suivant
         * l'écriture (référence viavarious du vrai VIC-20 : lectures en
         * avance d'un cycle sinon) ; le cycle de rechargement l'assure. */
        via->t1_reload = true;
        via->ifr &= ~VIA_INT_T1;
        /* ACR bit7 one-shot PB7 mode (bit6=0): writing T1CH pulls PB7 low for
         * the duration of the count; the underflow drives it high again. PB7 is
         * the timer output only when BOTH DDRB bit7 and ACR bit7 are set
         * (datasheet p.9); otherwise it is a normal port pin. */
        /* Neo6502Vic20 : la bascule PB7 passe à 0 à chaque écriture de T1C-H
         * (VICE viacore.c ; référence viavarious via10-13 du vrai VIC-20) */
        via->pb7_pin = false;
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
        via->t2_active = true;
        via->t2_hold = true;      /* Neo6502Vic20 : premier décompte au cycle suivant */
        via->t2_reload = false;
        via->ifr &= ~VIA_INT_T2;
        via_check_irq(via);
        break;
    case VIA_SR:
        via->sr = value;
        via->ifr &= ~VIA_INT_SR;
        via_check_irq(via);
        via_sr_access(via);  /* Neo6502Vic20 : lecture ou écriture, tous sens */
        break;
    case VIA_ACR:
        /* Neo6502Vic20 : ACR bit7 passant à 1 met la bascule PB7 à 1 (VICE
         * viacore.c ; référence viavarious via10-13 du vrai VIC-20) */
        if (!(via->acr & 0x80) && (value & 0x80))
            via->pb7_pin = true;
        {
            uint8_t old = via->acr & 0x1C, mode = value & 0x1C;
            via->acr = value;
            if (mode == 0x10) {
                via->sr_active = true;           /* sortie libre : tourne sans accès */
            } else if (mode == 0x00) {
                /* Désactivé : drapeau SR tenu à 0 (VICE) ; la séquence n'est
                 * pas arrêtée */
                via->ifr &= ~VIA_INT_SR;
                via_check_irq(via);
            }
            /* φ2 : premier événement 3 cycles après l'écriture (VICE) */
            if ((mode == 0x08 || mode == 0x18) && old != 0x08 && old != 0x18)
                via->sr_delay = 3;
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

    /* ─── Timer 1 (V2-E3 : décompte cycle par cycle) ───
     *
     * Mécanique du 6522, et elle compte : le compteur décrémente à chaque φ2, et
     * le sous-dépassement n'est PAS le passage à zéro — c'est le passage de
     * $0000 à $FFFF, un cycle plus tard. En mode continu, le rechargement depuis
     * le latch consomme encore un cycle. D'où la période **N+2** de la
     * datasheet : N décomptes, un cycle de sous-dépassement, un cycle de
     * rechargement. L'ancienne implémentation tirait dès l'atteinte de zéro et
     * rechargeait dans le même cycle : période N, soit **2 cycles trop court par
     * période** (0,02 % d'erreur à 100 Hz, mais 20 % pour N=10 — audible sur les
     * sons et les digidrums).
     *
     * Neo6502Vic20 : le compteur se recharge depuis le latch à chaque
     * sous-dépassement, en one-shot comme en continu ; seule l'interruption
     * (et PB7) est unique en one-shot (t1_running faux). Mesuré sur un vrai
     * VIC-20 : référence de testprogs/VIC20/viavarious/via1 (lecture de T1
     * après time-out) ; VICE viacore.c calcule T1 modulo (latch + 2) dans
     * les deux modes. */
    if (via->t1_active) {
        for (int i = 0; i < cycles; i++) {
            if (via->t1_reload) {
                /* Cycle de rechargement : le compteur ne décompte pas. */
                via->t1_counter = via->t1_latch;
                via->t1_reload = false;
                continue;
            }
            bool underflow = (via->t1_counter == 0x0000);
            via->t1_counter--;               /* $0000 → $FFFF au sous-dépassement */
            if (!underflow) continue;

            via->t1_reload = true;           /* rechargement au cycle suivant, tous modes */
            if (!via->t1_running) continue;  /* one-shot déjà tiré : pas d'interruption */

            via->ifr |= VIA_INT_T1;
            via_check_irq(via);

            /* PB7 n'est la sortie de Timer 1 (WRITE cassette de l'ORIC) que si
             * DDRB bit7 ET ACR bit7 sont à 1 (datasheet p.9). Mode signal carré
             * (bit6=1) : bascule à chaque sous-dépassement ; one-shot (bit6=0) :
             * une seule impulsion haute (PB7 ayant été tiré bas à l'écriture
             * de T1C-H). */
            /* Neo6502Vic20 : la bascule PB7 change d'état à chaque
             * sous-dépassement qui interrompt (à chaque période en continu,
             * une fois en one-shot), sans condition sur l'ACR ni DDRB ;
             * l'ACR bit7 décide seulement si elle sort sur PB7 (référence
             * viavarious via10-13 du vrai VIC-20). */
            via->pb7_pin = !via->pb7_pin;

            if (!(via->acr & 0x40))
                via->t1_running = false;     /* one-shot : plus d'interruption */
        }
    }

    /* ─── Timer 2 (mode timer : one-shot seulement ; le mode comptage
     * d'impulsions est piloté par via_pb6_pulse(), pas par φ2) ───
     * Même mécanique de sous-dépassement que Timer 1, sans rechargement. */
    /* Neo6502Vic20 : le mode de T2 choisi par l'ACR (bit 5 : φ2 ou PB6) ne
     * prend effet qu'au cycle suivant l'écriture, dans les deux sens
     * (références viavarious du vrai VIC-20 : via1 G, via2 E/I/K, via9). */
    for (int i = 0; i < cycles; i++) {
        if (via->sr_t2_pending && --via->sr_t2_pending == 0)
            via_sr_event(via);
        const bool phi2 = via->t2_phi2;
        const bool hold = via->t2_hold;  /* cycle de l'écriture de T2C-H */
        via->t2_phi2 = !(via->acr & 0x20);
        via->t2_hold = false;
        if (!via->t2_active || !phi2 || hold) continue;
        /* Neo6502Vic20 : registre à décalage cadencé par T2 (ACR b4-2 = 001,
         * 100, 101) : T2 compte sur 8 bits ; l'octet bas se recharge depuis
         * le latch bas (période latch + 2) et chaque sous-dépassement
         * décrémente l'octet haut ; le drapeau T2 se lève une fois, au passage
         * des 16 bits à $FFFF (référence viavarious via20/via21 du vrai
         * VIC-20). */
        const uint8_t srm = via->acr & 0x1C;
        if (srm == 0x04 || srm == 0x10 || srm == 0x14) {
            if (via->t2_reload) {
                via->t2_counter = (uint16_t)((via->t2_counter & 0xFF00) | via->t2_latch);
                via->t2_reload = false;
                continue;
            }
            uint8_t lo = (uint8_t)via->t2_counter;
            uint8_t hi = (uint8_t)(via->t2_counter >> 8);
            if (lo == 0) {
                if (hi == 0 && via->t2_running) {
                    via->ifr |= VIA_INT_T2;
                    via_check_irq(via);
                    via->t2_running = false;
                }
                hi--;
                via->t2_reload = true;
                /* Horloge du registre à décalage : un événement (demi-période
                 * de CB1) par sous-dépassement de l'octet bas, 2 cycles plus
                 * tard (chronogramme de VICE viacore.c : t2_shift_alarm) */
                via->sr_t2_pending = 2;
            }
            lo--;
            via->t2_counter = (uint16_t)((hi << 8) | lo);
            continue;
        }
        {
            bool underflow = (via->t2_counter == 0x0000);
            via->t2_counter--;
            if (!underflow) continue;
            if (!via->t2_running) continue;  /* déjà tiré : le compteur enroule */
            via->ifr |= VIA_INT_T2;
            via_check_irq(via);
            via->t2_running = false;         /* one-shot */
        }
    }

    /* Neo6502Vic20 : registre à décalage en φ2 (010, 110) : un événement
     * par cycle (deux par bit) ; les modes T2 sont cadencés dans la boucle de
     * Timer 2, les modes CB1 externes par via_shift_clock(). */
    {
        uint8_t srmode = via->acr & 0x1C;
        if (srmode == 0x08 || srmode == 0x18) {
            for (int i = 0; i < cycles; i++) {
                if (via->sr_delay) { via->sr_delay--; continue; }
                via_sr_event(via);
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
    /* Each call models one PB6 negative edge. The counter keeps counting pulses
     * even after a one-shot time-out (t2_active); only the first underflow with
     * t2_running set raises the flag (datasheet Fig 19). */
    if (!via->t2_active || !(via->acr & 0x20)) return;
    if (via->t2_counter == 0) {
        via->t2_counter = 0xFFFF;         /* underflow */
        if (via->t2_running) {
            via->ifr |= VIA_INT_T2;
            via_check_irq(via);
            via->t2_running = false;
        }
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

bool via_get_pb7(via6522_t* via) {
    /* PB7 driven by Timer 1 (Oric cassette WRITE line) only when BOTH DDRB bit7
     * and ACR bit7 are set (datasheet p.9). */
    if (via->acr & 0x80) return via->pb7_pin;  /* Neo6502Vic20 : sans condition sur DDRB */
    /* Otherwise PB7 is a normal port pin: output register bit7 if configured
     * as output, else idle high (pulled up). */
    if (via->ddrb & 0x80) return (via->orb & 0x80) != 0;
    return true;
}
