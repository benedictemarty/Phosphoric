/**
 * @file acia6551.h
 * @brief MOS 6551 ACIA (Asynchronous Communication Interface Adapter)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-22
 *
 * Emulates the 6551 ACIA used in ORIC serial interfaces (Digitelec DTL 2000,
 * MCP RS232-C, Kenema 300/300, Telestrat built-in) at $031C-$031F.
 *
 * Register map:
 *   $031C (R)  = Receiver Data Register (RDR)
 *   $031C (W)  = Transmitter Data Register (TDR)
 *   $031D (R)  = Status Register
 *   $031D (W)  = Programmed Reset
 *   $031E (R/W) = Command Register
 *   $031F (R/W) = Control Register
 *
 * Hardware references:
 *   - MOS 6551 datasheet (Princeton)
 *   - Defence Force Wiki: oric:hardware:serial
 *   - Digitelec DTL 2000 (V23 1200/75 Minitel modem)
 */

#ifndef ACIA6551_H
#define ACIA6551_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations */
typedef struct serial_backend_s serial_backend_t;
typedef struct emulator_s emulator_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  I/O Address Map
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_DEFAULT_BASE   0x031C  /* Default I/O base address */

/* Register indices (low 2 bits of address, used by acia_read/acia_write) */
#define ACIA_REG_DATA       0x00    /* R: RDR, W: TDR */
#define ACIA_REG_STATUS     0x01    /* R: Status, W: Programmed Reset */
#define ACIA_REG_COMMAND    0x02    /* R/W: Command Register */
#define ACIA_REG_CONTROL    0x03    /* R/W: Control Register */

#define ACIA_ADDR_MASK      0x03    /* 2-bit register select (A0-A1) */

/* ═══════════════════════════════════════════════════════════════════════
 *  Status Register bits (read $031D)
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_STATUS_PE      0x01    /* Bit 0: Parity Error */
#define ACIA_STATUS_FE      0x02    /* Bit 1: Framing Error */
#define ACIA_STATUS_OVRN    0x04    /* Bit 2: Overrun */
#define ACIA_STATUS_RDRF    0x08    /* Bit 3: Receiver Data Register Full */
#define ACIA_STATUS_TDRE    0x10    /* Bit 4: Transmitter Data Register Empty */
#define ACIA_STATUS_DCD     0x20    /* Bit 5: Data Carrier Detect (active low) */
#define ACIA_STATUS_DSR     0x40    /* Bit 6: Data Set Ready (active low) */
#define ACIA_STATUS_IRQ     0x80    /* Bit 7: IRQ occurred */

/* ═══════════════════════════════════════════════════════════════════════
 *  Command Register bits (R/W $031E)
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_CMD_DTR        0x01    /* Bit 0: Data Terminal Ready */
#define ACIA_CMD_IRD        0x02    /* Bit 1: Receiver IRQ Disable (1=disabled) */
#define ACIA_CMD_TIC_MASK   0x0C    /* Bits 3-2: Transmitter Interrupt Control */
#define ACIA_CMD_TIC_SHIFT  2
#define ACIA_CMD_ECHO       0x10    /* Bit 4: Echo mode (1=enabled) */
#define ACIA_CMD_PME        0x20    /* Bit 5: Parity Mode Enable */
#define ACIA_CMD_PMC_MASK   0xC0    /* Bits 7-6: Parity Mode Control */
#define ACIA_CMD_PMC_SHIFT  6

/* TIC field values */
#define ACIA_TIC_RTS_HIGH_IRQ_OFF   0x00  /* RTS high, TX IRQ disabled */
#define ACIA_TIC_RTS_LOW_IRQ_ON     0x01  /* RTS low, TX IRQ enabled */
#define ACIA_TIC_RTS_LOW_IRQ_OFF    0x02  /* RTS low, TX IRQ disabled */
#define ACIA_TIC_RTS_LOW_BRK        0x03  /* RTS low, TX break on line */

/* ═══════════════════════════════════════════════════════════════════════
 *  Control Register bits (R/W $031F)
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_CTL_BAUD_MASK  0x0F    /* Bits 3-0: Baud Rate Generator */
#define ACIA_CTL_RXCLK      0x10    /* Bit 4: Receiver Clock Source (0=external) */
#define ACIA_CTL_WL_MASK    0x60    /* Bits 6-5: Word Length */
#define ACIA_CTL_WL_SHIFT   5
#define ACIA_CTL_SBN        0x80    /* Bit 7: Stop Bit Number (0=1, 1=2) */

/* Baud rate table indices (bits 3-0 of Control Register) */
#define ACIA_BAUD_EXT       0x00    /* External clock */
#define ACIA_BAUD_50        0x01
#define ACIA_BAUD_75        0x02
#define ACIA_BAUD_110       0x03
#define ACIA_BAUD_135       0x04
#define ACIA_BAUD_150       0x05
#define ACIA_BAUD_300       0x06
#define ACIA_BAUD_600       0x07
#define ACIA_BAUD_1200      0x08
#define ACIA_BAUD_1800      0x09
#define ACIA_BAUD_2400      0x0A
#define ACIA_BAUD_3600      0x0B
#define ACIA_BAUD_4800      0x0C
#define ACIA_BAUD_7200      0x0D
#define ACIA_BAUD_9600      0x0E
#define ACIA_BAUD_19200     0x0F

/* Word length values (bits 6-5) */
#define ACIA_WL_8           0x00    /* 8 data bits */
#define ACIA_WL_7           0x01    /* 7 data bits */
#define ACIA_WL_6           0x02    /* 6 data bits */
#define ACIA_WL_5           0x03    /* 5 data bits */

/* ═══════════════════════════════════════════════════════════════════════
 *  V23 Minitel/Prestel mode preset
 * ═══════════════════════════════════════════════════════════════════════ */

/* V23 mode: 1200 baud receive, 75 baud transmit (half-duplex asymmetric)
 * Used by Digitelec DTL 2000, Minitel, Prestel */
#define ACIA_V23_RX_BAUD    1200
#define ACIA_V23_TX_BAUD    75

/* ═══════════════════════════════════════════════════════════════════════
 *  RX FIFO buffer (emulator enhancement)
 *
 *  The real MOS 6551 has only a 1-byte RX buffer, causing overrun when
 *  the CPU can't read fast enough (e.g. screen clear = 46000 cycles,
 *  5 bytes lost at 1200 baud). Real ORIC programs used IRQ + software
 *  buffer to work around this.
 *
 *  The FIFO option (--serial-buffer N) adds a transparent receive queue.
 *  When enabled, incoming bytes are queued in the FIFO instead of
 *  overwriting RDR. The ACIA still presents one byte at a time via RDR,
 *  but the next byte is automatically loaded from the FIFO when RDR is
 *  read. RDRF stays set as long as the FIFO is non-empty.
 *
 *  The WDC 65C51 improved on the MOS 6551 by re-triggering IRQ while
 *  RDRF is set. The --serial-irq-on-rdrf option emulates this behavior.
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_FIFO_MAX_SIZE  4096    /* Maximum configurable FIFO depth */

/* ═══════════════════════════════════════════════════════════════════════
 *  ACIA device structure
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct acia6551_s {
    /* Registers */
    uint8_t tdr;                /**< Transmitter Data Register (write) */
    uint8_t rdr;                /**< Receiver Data Register (read) */
    uint8_t status;             /**< Status Register (read-only) */
    uint8_t command;            /**< Command Register (R/W) */
    uint8_t control;            /**< Control Register (R/W) */

    /* Frame format (decoded from control/command registers) */
    uint8_t  framebits;         /**< Total bits per frame: start + data + parity + stop */
    uint32_t baud_rate;         /**< Current decoded baud rate (Hz) */
    uint8_t  bitmask;           /**< Data mask from word length (0xFF=8bit, 0x7F=7bit, etc.) */

    /* Internal state */
    bool    tx_pending;         /**< TDR has data waiting to be sent */
    bool    rx_full;            /**< RDR has unread data */
    bool    irq_line;           /**< Current IRQ output state */

    /* Timing: cycle counter for baud rate simulation */
    int32_t tx_cycles;          /**< Cycles until TX completes */
    int32_t rx_cycles;          /**< Cycles until next RX poll */
    int32_t tx_reload;          /**< Cycles per TX byte (from baud rate) */
    int32_t rx_reload;          /**< Cycles per RX byte (from baud rate) */

    /* V23 asymmetric mode (Minitel/Prestel/Digitelec DTL 2000) */
    bool    v23_mode;           /**< Asymmetric baud: 1200 RX / 75 TX */

    /* External signal lines */
    bool    dcd;                /**< Data Carrier Detect (true = active/low) */
    bool    dsr;                /**< Data Set Ready (true = active/low) */
    bool    cts;                /**< Clear To Send (true = active/low) */

    /* RX FIFO buffer (emulator enhancement, not in real 6551) */
    uint8_t* rx_fifo;           /**< Ring buffer (NULL = disabled, 1-byte mode) */
    int     rx_fifo_size;       /**< Configured FIFO depth (0 = disabled) */
    int     rx_fifo_head;       /**< Write position */
    int     rx_fifo_tail;       /**< Read position */
    int     rx_fifo_count;      /**< Bytes in FIFO */

    /* Enhanced IRQ mode (WDC 65C51 behavior) */
    bool    irq_on_rdrf;        /**< Re-trigger IRQ while RDRF set (--serial-irq-on-rdrf) */

    /* External-clock baud rate (--serial-baud N). When the control register
     * selects the external clock (baud index 0), the default is "instant
     * transfer" (1 cycle/byte); a non-zero value here substitutes a realistic
     * baud cadence instead, for timing-sensitive software. 0 = instant. */
    uint32_t ext_clock_baud;
    bool    ext_clock_warned;   /**< Instant-transfer warning already emitted */

    /* Serial trace / debug dump (--serial-trace FILE) */
    FILE*   trace_file;         /**< Trace output file (NULL = disabled) */
    uint64_t trace_cycle;       /**< Current CPU cycle for timestamping */

    /* Backend (loopback, TCP, PTY, modem, COM) */
    serial_backend_t* backend;

    /* CPU IRQ routing */
    void (*irq_set)(emulator_t* emu);
    void (*irq_clr)(emulator_t* emu);
    emulator_t* irq_userdata;
} acia6551_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize ACIA to power-on state
 */
void acia_init(acia6551_t* acia);

/**
 * @brief Hardware reset (equivalent to /RES pin)
 */
void acia_reset(acia6551_t* acia);

/**
 * @brief Read ACIA register
 * @param addr Address ($031C-$031F)
 * @return Register value
 */
uint8_t acia_read(acia6551_t* acia, uint16_t addr);

/**
 * @brief Write ACIA register
 * @param addr Address ($031C-$031F)
 * @param value Data to write
 */
void acia_write(acia6551_t* acia, uint16_t addr, uint8_t value);

/**
 * @brief Advance ACIA state by N CPU cycles (aggregated)
 *
 * Called once per CPU instruction with the cycle count of that
 * instruction (typically 2-7). Handles TX/RX timing based on baud rate.
 *
 * @param cycles  Number of CPU cycles elapsed (from cpu_step return)
 */
void acia_tick(acia6551_t* acia, int cycles);

/**
 * @brief Set V23 asymmetric baud mode (Minitel/Prestel)
 *
 * Forces 1200 baud RX / 75 baud TX regardless of Control Register.
 * Used by Digitelec DTL 2000 and similar V23 modems.
 */
void acia_set_v23_mode(acia6551_t* acia, bool enabled);

/**
 * @brief Attach a serial backend (loopback, TCP, PTY)
 */
void acia_set_backend(acia6551_t* acia, serial_backend_t* backend);

/**
 * @brief Set external signal lines (DCD, DSR, CTS)
 */
void acia_set_dcd(acia6551_t* acia, bool active);
void acia_set_dsr(acia6551_t* acia, bool active);
void acia_set_cts(acia6551_t* acia, bool active);

/**
 * @brief Enable RX FIFO buffer (emulator enhancement)
 *
 * Adds a software receive queue to prevent overrun when the CPU
 * can't service RDR fast enough. Not present in real MOS 6551.
 *
 * @param size  FIFO depth in bytes (0 = disable, 1-4096)
 */
void acia_set_rx_fifo(acia6551_t* acia, int size);

/**
 * @brief Enable WDC 65C51-style IRQ re-trigger on RDRF
 *
 * When enabled, the IRQ line stays asserted as long as RDRF is set,
 * even after reading the status register. This prevents lost IRQs
 * during simultaneous TX/RX (the MOS 6551 bug).
 */
void acia_set_irq_on_rdrf(acia6551_t* acia, bool enabled);

/**
 * @brief Set the baud rate used when the program selects the external clock.
 *
 * By default an external-clock selection (baud index 0) means "instant
 * transfer". Passing a non-zero @p baud makes the ACIA time those transfers at
 * that rate instead — useful to exercise timing-sensitive software. 0 restores
 * the instant default.
 */
void acia_set_ext_clock_baud(acia6551_t* acia, uint32_t baud);

/**
 * @brief Enable serial trace logging to file
 *
 * Logs all TX/RX bytes, register access, and signal line changes
 * with CPU cycle timestamps. Format:
 *   CYCLE     DIR  HEX  CHR  STATUS    SIGNALS
 *   00042510  TX   42   B    TDRE      DTR=1 DCD=1 CTS=1
 *
 * @param filename  Output file path (NULL to disable)
 */
void acia_set_trace(acia6551_t* acia, const char* filename);

/**
 * @brief Update cycle counter for trace timestamps
 */
void acia_set_trace_cycle(acia6551_t* acia, uint64_t cycle);

/**
 * @brief Flush trace file buffer (call once per frame)
 */
void acia_trace_flush(acia6551_t* acia);

#endif /* ACIA6551_H */
