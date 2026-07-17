/**
 * @file mageco.c
 * @brief Mageco MIDI interface — MC6850 ACIA at $03FE-$03FF
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 *
 * See mageco.h for the register map and the MIDI line semantics. This module is
 * deliberately thin: the MC6850 register/IRQ logic lives in acia6850.c, the byte
 * transport in the serial backend; here we wire the two together, pin the modem
 * inputs to "no handshake", and run both directions at the fixed MIDI 31250 baud.
 */

#include "io/mageco.h"
#include "io/serial_backend.h"
#include "utils/logging.h"

#include <string.h>

/* ───────────────────────────────────────────────────────────────────────
 *  Internal helpers
 * ─────────────────────────────────────────────────────────────────────── */

/* Emit one TX/RX line to the trace file (if enabled). */
static void mageco_trace_byte(mageco_t* dev, const char* dir, uint8_t byte,
                              const char* note)
{
    if (!dev->trace) return;
    uint8_t st = dev->acia.status;
    char c = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
    fprintf(dev->trace,
            "%-3s  %02X  '%c'  ST=%02X %s%s  TX=%u RX=%u  %s\n",
            dir, byte, c, st,
            (st & ACIA6850_SR_RDRF) ? "RDRF " : "",
            (st & ACIA6850_SR_TDRE) ? "TDRE " : "",
            dev->tx_count, dev->rx_count,
            note ? note : "");
}

/* Recompute the per-byte cycle reloads from the ACIA frame format at the fixed
 * MIDI baud (31250). MIDI is 8-N-1 → 10 bits → 320 cycles/byte at 1 MHz. */
static void mageco_recalc_timing(mageco_t* dev)
{
    uint32_t fb = dev->acia.framebits ? dev->acia.framebits : 10;
    int32_t reload = (int32_t)((uint64_t)MAGECO_CLOCK_HZ * fb / MAGECO_MIDI_BAUD);
    if (reload < 1) reload = 1;
    dev->rx_reload = reload;
    dev->tx_reload = reload;
}

/* The ACIA's IRQ output (called by acia6850_*) routed to the CPU /IRQ line. */
static void mageco_acia_irq_cb(void* ud, bool active)
{
    mageco_t* dev = (mageco_t*)ud;
    if (active && dev->irq_set)       dev->irq_set(dev->irq_userdata);
    else if (!active && dev->irq_clr) dev->irq_clr(dev->irq_userdata);
}

/* Apply a write to the ACIA data register: put a byte on the MIDI OUT wire.
 * MIDI has no carrier/handshake — the transmitter always sends the written
 * byte (RTS is not part of the MIDI Tx path). */
static void mageco_acia_data_write(mageco_t* dev, uint8_t value)
{
    acia6850_write_data(&dev->acia, value);
    uint8_t tdr = dev->acia.tdr;

    if (dev->backend && dev->backend->send) {
        dev->backend->send(dev->backend, tdr);
        dev->tx_count++;
        mageco_trace_byte(dev, "TX", tdr, "sent");
    } else {
        mageco_trace_byte(dev, "TX", tdr, "dropped (no backend)");
    }

    dev->tx_busy   = true;
    dev->tx_cycles = dev->tx_reload;
}

/* ───────────────────────────────────────────────────────────────────────
 *  Public API
 * ─────────────────────────────────────────────────────────────────────── */

void mageco_init(mageco_t* dev, uint16_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr ? base_addr : MAGECO_DEFAULT_BASE;
    dev->span      = MAGECO_REG_SPAN;
    dev->oricon    = false;
    mageco_reset(dev);
}

void mageco_init_oricon(mageco_t* dev, uint16_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr ? base_addr : MAGECO_ORICON_BASE;
    dev->span      = MAGECO_ORICON_SPAN;
    dev->oricon    = true;
    mageco_reset(dev);
}

void mageco_reset(mageco_t* dev)
{
    /* Wire the ACIA's IRQ output to the CPU /IRQ line, then reset it. */
    dev->acia.irq_out  = mageco_acia_irq_cb;
    dev->acia.userdata = dev;
    acia6850_init(&dev->acia);

    /* MIDI has no modem handshake: pin DCD "carrier present" and CTS "clear to
     * send" so status polling never blocks on a non-existent line. */
    acia6850_set_dcd(&dev->acia, true);
    acia6850_set_cts(&dev->acia, true);

    dev->tx_busy   = false;
    dev->tx_cycles = dev->rx_cycles = 0;
    dev->tx_count  = dev->rx_count  = 0;
    dev->clkgen[0] = dev->clkgen[1] = 0;

    mageco_recalc_timing(dev);
}

bool mageco_addr_in_range(const mageco_t* dev, uint16_t addr)
{
    uint8_t span = dev->span ? dev->span : MAGECO_REG_SPAN;
    return addr >= dev->base_addr &&
           addr < (uint16_t)(dev->base_addr + span);
}

/* ── Savestate (Epic 7 / US4) ───────────────────────────────────────────────
 * État émulé en blob (même-build, garde par taille) ; transport hôte (backend,
 * trace, callbacks) préservé depuis l'instance vivante — cf. dtl2000_save. */
bool mageco_save(const mageco_t* dev, FILE* fp)
{
    return fwrite(dev, sizeof(*dev), 1, fp) == 1;
}

void mageco_load(mageco_t* dev, FILE* fp, uint32_t size)
{
    if (size != sizeof(*dev))
        return;
    mageco_t keep = *dev;
    if (fread(dev, sizeof(*dev), 1, fp) != 1) {
        *dev = keep;
        return;
    }
    dev->backend      = keep.backend;
    dev->trace        = keep.trace;
    dev->irq_set      = keep.irq_set;
    dev->irq_clr      = keep.irq_clr;
    dev->irq_userdata = keep.irq_userdata;
    dev->acia.irq_out  = keep.acia.irq_out;
    dev->acia.userdata = keep.acia.userdata;
}

uint8_t mageco_read(mageco_t* dev, uint16_t addr)
{
    uint16_t off = (uint16_t)(addr - dev->base_addr);

    switch (off) {
        case MAGECO_REG_ACIA_CS:
            return acia6850_status(&dev->acia);
        case MAGECO_REG_ACIA_D:
            return acia6850_read_data(&dev->acia);
        case MAGECO_REG_CLKGEN_LO:
            return dev->oricon ? dev->clkgen[0] : 0xFF;  /* ORICON $31E */
        case MAGECO_REG_CLKGEN_HI:
            return dev->oricon ? dev->clkgen[1] : 0xFF;  /* ORICON $31F */
        default:
            return 0xFF;
    }
}

void mageco_write(mageco_t* dev, uint16_t addr, uint8_t value)
{
    uint16_t off = (uint16_t)(addr - dev->base_addr);

    switch (off) {
        case MAGECO_REG_ACIA_CS: {
            bool master_reset = acia6850_control_write(&dev->acia, value);
            if (master_reset) {
                dev->tx_busy   = false;
                dev->tx_cycles = 0;
                dev->rx_cycles = 0;
            }
            mageco_recalc_timing(dev);   /* frame format may have changed */
            break;
        }
        case MAGECO_REG_ACIA_D:
            mageco_acia_data_write(dev, value);
            break;
        case MAGECO_REG_CLKGEN_LO:
            if (dev->oricon) dev->clkgen[0] = value;  /* latch (baud stays MIDI) */
            break;
        case MAGECO_REG_CLKGEN_HI:
            if (dev->oricon) dev->clkgen[1] = value;
            break;
        default:
            break;
    }
}

void mageco_tick(mageco_t* dev, int cycles)
{
    if (!dev->backend) return;

    /* Receive: poll the MIDI IN backend at the 31250-baud cadence. */
    dev->rx_cycles -= cycles;
    if (dev->rx_cycles <= 0) {
        dev->rx_cycles += dev->rx_reload;
        if (dev->rx_cycles <= 0) dev->rx_cycles = dev->rx_reload;

        if (!acia6850_rdrf(&dev->acia)) {
            if (dev->backend->poll && dev->backend->poll(dev->backend)) {
                uint8_t b = 0;
                if (dev->backend->recv && dev->backend->recv(dev->backend, &b)) {
                    acia6850_rx_byte(&dev->acia, b);
                    dev->rx_count++;
                    mageco_trace_byte(dev, "RX", dev->acia.rdr, "recv");
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

void mageco_set_backend(mageco_t* dev, serial_backend_t* backend)
{
    dev->backend = backend;
}

void mageco_set_trace(mageco_t* dev, const char* filename)
{
    if (dev->trace) {
        fclose(dev->trace);
        dev->trace = NULL;
    }
    if (filename) {
        dev->trace = fopen(filename, "w");
        if (dev->trace) {
            fprintf(dev->trace,
                    "# Mageco MIDI — ACIA 6850 TX/RX trace (base $%04X, 31250 baud)\n"
                    "# DIR  HEX  ASCII  STATUS  COUNTERS  NOTE\n",
                    dev->base_addr);
            fflush(dev->trace);
            log_info("Mageco MIDI trace: %s", filename);
        } else {
            log_error("Mageco MIDI trace: failed to open %s", filename);
        }
    }
}
