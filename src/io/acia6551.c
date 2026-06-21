/**
 * @file acia6551.c
 * @brief MOS 6551 ACIA — Asynchronous Communication Interface Adapter
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-22
 *
 * Faithful emulation of the 6551 ACIA as used in ORIC serial interfaces
 * (Digitelec DTL 2000, MCP RS232-C, Kenema 300/300, Telestrat).
 * Mapped at $031C-$031F (standard de facto for ORIC).
 *
 * Emulator enhancements beyond real MOS 6551:
 *   - RX FIFO buffer (--serial-buffer N) prevents overrun during long
 *     CPU operations. Real programs used IRQ + software buffer.
 *   - WDC 65C51 IRQ mode (--serial-irq-on-rdrf) re-triggers IRQ while
 *     RDRF is set, preventing lost IRQs during simultaneous TX/RX.
 *
 * References:
 *   - MOS 6551 datasheet (Princeton)
 *   - WDC W65C51N datasheet (improved IRQ behavior)
 *   - Defence Force Wiki: oric:hardware:serial
 */

#include <stdlib.h>
#include <string.h>
#include "io/acia6551.h"
#include "io/serial_backend.h"
#include "utils/logging.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  ACIA 6551 crystal oscillator and baud rate
 * ═══════════════════════════════════════════════════════════════════════ */

#define ACIA_XTAL_HZ       1843200
#define ACIA_PRESCALER      16
#define ACIA_INTERNAL_HZ    (ACIA_XTAL_HZ / ACIA_PRESCALER)  /* 115200 */

static const int baud_rate_table[16] = {
    0,      /* 0: External clock (treated as max speed) */
    50, 75, 110, 135, 150, 300, 600,
    1200, 1800, 2400, 3600, 4800, 7200, 9600, 19200
};

/**
 * @brief Calculate frame format bits from control/command registers
 */
static void acia_calc_framebits(acia6551_t* acia)
{
    uint8_t wl_field = (acia->control & ACIA_CTL_WL_MASK) >> ACIA_CTL_WL_SHIFT;
    uint8_t wordlen = (uint8_t)(8 - wl_field);
    uint8_t stopbits = (acia->control & ACIA_CTL_SBN) ? 2 : 1;
    uint8_t parity = (acia->command & ACIA_CMD_PME) ? 1 : 0;

    acia->framebits = (uint8_t)(1 + wordlen + parity + stopbits);
    acia->bitmask = (uint8_t)((1U << wordlen) - 1);
}

/* Minimum cycles between two backend RX polls. At a real baud rate the
 * per-byte interval is much larger (e.g. ~1041 cycles at 9600), so this
 * floor never affects baud-accurate timing. It only matters when baud is 0
 * (external clock / "instant transfer", rx_reload == 1): without it, a
 * socket-backed backend would do a poll()/read() syscall every CPU cycle
 * while a connection is open. 32 cycles ≈ 32 µs of emulated time —
 * imperceptible latency, but it caps the syscall rate ~32×. */
#define ACIA_RX_POLL_FLOOR 32

/**
 * @brief Compute CPU cycles per byte for a given baud rate and frame format
 */
static int32_t cycles_per_byte(int baud, uint8_t framebits)
{
    if (baud <= 0) return 1;
    return (int32_t)((1000000L * framebits) / baud);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Serial trace helpers — flushed once per frame via acia_trace_flush()
 * ═══════════════════════════════════════════════════════════════════════ */

static void trace_header(acia6551_t* acia)
{
    if (!acia->trace_file) return;
    fprintf(acia->trace_file,
            "# Phosphoric ACIA 6551 Serial Trace\n"
            "# CYCLE      DIR  HEX  CHR  STATUS    FIFO  SIGNALS\n"
            "# ────────── ───  ──── ───  ────────  ────  ──────────────────\n");
    fflush(acia->trace_file);
}

static void trace_signals(acia6551_t* acia, char* buf, int size)
{
    snprintf(buf, (size_t)size, "DTR=%d DCD=%d CTS=%d DSR=%d",
             (acia->command & ACIA_CMD_DTR) ? 1 : 0,
             acia->dcd ? 1 : 0,
             acia->cts ? 1 : 0,
             acia->dsr ? 1 : 0);
}

static void trace_data(acia6551_t* acia, const char* dir, uint8_t byte)
{
    if (!acia->trace_file) return;
    char sig[64];
    trace_signals(acia, sig, (int)sizeof(sig));
    char ch = (byte >= 32 && byte < 127) ? (char)byte : '.';
    fprintf(acia->trace_file, "%010llu  %-3s  %02X   %c    %c%c%c%c%c%c%c%c  %3d   %s\n",
            (unsigned long long)acia->trace_cycle,
            dir, byte, ch,
            (acia->status & ACIA_STATUS_IRQ)  ? 'I' : '.',
            (acia->status & ACIA_STATUS_DSR)  ? 'D' : '.',
            (acia->status & ACIA_STATUS_DCD)  ? 'C' : '.',
            (acia->status & ACIA_STATUS_TDRE) ? 'T' : '.',
            (acia->status & ACIA_STATUS_RDRF) ? 'R' : '.',
            (acia->status & ACIA_STATUS_OVRN) ? 'O' : '.',
            (acia->status & ACIA_STATUS_FE)   ? 'F' : '.',
            (acia->status & ACIA_STATUS_PE)   ? 'P' : '.',
            acia->rx_fifo ? acia->rx_fifo_count : 0,
            sig);
    /* No fflush here — flushed per frame via acia_trace_flush() */
}

static void trace_event(acia6551_t* acia, const char* event)
{
    if (!acia->trace_file) return;
    char sig[64];
    trace_signals(acia, sig, (int)sizeof(sig));
    fprintf(acia->trace_file, "%010llu  SIG  --   -    --------  ---   %s  %s\n",
            (unsigned long long)acia->trace_cycle, sig, event);
}

static void trace_reg(acia6551_t* acia, const char* dir, const char* reg, uint8_t val)
{
    if (!acia->trace_file) return;
    fprintf(acia->trace_file, "%010llu  %-3s  %02X   -    %-8s  ---\n",
            (unsigned long long)acia->trace_cycle, dir, val, reg);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  RX FIFO helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static bool fifo_push(acia6551_t* acia, uint8_t byte)
{
    if (!acia->rx_fifo || acia->rx_fifo_count >= acia->rx_fifo_size)
        return false;
    acia->rx_fifo[acia->rx_fifo_head] = byte;
    acia->rx_fifo_head = (acia->rx_fifo_head + 1) % acia->rx_fifo_size;
    acia->rx_fifo_count++;
    return true;
}

static bool fifo_pop(acia6551_t* acia, uint8_t* byte)
{
    if (!acia->rx_fifo || acia->rx_fifo_count <= 0)
        return false;
    *byte = acia->rx_fifo[acia->rx_fifo_tail];
    acia->rx_fifo_tail = (acia->rx_fifo_tail + 1) % acia->rx_fifo_size;
    acia->rx_fifo_count--;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  IRQ management
 * ═══════════════════════════════════════════════════════════════════════ */

static void acia_update_irq(acia6551_t* acia)
{
    bool irq_wanted = false;

    /* RX IRQ: RDRF set AND receiver IRQ enabled (IRD bit = 0) */
    if ((acia->status & ACIA_STATUS_RDRF) && !(acia->command & ACIA_CMD_IRD)) {
        irq_wanted = true;
    }

    /* TX IRQ: TDRE set AND TIC = 01 (RTS low, TX IRQ enabled) */
    uint8_t tic = (acia->command & ACIA_CMD_TIC_MASK) >> ACIA_CMD_TIC_SHIFT;
    if ((acia->status & ACIA_STATUS_TDRE) && tic == ACIA_TIC_RTS_LOW_IRQ_ON) {
        irq_wanted = true;
    }

    /* Update status register IRQ bit */
    if (irq_wanted) {
        acia->status |= ACIA_STATUS_IRQ;
    } else {
        acia->status &= ~ACIA_STATUS_IRQ;
    }

    if (acia->irq_on_rdrf) {
        /* WDC 65C51 mode: IRQ is level-triggered on RDRF.
         * Assert /IRQ whenever irq_wanted is true, regardless of previous state.
         * Deassert only when irq_wanted goes false.
         * This means: even if irq_line is already true and status was read
         * (which clears irq_line), the NEXT call to acia_update_irq will
         * re-assert it if RDRF is still set. */
        if (irq_wanted) {
            if (!acia->irq_line) {
                acia->irq_line = true;
                if (acia->irq_set) acia->irq_set(acia->irq_userdata);
            }
        } else {
            if (acia->irq_line) {
                acia->irq_line = false;
                if (acia->irq_clr) acia->irq_clr(acia->irq_userdata);
            }
        }
    } else {
        /* Standard MOS 6551: edge-triggered IRQ.
         * Only fires on false→true transition. Once irq_line is true,
         * it stays true until cleared by reading STATUS register. */
        if (irq_wanted && !acia->irq_line) {
            acia->irq_line = true;
            if (acia->irq_set) acia->irq_set(acia->irq_userdata);
        } else if (!irq_wanted && acia->irq_line) {
            acia->irq_line = false;
            if (acia->irq_clr) acia->irq_clr(acia->irq_userdata);
        }
    }
}

/**
 * @brief Recalculate TX/RX timing from control/command registers
 */
static void acia_update_timing(acia6551_t* acia)
{
    acia_calc_framebits(acia);

    if (acia->v23_mode) {
        acia->baud_rate = ACIA_V23_RX_BAUD;
        acia->rx_reload = cycles_per_byte(ACIA_V23_RX_BAUD, acia->framebits);
        acia->tx_reload = cycles_per_byte(ACIA_V23_TX_BAUD, acia->framebits);
    } else {
        int baud_idx = acia->control & ACIA_CTL_BAUD_MASK;
        int baud = baud_rate_table[baud_idx];
        if (baud == 0 && acia->ext_clock_baud > 0) {
            /* External clock selected, but the user asked for a realistic
             * cadence (--serial-baud) instead of instant transfer. */
            baud = (int)acia->ext_clock_baud;
        }
        acia->baud_rate = (uint32_t)baud;
        acia->rx_reload = cycles_per_byte(baud, acia->framebits);
        acia->tx_reload = cycles_per_byte(baud, acia->framebits);
        if (baud == 0 && !acia->ext_clock_warned) {
            log_warning("ACIA: baud rate 0 (external clock) — using instant transfer "
                        "(set --serial-baud N for realistic timing)");
            acia->ext_clock_warned = true;
        }
    }

    /* Resynchronize counters when timing changes */
    if (acia->tx_cycles <= 0 || acia->tx_cycles > acia->tx_reload)
        acia->tx_cycles = acia->tx_reload;
    if (acia->rx_cycles <= 0 || acia->rx_cycles > acia->rx_reload)
        acia->rx_cycles = acia->rx_reload;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Init / Reset
 * ═══════════════════════════════════════════════════════════════════════ */

void acia_init(acia6551_t* acia)
{
    memset(acia, 0, sizeof(acia6551_t));

    /* Power-on state per datasheet */
    acia->status = ACIA_STATUS_TDRE;
    acia->command = 0x00;
    acia->control = 0x00;
    acia->tx_pending = false;
    acia->rx_full = false;
    acia->irq_line = false;

    /* Default signal lines: carrier and ready active */
    acia->dcd = true;
    acia->dsr = true;
    acia->cts = true;

    acia->v23_mode = false;

    /* Calculate timing and initialize cycle counters */
    acia_update_timing(acia);

    log_info("ACIA 6551 initialized (xtal %d Hz / %d = %d Hz)",
             ACIA_XTAL_HZ, ACIA_PRESCALER, ACIA_INTERNAL_HZ);
}

void acia_reset(acia6551_t* acia)
{
    acia->status = ACIA_STATUS_TDRE;
    acia->command = acia->command & 0xE0;
    acia->tx_pending = false;
    acia->rx_full = false;

    /* Flush FIFO on reset */
    acia->rx_fifo_head = 0;
    acia->rx_fifo_tail = 0;
    acia->rx_fifo_count = 0;

    /* Deassert IRQ */
    if (acia->irq_line) {
        acia->irq_line = false;
        if (acia->irq_clr) acia->irq_clr(acia->irq_userdata);
    }

    trace_event(acia, "PROGRAMMED RESET");
    log_debug("ACIA 6551 programmed reset");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Register Read
 * ═══════════════════════════════════════════════════════════════════════ */

uint8_t acia_read(acia6551_t* acia, uint16_t addr)
{
    switch (addr & ACIA_ADDR_MASK) {
    case 0x00: {
        /* $031C: Read Receiver Data Register */
        uint8_t data = acia->rdr;
        acia->rx_full = false;
        acia->status &= ~(ACIA_STATUS_RDRF | ACIA_STATUS_OVRN |
                          ACIA_STATUS_PE | ACIA_STATUS_FE);

        /* FIFO: auto-load next byte if available */
        if (acia->rx_fifo && acia->rx_fifo_count > 0) {
            uint8_t next;
            if (fifo_pop(acia, &next)) {
                acia->rdr = next;
                acia->rx_full = true;
                acia->status |= ACIA_STATUS_RDRF;
            }
        }

        acia_update_irq(acia);
        return data;
    }

    case 0x01: {
        /* $031D: Read Status Register.
         * DCD/DSR bits reflect live pin state (stored in acia->status). */
        uint8_t status = acia->status;

        /* Clear IRQ on status read (MOS 6551 datasheet) */
        acia->status &= ~ACIA_STATUS_IRQ;
        if (acia->irq_line) {
            acia->irq_line = false;
            if (acia->irq_clr) acia->irq_clr(acia->irq_userdata);
        }

        /* 65C51 mode: immediately re-evaluate IRQ (re-assert if RDRF set) */
        if (acia->irq_on_rdrf) {
            acia_update_irq(acia);
        }

        return status;
    }

    case 0x02:
        return acia->command;

    case 0x03:
        return acia->control;

    default:
        return 0xFF;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Register Write
 * ═══════════════════════════════════════════════════════════════════════ */

void acia_write(acia6551_t* acia, uint16_t addr, uint8_t value)
{
    switch (addr & ACIA_ADDR_MASK) {
    case 0x00:
        /* TX overwrite check: if TDR is still pending, the previous byte
         * is lost (program didn't check TDRE before writing) */
        if (acia->tx_pending) {
            trace_event(acia, "TX OVERWRITE (TDR written before TDRE)");
        }
        acia->tdr = value & acia->bitmask;
        acia->tx_pending = true;
        acia->tx_cycles = acia->tx_reload;
        acia->status &= ~ACIA_STATUS_TDRE;
        acia_update_irq(acia);
        break;

    case 0x01:
        (void)value;
        acia_reset(acia);
        break;

    case 0x02:
        acia->command = value;
        trace_reg(acia, "WR", "CMD", value);
        acia_update_timing(acia);
        acia_update_irq(acia);
        break;

    case 0x03:
        acia->control = value;
        trace_reg(acia, "WR", "CTL", value);
        acia_update_timing(acia);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Tick — advance ACIA state by N CPU cycles (aggregated)
 *
 *  Called once per CPU instruction with the cycle count of that
 *  instruction (typically 2-7). This avoids N individual calls per
 *  instruction and reduces overhead significantly.
 * ═══════════════════════════════════════════════════════════════════════ */

void acia_tick(acia6551_t* acia, int cycles)
{
    if (!acia->backend) return;
    if (!(acia->command & ACIA_CMD_DTR)) return;

    /* ── TX path ── */
    if (acia->tx_pending) {
        acia->tx_cycles -= cycles;
        if (acia->tx_cycles <= 0) {
            if (acia->cts && acia->backend->send) {
                acia->backend->send(acia->backend, acia->tdr);
                trace_data(acia, "TX", acia->tdr);
            }
            acia->tx_pending = false;
            acia->status |= ACIA_STATUS_TDRE;
            acia->tx_cycles = acia->tx_reload;
            acia_update_irq(acia);
        }
    }

    /* ── RX path ── */
    acia->rx_cycles -= cycles;
    if (acia->rx_cycles <= 0) {
        /* Reset to the baud-rate interval, but never poll the backend more
         * often than the floor (avoids a per-cycle syscall storm at baud 0). */
        acia->rx_cycles = (acia->rx_reload > ACIA_RX_POLL_FLOOR)
                          ? acia->rx_reload : ACIA_RX_POLL_FLOOR;

        /* Always poll the backend, even with DCD deasserted, so it can
         * raise the carrier on its own. The Digitelec DTL 2000 dials on a
         * DTR edge from inside poll() (digitelec_check_dtr); gating poll()
         * on DCD would deadlock in pure-receive mode — poll() sets DCD, but
         * poll() was never reached because DCD was still low. The DCD guard
         * therefore only gates delivery of received bytes to RDRF below. */
        bool rx_ready = acia->backend->poll &&
                        acia->backend->poll(acia->backend);

        if (acia->dcd && rx_ready) {
            uint8_t byte;
            if (acia->backend->recv(acia->backend, &byte)) {
                byte &= acia->bitmask;

                trace_data(acia, "RX", byte);

                if (acia->rx_fifo) {
                    /* ── FIFO mode: queue byte ── */
                    if (!acia->rx_full) {
                        acia->rdr = byte;
                        acia->rx_full = true;
                        acia->status |= ACIA_STATUS_RDRF;
                    } else if (!fifo_push(acia, byte)) {
                        acia->status |= ACIA_STATUS_OVRN;
                        trace_event(acia, "OVERRUN (FIFO full)");
                    }
                } else {
                    /* ── Classic 1-byte mode ── */
                    if (acia->rx_full) {
                        acia->status |= ACIA_STATUS_OVRN;
                        trace_event(acia, "OVERRUN (RDR not read)");
                    }
                    acia->rdr = byte;
                    acia->rx_full = true;
                    acia->status |= ACIA_STATUS_RDRF;
                }

                /* Echo mode: queue echo byte through TX path.
                 * On real hardware, echo uses the transmitter (subject to
                 * baud rate). We set tx_pending so the echo byte goes through
                 * normal TX timing instead of being sent immediately. */
                if (acia->command & ACIA_CMD_ECHO) {
                    if (!acia->tx_pending) {
                        acia->tdr = byte;
                        acia->tx_pending = true;
                        acia->tx_cycles = acia->tx_reload;
                        acia->status &= ~ACIA_STATUS_TDRE;
                    }
                    /* If TX is busy, echo byte is lost (real hw behavior) */
                }

                acia_update_irq(acia);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Configuration helpers
 * ═══════════════════════════════════════════════════════════════════════ */

void acia_set_v23_mode(acia6551_t* acia, bool enabled)
{
    acia->v23_mode = enabled;
    acia_update_timing(acia);
    if (enabled) {
        log_info("ACIA V23 mode: RX=%d baud, TX=%d baud (Minitel/Prestel)",
                 ACIA_V23_RX_BAUD, ACIA_V23_TX_BAUD);
    }
}

void acia_set_backend(acia6551_t* acia, serial_backend_t* backend)
{
    acia->backend = backend;
}

void acia_set_dcd(acia6551_t* acia, bool active)
{
    bool old = acia->dcd;
    acia->dcd = active;
    /* Update DCD bit in status register (bit 5: 1=carrier lost) */
    if (active) acia->status &= ~ACIA_STATUS_DCD;
    else        acia->status |= ACIA_STATUS_DCD;
    if (old != active) {
        trace_event(acia, active ? "DCD ON (carrier)" : "DCD OFF (no carrier)");
        acia_update_irq(acia);
    }
}

void acia_set_dsr(acia6551_t* acia, bool active)
{
    acia->dsr = active;
    /* Update DSR bit in status register (bit 6: 1=DSR inactive) */
    if (active) acia->status &= ~ACIA_STATUS_DSR;
    else        acia->status |= ACIA_STATUS_DSR;
}

void acia_set_cts(acia6551_t* acia, bool active)
{
    acia->cts = active;
}

void acia_set_rx_fifo(acia6551_t* acia, int size)
{
    free(acia->rx_fifo);
    acia->rx_fifo = NULL;
    acia->rx_fifo_size = 0;
    acia->rx_fifo_head = 0;
    acia->rx_fifo_tail = 0;
    acia->rx_fifo_count = 0;

    if (size > 0) {
        if (size > ACIA_FIFO_MAX_SIZE) size = ACIA_FIFO_MAX_SIZE;
        acia->rx_fifo = (uint8_t*)malloc((size_t)size);
        if (acia->rx_fifo) {
            acia->rx_fifo_size = size;
            log_info("ACIA RX FIFO enabled: %d bytes", size);
        } else {
            log_error("ACIA RX FIFO: allocation failed for %d bytes", size);
        }
    }
}

void acia_set_irq_on_rdrf(acia6551_t* acia, bool enabled)
{
    acia->irq_on_rdrf = enabled;
    if (enabled) {
        log_info("ACIA IRQ mode: WDC 65C51 (re-trigger while RDRF set)");
    }
}

void acia_set_ext_clock_baud(acia6551_t* acia, uint32_t baud)
{
    acia->ext_clock_baud = baud;
    if (baud > 0) {
        log_info("ACIA external-clock baud: %u (realistic timing instead of instant)",
                 baud);
    }
    acia_update_timing(acia);  /* apply immediately if already in external-clock mode */
}

void acia_set_trace(acia6551_t* acia, const char* filename)
{
    if (acia->trace_file) {
        fclose(acia->trace_file);
        acia->trace_file = NULL;
    }

    if (filename) {
        acia->trace_file = fopen(filename, "w");
        if (acia->trace_file) {
            trace_header(acia);
            log_info("ACIA serial trace: %s", filename);
        } else {
            log_error("ACIA serial trace: failed to open %s", filename);
        }
    }
}

void acia_trace_flush(acia6551_t* acia)
{
    if (acia->trace_file) fflush(acia->trace_file);
}

void acia_set_trace_cycle(acia6551_t* acia, uint64_t cycle)
{
    acia->trace_cycle = cycle;
}
