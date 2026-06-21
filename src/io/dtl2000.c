/**
 * @file dtl2000.c
 * @brief Digitelec DTL 2000 — faithful PIA 6821 + ACIA 6850 modem card
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 *
 * See dtl2000.h for the register map and the period-manual semantics.
 */

#include "io/dtl2000.h"
#include "io/serial_backend.h"
#include "utils/logging.h"

#include <string.h>

/* ───────────────────────────────────────────────────────────────────────
 *  Internal helpers
 * ─────────────────────────────────────────────────────────────────────── */

/* Emit one TX/RX line to the trace file (if enabled). @p note flags special
 * cases such as a transmit dropped because the line is open / carrier off. */
static void dtl_trace_byte(dtl2000_t* dev, const char* dir, uint8_t byte,
                           const char* note)
{
    if (!dev->trace) return;
    uint8_t st = dev->acia.status;
    char c = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
    fprintf(dev->trace,
            "%-3s  %02X  '%c'  ST=%02X %s%s%s%s  TX=%u RX=%u  %s\n",
            dir, byte, c, st,
            (st & DTL_ACIA_SR_RDRF) ? "RDRF " : "",
            (st & DTL_ACIA_SR_TDRE) ? "TDRE " : "",
            (st & DTL_ACIA_SR_DCD)  ? "DCD "  : "",
            (st & DTL_ACIA_SR_CTS)  ? "CTS"   : "",
            dev->tx_count, dev->rx_count,
            note ? note : "");
}

/* Recompute baud rates and per-byte cycle reloads from the current mode
 * and the ACIA's frame format. */
static void dtl_recalc_timing(dtl2000_t* dev)
{
    if (dev->symmetric) {
        dev->rx_baud = 1200;
        dev->tx_baud = 1200;
    } else {
        /* V23 appel (terminal Minitel): receive fast, transmit slow */
        dev->rx_baud = DTL_V23_RX_BAUD;  /* 1200 */
        dev->tx_baud = DTL_V23_TX_BAUD;  /* 75   */
    }
    uint32_t fb = dev->acia.framebits ? dev->acia.framebits : 10;
    dev->rx_reload = (int32_t)((uint64_t)DTL2000_CLOCK_HZ * fb / dev->rx_baud);
    dev->tx_reload = (int32_t)((uint64_t)DTL2000_CLOCK_HZ * fb / dev->tx_baud);
    if (dev->rx_reload < 1) dev->rx_reload = 1;
    if (dev->tx_reload < 1) dev->tx_reload = 1;
}

/* Reflect the live signal lines into the ACIA status register. On the DTL
 * 2000, DCD (bit2) is 0 when the distant carrier is present and CTS (bit3)
 * is 0 when the card is clear to send — both active low. */
static void dtl_refresh_signals(dtl2000_t* dev)
{
    bool carrier = dev->line_connected &&
                   dev->backend && dev->backend->connected &&
                   dev->backend->connected(dev->backend);

    /* Drive the ACIA's DCD/CTS input pins from the modem-line state. */
    acia6850_set_dcd(&dev->acia, carrier);
    acia6850_set_cts(&dev->acia, carrier);
}

/* The ACIA's IRQ output (called by acia6850_*) routed to the CPU /IRQ line. */
static void dtl_acia_irq_cb(void* ud, bool active)
{
    dtl2000_t* dev = (dtl2000_t*)ud;
    if (active && dev->irq_set)       dev->irq_set(dev->irq_userdata);
    else if (!active && dev->irq_clr) dev->irq_clr(dev->irq_userdata);
}

/* Decode the Port A output register after a write: line connection + mode. */
static void dtl_pia_update_outputs(dtl2000_t* dev)
{
    bool new_connected = ((dev->pia.ora & DTL_PIA_A_LINE) == 0);
    bool new_symmetric = ((dev->pia.ora & DTL_PIA_A_MODE) == 0);

    if (new_symmetric != dev->symmetric) {
        dev->symmetric = new_symmetric;
        dtl_recalc_timing(dev);
    }
    if (new_connected != dev->line_connected) {
        dev->line_connected = new_connected;
        log_info("DTL 2000: line %s (%s mode)",
                 new_connected ? "CLOSED (connect)" : "OPEN (disconnect)",
                 dev->symmetric ? "symmetric" : "asymmetric V23");
    }
    dtl_refresh_signals(dev);
}

/* Apply a write to the ACIA control register. */
static void dtl_acia_control_write(dtl2000_t* dev, uint8_t value)
{
    bool master_reset = acia6850_control_write(&dev->acia, value);
    if (master_reset) {
        dev->tx_busy   = false;
        dev->tx_cycles = 0;
        dev->rx_cycles = 0;
        dtl_refresh_signals(dev);
        return;
    }
    dtl_recalc_timing(dev);     /* frame format may have changed */
    dtl_refresh_signals(dev);
}

/* Apply a write to the ACIA data register: queue a byte for transmission. */
static void dtl_acia_data_write(dtl2000_t* dev, uint8_t value)
{
    /* The ACIA latches TDR (masked to the frame width) and clears TDRE. */
    acia6850_write_data(&dev->acia, value);
    uint8_t tdr = dev->acia.tdr;

    /* The modem only puts the byte on the line when connected and emitting. */
    if (dev->line_connected && acia6850_rts_low(&dev->acia) &&
        dev->backend && dev->backend->send) {
        dev->backend->send(dev->backend, tdr);
        dev->tx_count++;
        dtl_trace_byte(dev, "TX", tdr, "sent");
    } else {
        dtl_trace_byte(dev, "TX", tdr,
                       dev->line_connected ? "dropped (no carrier)"
                                           : "dropped (line open)");
    }

    dev->tx_busy   = true;
    dev->tx_cycles = dev->tx_reload;
}

/* ───────────────────────────────────────────────────────────────────────
 *  Public API
 * ─────────────────────────────────────────────────────────────────────── */

void dtl2000_init(dtl2000_t* dev, uint16_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr ? base_addr : DTL2000_DEFAULT_BASE;
    dtl2000_reset(dev);
}

void dtl2000_reset(dtl2000_t* dev)
{
    pia6821_init(&dev->pia);
    pia6821_reset(&dev->pia);

    /* Wire the ACIA's IRQ output to the CPU /IRQ line, then reset it. */
    dev->acia.irq_out  = dtl_acia_irq_cb;
    dev->acia.userdata = dev;
    acia6850_init(&dev->acia);

    dev->line_connected = false;
    dev->symmetric      = false;       /* asymmetric V23 by default */
    dev->tx_busy        = false;
    dev->tx_cycles = dev->rx_cycles = 0;
    dev->tx_count = dev->rx_count = 0;

    dtl_recalc_timing(dev);
}

bool dtl2000_addr_in_range(const dtl2000_t* dev, uint16_t addr)
{
    return addr >= dev->base_addr &&
           addr < (uint16_t)(dev->base_addr + DTL2000_REG_SPAN);
}

uint8_t dtl2000_read(dtl2000_t* dev, uint16_t addr)
{
    uint16_t off = (uint16_t)(addr - dev->base_addr);

    switch (off) {
        case DTL_REG_PIA_A:
        case DTL_REG_PIA_CRA:
        case DTL_REG_PIA_B:
        case DTL_REG_PIA_CRB:
            /* PIA register offsets 0-3 map directly to RS1,RS0. */
            return pia6821_read(&dev->pia, (uint8_t)off);
        case DTL_REG_ACIA_CS:
            dtl_refresh_signals(dev);
            return acia6850_status(&dev->acia);
        case DTL_REG_ACIA_D: {
            /* Reading data clears RDRF (and recomputes IRQ); refresh DCD/CTS. */
            uint8_t v = acia6850_read_data(&dev->acia);
            dtl_refresh_signals(dev);
            return v;
        }
        default:
            return 0xFF;
    }
}

void dtl2000_write(dtl2000_t* dev, uint16_t addr, uint8_t value)
{
    uint16_t off = (uint16_t)(addr - dev->base_addr);

    switch (off) {
        case DTL_REG_PIA_A:
            pia6821_write(&dev->pia, PIA_RS_PRA, value);
            /* An ORA write (CRA bit2 = 1) may change the line/mode outputs. */
            if (dev->pia.cra & DTL_PIA_CR_DDR_SEL) {
                dtl_pia_update_outputs(dev);
            }
            break;
        case DTL_REG_PIA_CRA:
            pia6821_write(&dev->pia, PIA_RS_CRA, value);
            break;
        case DTL_REG_PIA_B:
            pia6821_write(&dev->pia, PIA_RS_PRB, value);
            break;
        case DTL_REG_PIA_CRB:
            pia6821_write(&dev->pia, PIA_RS_CRB, value);
            break;
        case DTL_REG_ACIA_CS:
            dtl_acia_control_write(dev, value);
            break;
        case DTL_REG_ACIA_D:
            dtl_acia_data_write(dev, value);
            break;
        default:
            break;
    }
}

void dtl2000_tick(dtl2000_t* dev, int cycles)
{
    if (!dev->backend) return;

    /* Receive: poll the backend at the configured baud cadence. */
    dev->rx_cycles -= cycles;
    if (dev->rx_cycles <= 0) {
        dev->rx_cycles += dev->rx_reload;
        if (dev->rx_cycles <= 0) dev->rx_cycles = dev->rx_reload;

        if (dev->line_connected && !acia6850_rdrf(&dev->acia)) {
            if (dev->backend->poll && dev->backend->poll(dev->backend)) {
                uint8_t b = 0;
                if (dev->backend->recv && dev->backend->recv(dev->backend, &b)) {
                    acia6850_rx_byte(&dev->acia, b);
                    dev->rx_count++;
                    dtl_trace_byte(dev, "RX", dev->acia.rdr, "recv");
                }
            }
        }
    }

    /* Transmit: release TDRE once the byte has shifted out. */
    if (dev->tx_busy) {
        dev->tx_cycles -= cycles;
        if (dev->tx_cycles <= 0) {
            dev->tx_busy = false;
            acia6850_tx_complete(&dev->acia);
        }
    }
}

void dtl2000_set_backend(dtl2000_t* dev, serial_backend_t* backend)
{
    dev->backend = backend;
    dtl_refresh_signals(dev);
}

void dtl2000_set_trace(dtl2000_t* dev, const char* filename)
{
    if (dev->trace) {
        fclose(dev->trace);
        dev->trace = NULL;
    }
    if (filename) {
        dev->trace = fopen(filename, "w");
        if (dev->trace) {
            fprintf(dev->trace,
                    "# Digitelec DTL 2000 — ACIA 6850 TX/RX trace (base $%04X)\n"
                    "# DIR  HEX  ASCII  STATUS  COUNTERS  NOTE\n",
                    dev->base_addr);
            fflush(dev->trace);
            log_info("DTL 2000 serial trace: %s", filename);
        } else {
            log_error("DTL 2000 serial trace: failed to open %s", filename);
        }
    }
}
