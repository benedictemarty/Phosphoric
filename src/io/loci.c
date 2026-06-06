/**
 * @file loci.c
 * @brief LOCI emulation — skeleton + dispatcher (Sprint 34y)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Phase 1 implementation: MIA register file + API op dispatcher.
 * All API operations return LOCI_ENOSYS synchronously — subsequent sprints
 * implement the real handlers.
 *
 * Bus address layout (real LOCI hardware, extracted from sodiumlb/loci-firmware):
 *
 *   $03A0  CONS_FLAGS    bit 7 = console data avail, bit 6 = VSYNC ack
 *   $03A2  CONS_CHAR     console input character latch
 *   $03A4  RW0           DMA window 0 data byte
 *   $03A5  STEP0         DMA window 0 increment (signed)
 *   $03A8  RW1           DMA window 1 data byte
 *   $03A9  STEP1         DMA window 1 increment (signed)
 *   $03AC  API_STACK     xstack pointer (writes pop, reads push)
 *   $03AD  API_ERRNO_LO  16-bit errno
 *   $03AE  API_ERRNO_HI
 *   $03AF  API_OP        write triggers dispatch
 *   $03B0..$03B9         6502 injection slot (used by MIA for RAM bursts)
 *   $03B2.7              BUSY flag
 *   $03B4  API_A         return A
 *   $03B6  API_X         return X
 *   $03B8  API_SREG_LO   return SREG lo
 *   $03B9  API_SREG_HI   return SREG hi
 */

#include "io/loci.h"
#include "utils/logging.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

/* ─── name lookup for diagnostics ───────────────────────────────── */

static const char* op_name(uint8_t op) {
    switch (op) {
        case LOCI_OP_PIX_XREG:        return "PIX_XREG";
        case LOCI_OP_CPU_PHI2:        return "CPU_PHI2";
        case LOCI_OP_OEM_CODEPAGE:    return "OEM_CODEPAGE";
        case LOCI_OP_RNG_LRAND:       return "RNG_LRAND";
        case LOCI_OP_STDIN_OPT:       return "STDIN_OPT";
        case LOCI_OP_CLOCK:           return "CLOCK";
        case LOCI_OP_CLK_GETRES:      return "CLK_GETRES";
        case LOCI_OP_CLK_GETTIME:     return "CLK_GETTIME";
        case LOCI_OP_CLK_SETTIME:     return "CLK_SETTIME";
        case LOCI_OP_OPEN:            return "OPEN";
        case LOCI_OP_CLOSE:           return "CLOSE";
        case LOCI_OP_READ_XSTACK:     return "READ_XSTACK";
        case LOCI_OP_READ_XRAM:       return "READ_XRAM";
        case LOCI_OP_WRITE_XSTACK:    return "WRITE_XSTACK";
        case LOCI_OP_WRITE_XRAM:      return "WRITE_XRAM";
        case LOCI_OP_LSEEK:           return "LSEEK";
        case LOCI_OP_UNLINK:          return "UNLINK";
        case LOCI_OP_RENAME:          return "RENAME";
        case LOCI_OP_OPENDIR:         return "OPENDIR";
        case LOCI_OP_CLOSEDIR:        return "CLOSEDIR";
        case LOCI_OP_READDIR:         return "READDIR";
        case LOCI_OP_MKDIR:           return "MKDIR";
        case LOCI_OP_GETCWD:          return "GETCWD";
        case LOCI_OP_MOUNT:           return "MOUNT";
        case LOCI_OP_UMOUNT:          return "UMOUNT";
        case LOCI_OP_TAP_SEEK:        return "TAP_SEEK";
        case LOCI_OP_TAP_TELL:        return "TAP_TELL";
        case LOCI_OP_TAP_READ_HEADER: return "TAP_READ_HEADER";
        case LOCI_OP_UNAME:           return "UNAME";
        case LOCI_OP_MIA_BOOT:        return "MIA_BOOT";
        case LOCI_OP_MAP_TUNE_TMAP:   return "MAP_TUNE_TMAP";
        case LOCI_OP_MAP_TUNE_TIOR:   return "MAP_TUNE_TIOR";
        case LOCI_OP_MAP_TUNE_TIOW:   return "MAP_TUNE_TIOW";
        case LOCI_OP_MAP_TUNE_TIOD:   return "MAP_TUNE_TIOD";
        case LOCI_OP_MAP_TUNE_TADR:   return "MAP_TUNE_TADR";
        default:                      return "?";
    }
}

/* ─── clock helpers ────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ─── lifecycle ────────────────────────────────────────────────── */

bool loci_init(loci_t* loci) {
    if (!loci) return false;
    memset(loci, 0, sizeof(*loci));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;   /* empty */
    loci->clock_start_us = now_us();
    loci->rng_state = loci->clock_start_us ^ 0xA5A5A5A5A5A5A5A5ULL;
    return true;
}

void loci_reset(loci_t* loci) {
    if (!loci || !loci->enabled) return;
    memset(loci->regs, 0, sizeof(loci->regs));
    memset(loci->xstack, 0, sizeof(loci->xstack));
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    loci->active_op = 0;
    loci->clock_start_us = now_us();
}

void loci_cleanup(loci_t* loci) {
    if (!loci) return;
    /* Sprint 34y: no external resources to free. */
    (void)loci;
}

/* ─── errno / BUSY / xstack helpers ────────────────────────────── */

static void set_errno(loci_t* loci, uint16_t e) {
    loci->regs[LOCI_REG_API_ERRNO_LO] = (uint8_t)(e & 0xFF);
    loci->regs[LOCI_REG_API_ERRNO_HI] = (uint8_t)(e >> 8);
}

static void set_busy(loci_t* loci, bool busy) {
    if (busy) loci->regs[LOCI_REG_BUSY] |=  0x80;
    else      loci->regs[LOCI_REG_BUSY] &= ~0x80;
}

/* Mirror the top byte of xstack into $03AC so the 6502 sees it on read.
 * Called after every xstack mutation. */
static void xstack_sync(loci_t* loci) {
    if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) {
        loci->regs[LOCI_REG_API_STACK] = 0;
    } else {
        loci->regs[LOCI_REG_API_STACK] = loci->xstack[loci->xstack_ptr];
    }
}

/* Reset the xstack to empty. */
static void xstack_zero(loci_t* loci) {
    loci->xstack_ptr = LOCI_XSTACK_SIZE;
    xstack_sync(loci);
}

/* Push N bytes onto the xstack. Returns false if not enough room. */
static bool xstack_push_n(loci_t* loci, const void* data, size_t n) {
    if (n > loci->xstack_ptr) return false;
    loci->xstack_ptr -= (uint16_t)n;
    memcpy(&loci->xstack[loci->xstack_ptr], data, n);
    xstack_sync(loci);
    return true;
}

static bool xstack_push_u32(loci_t* loci, uint32_t v) {
    return xstack_push_n(loci, &v, 4);
}

static bool xstack_push_i32(loci_t* loci, int32_t v) {
    return xstack_push_n(loci, &v, 4);
}

/* ─── API return helpers (mirror firmware semantics) ──────────── */

static void api_set_ax(loci_t* loci, uint16_t val) {
    loci->regs[LOCI_REG_API_A] = (uint8_t)(val & 0xFF);
    loci->regs[LOCI_REG_API_X] = (uint8_t)((val >> 8) & 0xFF);
}

static void api_set_axsreg(loci_t* loci, uint32_t val) {
    api_set_ax(loci, (uint16_t)val);
    loci->regs[LOCI_REG_API_SREG]    = (uint8_t)((val >> 16) & 0xFF);
    loci->regs[LOCI_REG_API_SREG_HI] = (uint8_t)((val >> 24) & 0xFF);
}

static void api_return_ax(loci_t* loci, uint16_t val) {
    api_set_ax(loci, val);
    set_busy(loci, false);
}

static void api_return_axsreg(loci_t* loci, uint32_t val) {
    api_set_axsreg(loci, val);
    set_busy(loci, false);
}

static void api_return_errno(loci_t* loci, uint16_t e) {
    xstack_zero(loci);
    set_errno(loci, e);
    api_return_axsreg(loci, 0xFFFFFFFFu);
}

/* ─── API handlers (Sprint 34z: system / RTC / RNG) ──────────── */

/* 0x01 PIX_XREG — forwards a 24-bit "channel:addr:word" packet to the PIX
 * (USB HID) subsystem. Sprint 34z accepts it silently with ax=0; Sprint
 * 34ag will route to SDL kbd/mouse/pad bridges. */
static void op_pix_xreg(loci_t* loci) {
    api_return_ax(loci, 0);
}

/* 0x04 RNG_LRAND — returns a 31-bit positive random uint32 in AXSREG. */
static void op_rng_lrand(loci_t* loci) {
    /* Use rand() seeded once at init time for variety. The firmware uses
     * the Pi Pico's hardware RNG; deterministic-mode hook reserved for
     * later. */
    uint32_t v = (uint32_t)rand();
    v ^= (uint32_t)(rand() << 16);
    v &= 0x7FFFFFFFu;
    api_return_axsreg(loci, v);
}

/* 0x0F CLOCK — uptime in 10 ms units (firmware: us_64 / 10000). */
static void op_clock(loci_t* loci) {
    uint64_t now = now_us();
    uint64_t up  = (now >= loci->clock_start_us)
                       ? (now - loci->clock_start_us)
                       : 0;
    api_return_axsreg(loci, (uint32_t)(up / 10000ULL));
}

#define CLK_ID_REALTIME 0

/* 0x10 CLK_GETRES — push (uint32 sec=1, int32 nsec=0); return ax=0. */
static void op_clk_getres(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    int32_t nsec = 0;
    uint32_t sec = 1;
    if (!xstack_push_i32(loci, nsec) || !xstack_push_u32(loci, sec)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x11 CLK_GETTIME — push (uint32 rawtime, int32 nsec=0); return ax=0. */
static void op_clk_gettime(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    time_t t = time(NULL);
    int32_t nsec = 0;
    if (!xstack_push_i32(loci, nsec) ||
        !xstack_push_u32(loci, (uint32_t)t)) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    api_return_ax(loci, 0);
}

/* 0x12 CLK_SETTIME — pop (uint32 rawtime, int32 nsec); ack ax=0
 * (we don't actually retune the host clock — would need privileges). */
static void op_clk_settime(loci_t* loci) {
    uint8_t clock_id = loci->regs[LOCI_REG_API_A];
    if (clock_id != CLK_ID_REALTIME) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    /* Drain four bytes (rawtime u32) + four bytes (nsec i32) from xstack.
     * We don't actually use them. */
    if (loci->xstack_ptr + 8 > LOCI_XSTACK_SIZE) {
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
    loci->xstack_ptr += 8;
    xstack_sync(loci);
    api_return_ax(loci, 0);
}

/* ─── dispatch ─────────────────────────────────────────────────── */

static void dispatch_op(loci_t* loci, uint8_t op) {
    loci->op_count[op]++;
    loci->active_op = op;
    switch (op) {
        case LOCI_OP_PIX_XREG:    op_pix_xreg(loci);     break;
        case LOCI_OP_RNG_LRAND:   op_rng_lrand(loci);    break;
        case LOCI_OP_CLOCK:       op_clock(loci);        break;
        case LOCI_OP_CLK_GETRES:  op_clk_getres(loci);   break;
        case LOCI_OP_CLK_GETTIME: op_clk_gettime(loci);  break;
        case LOCI_OP_CLK_SETTIME: op_clk_settime(loci);  break;
        default:
            log_debug("LOCI op $%02X (%s) — stubbed, returns ENOSYS",
                      op, op_name(op));
            set_errno(loci, LOCI_ENOSYS);
            set_busy(loci, false);
            break;
    }
    loci->active_op = 0;
}

/* ─── bus interface ────────────────────────────────────────────── */

uint8_t loci_read(loci_t* loci, uint16_t address) {
    if (!loci || !loci->enabled) return 0xFF;
    if (!loci_addr_in_mia(address)) return 0xFF;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* API_STACK: a 6502 read pops the top byte. Firmware semantics:
     *   value = xstack[ptr]; ptr++ (clamped to LOCI_XSTACK_SIZE);
     *   regs[STACK] = next top byte (or 0 if empty).
     * This consumes whatever LOCI pushed (e.g. clock_gettime data). */
    if (off == LOCI_REG_API_STACK) {
        uint8_t v;
        if (loci->xstack_ptr >= LOCI_XSTACK_SIZE) {
            v = 0;
        } else {
            v = loci->xstack[loci->xstack_ptr];
            loci->xstack_ptr++;
        }
        xstack_sync(loci);
        return v;
    }

    return loci->regs[off];
}

void loci_write(loci_t* loci, uint16_t address, uint8_t value) {
    if (!loci || !loci->enabled) return;
    if (!loci_addr_in_mia(address)) return;
    uint8_t off = (uint8_t)(address - LOCI_MIA_BASE);

    /* API_STACK: 6502 writes push onto xstack (grows downward). */
    if (off == LOCI_REG_API_STACK) {
        if (loci->xstack_ptr > 0) {
            loci->xstack[--loci->xstack_ptr] = value;
        }
        xstack_sync(loci);
        return;
    }

    loci->regs[off] = value;

    /* API_OP: writing here triggers a dispatch. The firmware sets BUSY
     * before the write so the polling Oric code waits for completion. */
    if (off == LOCI_REG_API_OP) {
        if (value != LOCI_OP_NONE && value != LOCI_OP_RESET_SENTINEL) {
            set_busy(loci, true);
            dispatch_op(loci, value);
        }
    }
}

void loci_task(loci_t* loci) {
    /* Sprint 34y: dispatch is synchronous in loci_write. No async work. */
    (void)loci;
}
