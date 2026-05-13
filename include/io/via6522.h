/**
 * @file via6522.h
 * @brief MOS 6522 VIA (Versatile Interface Adapter) emulation
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 *
 * The 6522 VIA provides parallel I/O, timers, and shift register.
 * In ORIC-1, it's used for:
 * - Keyboard scanning (Port A)
 * - Cassette I/O (Port B)
 * - PSG (AY-3-8910) control (Port B)
 * - Printer interface
 */

#ifndef VIA6522_H
#define VIA6522_H

#include <stdint.h>
#include <stdbool.h>

/* VIA Register offsets (base address $0300 in ORIC-1) */
#define VIA_ORB     0x00  /**< Output Register B */
#define VIA_ORA     0x01  /**< Output Register A */
#define VIA_DDRB    0x02  /**< Data Direction Register B */
#define VIA_DDRA    0x03  /**< Data Direction Register A */
#define VIA_T1CL    0x04  /**< Timer 1 Counter Low */
#define VIA_T1CH    0x05  /**< Timer 1 Counter High */
#define VIA_T1LL    0x06  /**< Timer 1 Latch Low */
#define VIA_T1LH    0x07  /**< Timer 1 Latch High */
#define VIA_T2CL    0x08  /**< Timer 2 Counter Low */
#define VIA_T2CH    0x09  /**< Timer 2 Counter High */
#define VIA_SR      0x0A  /**< Shift Register */
#define VIA_ACR     0x0B  /**< Auxiliary Control Register */
#define VIA_PCR     0x0C  /**< Peripheral Control Register */
#define VIA_IFR     0x0D  /**< Interrupt Flag Register */
#define VIA_IER     0x0E  /**< Interrupt Enable Register */
#define VIA_ORA_NH  0x0F  /**< ORA without handshake */

/* Interrupt flags */
#define VIA_INT_CA2     0x01
#define VIA_INT_CA1     0x02
#define VIA_INT_SR      0x04
#define VIA_INT_CB2     0x08
#define VIA_INT_CB1     0x10
#define VIA_INT_T2      0x20
#define VIA_INT_T1      0x40
#define VIA_INT_ANY     0x80
#define VIA_IER_MASK    0x7F  /**< Lower 7 bits: individual interrupt enable/flag bits */

/**
 * @brief VIA 6522 state structure
 */
typedef struct via6522_s {
    /* I/O Registers */
    uint8_t ora;        /**< Output Register A */
    uint8_t orb;        /**< Output Register B */
    uint8_t ira;        /**< Input Register A */
    uint8_t irb;        /**< Input Register B */
    uint8_t ddra;       /**< Data Direction Register A */
    uint8_t ddrb;       /**< Data Direction Register B */

    /* Timers */
    uint16_t t1_counter;    /**< Timer 1 Counter */
    uint16_t t1_latch;      /**< Timer 1 Latch */
    uint16_t t2_counter;    /**< Timer 2 Counter */
    uint8_t  t2_latch;      /**< Timer 2 Latch (low byte only) */
    bool     t1_running;    /**< Timer 1 running flag */
    bool     t2_running;    /**< Timer 2 running flag */

    /* Shift Register */
    uint8_t sr;         /**< Shift Register */
    uint8_t sr_count;   /**< Shift counter */

    /* Control Registers */
    uint8_t acr;        /**< Auxiliary Control Register */
    uint8_t pcr;        /**< Peripheral Control Register */
    uint8_t ifr;        /**< Interrupt Flag Register */
    uint8_t ier;        /**< Interrupt Enable Register */

    /* External callbacks */
    uint8_t (*porta_read)(void* userdata);
    void (*porta_write)(uint8_t value, void* userdata);
    uint8_t (*portb_read)(void* userdata);
    void (*portb_write)(uint8_t value, void* userdata);
    void* userdata;

    /* CB1 pin state for edge detection */
    bool cb1_pin;  /**< Current CB1 pin level (true=high, idle on Oric) */

    /* IRQ output state (tracks /IRQ pin level to avoid spurious callbacks) */
    bool irq_line;  /**< Current IRQ output: true = asserted, false = deasserted */

    /* IRQ callback */
    void (*irq_callback)(bool state, void* userdata);
    void* irq_userdata;
} via6522_t;

/**
 * @brief Initialize VIA
 *
 * @param via Pointer to VIA structure
 */
void via_init(via6522_t* via);

/**
 * @brief Reset VIA
 *
 * @param via Pointer to VIA structure
 */
void via_reset(via6522_t* via);

/**
 * @brief Read from VIA register
 *
 * @param via Pointer to VIA structure
 * @param reg Register offset (0x00-0x0F)
 * @return Register value
 */
uint8_t via_read(via6522_t* via, uint8_t reg);

/**
 * @brief Write to VIA register
 *
 * @param via Pointer to VIA structure
 * @param reg Register offset (0x00-0x0F)
 * @param value Value to write
 */
void via_write(via6522_t* via, uint8_t reg, uint8_t value);

/**
 * @brief Update VIA timers (call every CPU cycle)
 *
 * @param via Pointer to VIA structure
 * @param cycles Number of cycles elapsed
 */
void via_update(via6522_t* via, int cycles);

/**
 * @brief Set port callbacks
 *
 * @param via Pointer to VIA structure
 * @param porta_read Port A read callback
 * @param porta_write Port A write callback
 * @param portb_read Port B read callback
 * @param portb_write Port B write callback
 * @param userdata User data for callbacks
 */
void via_set_port_callbacks(via6522_t* via,
                            uint8_t (*porta_read)(void*),
                            void (*porta_write)(uint8_t, void*),
                            uint8_t (*portb_read)(void*),
                            void (*portb_write)(uint8_t, void*),
                            void* userdata);

/**
 * @brief Set IRQ callback
 *
 * @param via Pointer to VIA structure
 * @param callback IRQ state change callback
 * @param userdata User data for callback
 */
void via_set_irq_callback(via6522_t* via,
                         void (*callback)(bool, void*),
                         void* userdata);

/**
 * @brief Trigger CA1 interrupt
 *
 * @param via Pointer to VIA structure
 */
void via_trigger_ca1(via6522_t* via);

/**
 * @brief Trigger CA2 interrupt
 *
 * @param via Pointer to VIA structure
 */
void via_trigger_ca2(via6522_t* via);

/**
 * @brief Trigger CB1 interrupt (legacy: pulse high→low→high)
 *
 * @param via Pointer to VIA structure
 */
void via_trigger_cb1(via6522_t* via);

/**
 * @brief Set CB1 pin level with edge detection
 *
 * Compares new state against cb1_pin. If a transition occurs,
 * checks PCR bit 4 to determine active edge (0=falling, 1=rising).
 * Sets IFR CB1 flag only on the correct edge.
 *
 * @param via Pointer to VIA structure
 * @param state New CB1 pin level (true=high, false=low)
 */
void via_set_cb1(via6522_t* via, bool state);

/**
 * @brief Trigger CB2 interrupt
 *
 * @param via Pointer to VIA structure
 */
void via_trigger_cb2(via6522_t* via);

#endif /* VIA6522_H */
