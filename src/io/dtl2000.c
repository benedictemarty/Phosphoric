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
    char c = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
    fprintf(dev->trace,
            "%-3s  %02X  '%c'  ST=%02X %s%s%s%s  TX=%u RX=%u  %s\n",
            dir, byte, c, dev->acia_status,
            (dev->acia_status & DTL_ACIA_SR_RDRF) ? "RDRF " : "",
            (dev->acia_status & DTL_ACIA_SR_TDRE) ? "TDRE " : "",
            (dev->acia_status & DTL_ACIA_SR_DCD)  ? "DCD "  : "",
            (dev->acia_status & DTL_ACIA_SR_CTS)  ? "CTS"   : "",
            dev->tx_count, dev->rx_count,
            note ? note : "");
}

/* Decode word-select (6850 bits 2-4) into data bits / parity / stop bits,
 * returning the total frame length and setting the data mask. */
static uint8_t dtl_frame_from_ws(uint8_t ws, uint8_t* bitmask_out)
{
    /* ws: 0=7E2 1=7O2 2=7E1 3=7O1 4=8N2 5=8N1 6=8E1 7=8O1 */
    uint8_t data   = (ws < 4) ? 7 : 8;
    uint8_t parity = (ws == 4 || ws == 5) ? 0 : 1;
    uint8_t stop   = (ws == 0 || ws == 1 || ws == 4) ? 2 : 1;
    if (bitmask_out) *bitmask_out = (data == 7) ? 0x7F : 0xFF;
    return (uint8_t)(1 + data + parity + stop);  /* + start bit */
}

/* Recompute baud rates and per-byte cycle reloads from the current mode
 * and frame format. */
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
    uint32_t fb = dev->framebits ? dev->framebits : 10;
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

    if (carrier) dev->acia_status &= (uint8_t)~DTL_ACIA_SR_DCD;
    else         dev->acia_status |= DTL_ACIA_SR_DCD;

    /* Clear to send once the line is up and carrier established. */
    if (carrier) dev->acia_status &= (uint8_t)~DTL_ACIA_SR_CTS;
    else         dev->acia_status |= DTL_ACIA_SR_CTS;
}

/* Recompute the IRQ status bit (and optionally drive the CPU line). */
static void dtl_update_irq(dtl2000_t* dev)
{
    bool irq = false;
    if ((dev->acia_control & DTL_ACIA_CR_RIE) &&
        (dev->acia_status & DTL_ACIA_SR_RDRF)) {
        irq = true;
    }
    uint8_t tc = (uint8_t)((dev->acia_control & DTL_ACIA_CR_TC_MASK) >> DTL_ACIA_CR_TC_SHIFT);
    if (tc == DTL_TC_RTS_LOW_TIE_ON && (dev->acia_status & DTL_ACIA_SR_TDRE)) {
        irq = true;
    }

    if (irq) dev->acia_status |= DTL_ACIA_SR_IRQ;
    else     dev->acia_status &= (uint8_t)~DTL_ACIA_SR_IRQ;

    if (irq && dev->irq_set)      dev->irq_set(dev->irq_userdata);
    else if (!irq && dev->irq_clr) dev->irq_clr(dev->irq_userdata);
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
    if ((value & DTL_ACIA_CR_CDS_MASK) == DTL_ACIA_CR_MASTER_RST) {
        /* Master reset: clear receiver/transmitter state, TDRE set. */
        dev->acia_control = value;
        dev->acia_status  = DTL_ACIA_SR_TDRE;
        dev->acia_rdr     = 0;
        dev->tx_busy      = false;
        dev->tx_cycles    = 0;
        dev->rx_cycles    = 0;
        dtl_refresh_signals(dev);
        dtl_update_irq(dev);
        return;
    }

    dev->acia_control = value;

    uint8_t ws = (uint8_t)((value & DTL_ACIA_CR_WS_MASK) >> DTL_ACIA_CR_WS_SHIFT);
    dev->framebits = dtl_frame_from_ws(ws, &dev->bitmask);

    uint8_t tc = (uint8_t)((value & DTL_ACIA_CR_TC_MASK) >> DTL_ACIA_CR_TC_SHIFT);
    /* Carrier emitted when /RTS is driven low — every TC value except the
     * "RTS high" code (10). */
    dev->tx_carrier = (tc != DTL_TC_RTS_HIGH_TIE_OFF);

    dtl_recalc_timing(dev);
    dtl_refresh_signals(dev);
    dtl_update_irq(dev);
}

/* Apply a write to the ACIA data register: queue a byte for transmission. */
static void dtl_acia_data_write(dtl2000_t* dev, uint8_t value)
{
    dev->acia_tdr = (uint8_t)(value & dev->bitmask);

    if (dev->line_connected && dev->tx_carrier &&
        dev->backend && dev->backend->send) {
        dev->backend->send(dev->backend, dev->acia_tdr);
        dev->tx_count++;
        dtl_trace_byte(dev, "TX", dev->acia_tdr, "sent");
    } else {
        dtl_trace_byte(dev, "TX", dev->acia_tdr,
                       dev->line_connected ? "dropped (no carrier)"
                                           : "dropped (line open)");
    }

    dev->acia_status &= (uint8_t)~DTL_ACIA_SR_TDRE;  /* transmitter busy */
    dev->tx_busy   = true;
    dev->tx_cycles = dev->tx_reload;
    dtl_update_irq(dev);
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

    dev->acia_control = 0;
    /* Power-on: transmitter empty, line open → carrier absent / not CTS. */
    dev->acia_status  = DTL_ACIA_SR_TDRE | DTL_ACIA_SR_DCD | DTL_ACIA_SR_CTS;
    dev->acia_rdr = 0;
    dev->acia_tdr = 0;

    dev->line_connected = false;
    dev->symmetric      = false;       /* asymmetric V23 by default */
    dev->tx_carrier     = false;
    dev->bitmask        = 0x7F;        /* 7-bit until configured */
    dev->framebits      = 10;
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
            return dev->acia_status;
        case DTL_REG_ACIA_D: {
            uint8_t v = dev->acia_rdr;
            /* Reading data clears RDRF; the blank read also re-arms the DCD
             * latch per the manual. */
            dev->acia_status &= (uint8_t)~DTL_ACIA_SR_RDRF;
            dtl_refresh_signals(dev);
            dtl_update_irq(dev);
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

        if (dev->line_connected && !(dev->acia_status & DTL_ACIA_SR_RDRF)) {
            if (dev->backend->poll && dev->backend->poll(dev->backend)) {
                uint8_t b = 0;
                if (dev->backend->recv && dev->backend->recv(dev->backend, &b)) {
                    dev->acia_rdr = (uint8_t)(b & dev->bitmask);
                    dev->acia_status |= DTL_ACIA_SR_RDRF;
                    dev->rx_count++;
                    dtl_trace_byte(dev, "RX", dev->acia_rdr, "recv");
                    dtl_update_irq(dev);
                }
            }
        }
    }

    /* Transmit: release TDRE once the byte has shifted out. */
    if (dev->tx_busy) {
        dev->tx_cycles -= cycles;
        if (dev->tx_cycles <= 0) {
            dev->tx_busy = false;
            dev->acia_status |= DTL_ACIA_SR_TDRE;
            dtl_update_irq(dev);
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
